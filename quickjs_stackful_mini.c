/**
 * quickjs_stackful_mini.c
 * QuickJS Stackful Coroutine Implementation (based on Tina)
 * 
 * Migration: Minicoro → Tina (2025-11-01)
 * Migration: Symmetric → Asymmetric coroutines (2025-11-04)
 * 
 * Asymmetric coroutine model (aligned with ltask Lua coroutines):
 * - resume: always from dispatcher to target coroutine
 * - yield: always from coroutine back to dispatcher
 * - S->running is managed internally, external code doesn't touch it
 */

#define TINA_IMPLEMENTATION
#include "quickjs_stackful_mini.h"

// Optional: coroutine tracing (jtask-specific debug feature)
#ifdef ENABLE_CORO_TRACE
#include "coroutine_trace.h"
#else
// Stub macros when tracing is disabled
#define CORO_TRACE_CREATE(id, caller, count) ((void)0)
#define CORO_TRACE_RESUME(from, to, from_real, to_real) ((void)0)
#define CORO_TRACE_RESUME_RET(from, to, to_real, from_real, status) ((void)0)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_MSC_VER) && !defined(__EMSCRIPTEN__)
#include <execinfo.h>  // For backtrace
#endif

#ifdef __EMSCRIPTEN__
extern uintptr_t emscripten_stack_get_current(void);
#endif

#define DEFAULT_COROUTINE 16
#define TINA_DEFAULT_STACK_SIZE (1024*1024)  /* 1MB, safe for modern systems */

/* Debug flag - set to 0 for production */
#define STACKFUL_DEBUG 0

#if STACKFUL_DEBUG
#define DEBUG_LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_LOG(...) ((void)0)
#endif

/* Thread-local storage for current coroutine (replaces S->running) */
#ifdef _MSC_VER
    #define THREAD_LOCAL __declspec(thread)
#else
    #define THREAD_LOCAL _Thread_local
#endif

/* native 后端:OS thread 内"当前 coro"用 thread-local 跟踪 (single-threaded
 * fiber 语义).wasm/JSPI 后端不用这个 — 见下面的 active_stack. */
static THREAD_LOCAL tina* tl_current_coro = NULL;

/* ============================================================================
 * WASM/JSPI 路径封存中（2026-04-30）
 *
 * 根因(已诊断,未解决):QuickJS JS_CallInternal interpreter 主循环的局部
 * 变量副本(emcc 编译为 wasm shadow stack 槽 / wasm local)跨 V8 promising
 * stack 切栈后值失效.详见 docs/handoff_jspi_integration_2026_04_30.md
 * "2026-04-30 调查总结".
 *
 * 本段 wasm/JSPI 代码保留作为"已修对的基础设施 + 已验证无效的修法"双重
 * 物证.下次重启 wasm 路径前先读 docs/plan_tina_wasm_contract.md §6 暂停
 * 决定.
 *
 * 已走过的 4 条死路（按时间顺序）:
 *   1. shadow stack 物理覆盖
 *   2. QuickJS argv 零拷贝指向 caller frame
 *   3. JSStackFrame 字段被踩
 *   4. JS_CallInternal C 局部缓存(真根因,stackful_mini 层无法干预)
 *
 * 已验证状态分类:
 *   ✅ sf_active_stack[] / sf_active_push/pop/top_ptr / wasm 分支的
 *      stackful_resume[_with_value]/stackful_yield* 改写 — 解决 L3 嵌套
 *      caller 链,与最终 hang 成因无关,wasm 路径必须的基础设施.
 *   ✅ stackful_resume 入口 self-resume fail-fast abort — 下次重启 wasm
 *      时 self-resume 立即 backtrace,有用,保留.
 *   🔍 [sf PRE/POST] / [stackful_resume CALLER] / [yield_with_value PRE/POST]
 *      等 fprintf 诊断 trace — 保留代码作为重启时直接打开的诊断点.
 *
 * 禁止:在不读 hand-off 调查总结的情况下尝试新一轮诊断或修复.
 * ============================================================================ */

#ifdef __EMSCRIPTEN__
/* wasm/JSPI: per-worker-thread active coro stack.
 *
 * 设计:同一个 worker thread 内可能有多条挂起的 promising stack (dispatcher
 * + 嵌套 child coro).每条 promising stack 跑 wasm 时,我们需要知道"当前在
 * 哪个 coro 的 user body 内" — 以便 yield 把值送到正确的 mailbox.
 *
 * native 用 OS thread-local (tl_current_coro) 存它,但 wasm/JSPI 下多 promising
 * stack 共享 thread-local,会跨着踩.改用一个普通 thread-local 数组 +
 * 计数器构成的栈:
 *   - stackful_resume[_with_value] 调 tina_wasm_resume 之前 push target,
 *     之后 pop.
 *   - stackful_yield[_with_value/_with_flag] 读栈顶拿 self,调 tina_wasm_yield.
 *   - stackful_running 读栈顶 self_id.
 *
 * 关键:push/pop 永远成对出现在同一条 promising stack 的 sync 调用链内
 * (push 在 resume 调用前,pop 在 resume 返回后).嵌套 resume 时 inner stack
 * 也是同样的 push→tina_wasm_resume→pop 模式,inner pop 完才轮到 outer pop.
 * V8 在 tina_wasm_resume 内部 await 切栈,但 await 之前的 push 已经完成,
 * await 醒来时栈顶仍是该 coro 的 wrapper.
 *
 * 32 层够用:嵌套 resume 在 jtask 里不会超过 wakeup_session 的 inner loop
 * 深度 (实际只有 1-2 层).
 */
#define STACKFUL_ACTIVE_STACK_MAX 32
static THREAD_LOCAL tina_wrapper* sf_active_stack[STACKFUL_ACTIVE_STACK_MAX];
static THREAD_LOCAL int           sf_active_top = 0;  /* 0 = 空; top index = sf_active_top - 1 */

static inline void sf_active_push(tina_wrapper *w) {
    if (sf_active_top >= STACKFUL_ACTIVE_STACK_MAX) {
        fprintf(stderr, "[stackful/wasm] ERROR: active stack overflow (>%d)\n",
                STACKFUL_ACTIVE_STACK_MAX);
        abort();
    }
    sf_active_stack[sf_active_top++] = w;
}
static inline void sf_active_pop(void) {
    if (sf_active_top <= 0) {
        fprintf(stderr, "[stackful/wasm] ERROR: active stack underflow\n");
        abort();
    }
    sf_active_stack[--sf_active_top] = NULL;
}
static inline tina_wrapper* sf_active_top_ptr(void) {
    return (sf_active_top > 0) ? sf_active_stack[sf_active_top - 1] : NULL;
}
#endif

/* ========== Data storage functions ========== */

static int tina_storage_push(tina_storage *s, const void *data, size_t len) {
    if (s->size + len > sizeof(s->buffer)) {
        DEBUG_LOG("[tina_storage_push] ERROR: Buffer full (size=%zu, len=%zu, max=%zu)\n",
                s->size, len, sizeof(s->buffer));
        return -1;
    }
    memcpy(s->buffer + s->size, data, len);
    s->size += len;
    return 0;
}

static int tina_storage_pop(tina_storage *s, void *data, size_t len) {
    if (s->size < len) {
        DEBUG_LOG("[tina_storage_pop] ERROR: Not enough data (size=%zu, len=%zu)\n",
                s->size, len);
        return -1;
    }
    s->size -= len;
    memcpy(data, s->buffer + s->size, len);
    return 0;
}

static size_t tina_storage_bytes(const tina_storage *s) {
    return s->size;
}

static void tina_storage_clear(tina_storage *s) {
    s->size = 0;
}

/* ========== Tina entry wrapper ========== */

static void* tina_entry_wrapper(tina *coro, void *value) {
    tina_wrapper *wrapper = (tina_wrapper*)coro->user_data;

    DEBUG_LOG("[tina_entry_wrapper] Coroutine starting, value=%p\n", value);

    /* Call the original function with user_data AND the resume value */
    wrapper->user_func(wrapper->user_data, value);

    DEBUG_LOG("[tina_entry_wrapper] Coroutine finished\n");

    /* Asymmetric coroutine: simply return, Tina will automatically:
     * 1. Set coro->completed = true
     * 2. Return to the caller (dispatcher)
     */
    return NULL;
}

/* ========== Scheduler implementation ========== */

stackful_schedule* stackful_open(JSRuntime *rt, JSContext *main_ctx) {
    stackful_schedule *S = malloc(sizeof(*S));
    if (!S) {
        DEBUG_LOG( "[stackful_open] ERROR: Failed to allocate scheduler\n");
        return NULL;
    }

    S->rt = rt;
    S->main_ctx = main_ctx;
    S->cap = DEFAULT_COROUTINE;
    S->count = 0;

    /* Initialize dispatcher (empty coroutine for symmetric mode) */
    S->dispatcher = TINA_EMPTY;  /* Copy TINA_EMPTY including canary values */

    S->coroutines = calloc(S->cap, sizeof(tina_wrapper*));
    S->storages = calloc(S->cap, sizeof(tina_storage));

    if (!S->coroutines || !S->storages) {
        DEBUG_LOG( "[stackful_open] ERROR: Failed to allocate arrays\n");
        free(S->coroutines);
        free(S->storages);
        free(S);
        return NULL;
    }

    DEBUG_LOG( "[stackful_open] Scheduler created with symmetric coroutines (cap=%d)\n", S->cap);
    return S;
}

void stackful_close(stackful_schedule *S) {
    if (!S) return;
    
    DEBUG_LOG( "[stackful_close] Destroying scheduler\n");
    
    for (int i = 0; i < S->cap; i++) {
        if (S->coroutines[i]) {
            tina_wrapper *wrapper = S->coroutines[i];
            if (wrapper->coro) {
                /* Tina's coro pointer points INTO buffer, so only free buffer */
                if (wrapper->coro->buffer) {
                    free(wrapper->coro->buffer);
                }
                /* Don't free wrapper->coro itself - it's inside the buffer! */
            }
            free(wrapper);
        }
    }
    
    free(S->coroutines);
    free(S->storages);
    free(S);
    
    DEBUG_LOG( "[stackful_close] Scheduler destroyed\n");
}

int stackful_new(stackful_schedule *S, stackful_func func, void *ud) {
    if (!S || !func) {
        DEBUG_LOG( "[stackful_new] ERROR: Invalid arguments\n");
        return -1;
    }
    
    /* Find empty slot */
    int id = -1;
    for (int i = 0; i < S->cap; i++) {
        if (S->coroutines[i] == NULL) {
            id = i;
            break;
        }
    }
    
    /* Expand if needed */
    if (id == -1) {
        id = S->cap;
        int new_cap = S->cap * 2;
        
        tina_wrapper **new_coros = realloc(S->coroutines, new_cap * sizeof(tina_wrapper*));
        tina_storage *new_storages = realloc(S->storages, new_cap * sizeof(tina_storage));
        
        if (!new_coros || !new_storages) {
            DEBUG_LOG( "[stackful_new] ERROR: Failed to expand capacity\n");
            return -1;
        }
        
        S->coroutines = new_coros;
        S->storages = new_storages;
        
        /* Zero new entries */
        memset(S->coroutines + S->cap, 0, (new_cap - S->cap) * sizeof(tina_wrapper*));
        memset(S->storages + S->cap, 0, (new_cap - S->cap) * sizeof(tina_storage));
        
        S->cap = new_cap;
        
        DEBUG_LOG( "[stackful_new] Expanded capacity to %d\n", new_cap);
    }
    
    /* Create wrapper */
    tina_wrapper *wrapper = malloc(sizeof(tina_wrapper));
    if (!wrapper) {
        DEBUG_LOG("[stackful_new] ERROR: Failed to allocate wrapper\n");
        return -1;
    }
    
    wrapper->user_data = ud;
    wrapper->user_func = func;
    wrapper->status = STACKFUL_STATUS_SUSPENDED;
    wrapper->yield_count = 0;
    wrapper->self_id = id;  /* Store self ID for asymmetric yield */
#ifdef __EMSCRIPTEN__
    wrapper->launched = 0;  /* wasm/JSPI: 推迟到首次 stackful_resume */
#endif

    /* Create Tina coroutine */
    wrapper->coro = tina_init(NULL, TINA_DEFAULT_STACK_SIZE, tina_entry_wrapper, wrapper);
    
    if (!wrapper->coro) {
        DEBUG_LOG( "[stackful_new] ERROR: tina_init failed\n");
        free(wrapper);
        return -1;
    }
    
    S->coroutines[id] = wrapper;
    S->count++;
    
    DEBUG_LOG( "[stackful_new] Created coroutine id=%d (count=%d)\n", id, S->count);
    CORO_TRACE_CREATE(id, -1, S->count);
    
    return id;
}

int stackful_resume(stackful_schedule *S, int id) {
    if (!S || id < 0 || id >= S->cap || !S->coroutines[id]) {
        DEBUG_LOG("[stackful_resume] ERROR: Invalid coroutine id=%d\n", id);
        return -1;
    }

    tina_wrapper *target = S->coroutines[id];

    if (target->status == STACKFUL_STATUS_DEAD) {
        DEBUG_LOG("[stackful_resume] ERROR: Coroutine id=%d already dead\n", id);
        return -1;
    }

#ifdef __EMSCRIPTEN__
    /* 🔍 wasm/JSPI 调查诊断 trace(2026-04-30 封存).native 路径无影响. */
    fprintf(stderr, "[stackful_resume CALLER id=%d] caller_ra=%p\n",
        id, __builtin_return_address(0)); fflush(stderr);
    /* ✅ fail-fast: 禁止 self-resume (上层逻辑错误,wasm 下会 deadlock 在
     * mb_await_out 上).保留作为下次重启 wasm 时的立即 backtrace 入口. */
    if (target == sf_active_top_ptr()) {
        fprintf(stderr,
            "[stackful_resume FATAL] self-resume detected: id=%d caller_ra=%p\n"
            "  active stack (top→bottom):\n",
            id, __builtin_return_address(0));
        for (int i = sf_active_top - 1; i >= 0; i--) {
            tina_wrapper *w = sf_active_stack[i];
            fprintf(stderr, "    [%d] wrapper=%p coro=%p self_id=%d\n",
                i, (void*)w, (void*)(w ? w->coro : NULL), w ? w->self_id : -1);
        }
        fflush(stderr);
        abort();
    }
#endif

#ifdef __EMSCRIPTEN__
    /* wasm/JSPI 路径:走 tina_wasm_launch + tina_wasm_resume,推 active stack.
     * GC 禁用 + JS_UpdateStackTop 与 native 路径同样语义保守做. */
    if (!target->launched) {
        tina_wasm_launch(target->coro);
        target->launched = 1;
    }
    size_t old_gc_threshold = JS_GetGCThreshold(S->rt);
    JS_SetGCThreshold(S->rt, (size_t)-1);

    sf_active_push(target);
    target->status = STACKFUL_STATUS_RUNNING;
    CORO_TRACE_RESUME(-1, id, -1, id);
    fprintf(stderr, "[stackful_resume id=%d] PRE  active_top=%d\n", id, sf_active_top); fflush(stderr);

    (void)tina_wasm_resume(target->coro, NULL);

    fprintf(stderr, "[stackful_resume id=%d] POST active_top=%d completed=%d\n",
        id, sf_active_top, target->coro->completed); fflush(stderr);
    sf_active_pop();
    fprintf(stderr, "[stackful_resume id=%d] AFTER_POP active_top=%d\n",
        id, sf_active_top); fflush(stderr);

    JS_SetGCThreshold(S->rt, old_gc_threshold);
    JS_UpdateStackTop(S->rt);

    if (target->coro->completed) {
        target->status = STACKFUL_STATUS_DEAD;
        tina_finalize(target->coro);
        if (target->coro->buffer) free(target->coro->buffer);
        free(target);
        S->coroutines[id] = NULL;
        S->count--;
    } else {
        target->status = STACKFUL_STATUS_SUSPENDED;
        target->yield_count++;
    }
    CORO_TRACE_RESUME_RET(-1, id, id, -1, target->status);
    return 0;
#else
    /* native 路径 (原实现保持不变) */
    /* Get caller coroutine ID for nested context tracking */
    int caller_id = -1;
    if (tl_current_coro) {
        tina_wrapper *caller = (tina_wrapper*)tl_current_coro->user_data;
        caller_id = caller ? caller->self_id : -1;
    }

    DEBUG_LOG("[stackful_resume] === RESUME START ===\n");
    DEBUG_LOG("[stackful_resume] target_id=%d, status=%d\n", id, target->status);
    DEBUG_LOG("[stackful_resume] caller_id=%d, tl_current_coro=%p (BEFORE SAVE)\n",
              caller_id, (void*)tl_current_coro);

    /* Save current coroutine (for nested resume support) */
    tina* saved_current_coro = tl_current_coro;
    DEBUG_LOG("[stackful_resume] SAVED: saved_current_coro=%p\n", (void*)saved_current_coro);

    /* Disable GC before entering Tina stack to prevent GC from scanning Tina stack */
    size_t old_gc_threshold = JS_GetGCThreshold(S->rt);
    JS_SetGCThreshold(S->rt, (size_t)-1);
    DEBUG_LOG("[stackful_resume] GC disabled (old_threshold=%zu)\n", old_gc_threshold);

    /* Set thread-local current coroutine for yield */
    tl_current_coro = target->coro;
    DEBUG_LOG("[stackful_resume] SET: tl_current_coro=%p (target coro %d)\n",
              (void*)tl_current_coro, id);
    target->status = STACKFUL_STATUS_RUNNING;
    CORO_TRACE_RESUME(-1, id, -1, id);

    /* ========== ASYMMETRIC COROUTINE: Use tina_resume (proper asymmetric API) ========== */
    DEBUG_LOG("[stackful_resume] >>> CALLING tina_resume(coro %d, NULL)\n", id);

    (void)tina_resume(target->coro, NULL);

    DEBUG_LOG("[stackful_resume] <<< RETURNED from tina_resume\n");
    DEBUG_LOG("[stackful_resume] tl_current_coro=%p (should still be target)\n",
              (void*)tl_current_coro);

    /* CRITICAL FIX: Update QuickJS stack top after switching to coroutine's C stack
     * QuickJS detects stack overflow by checking C stack pointer against stack_limit,
     * but stackful coroutine uses a different C stack. Must update stack_top/stack_limit
     * to match the current stack position to avoid false "Maximum call stack size exceeded" errors */
    JS_UpdateStackTop(S->rt);

    /* Restore GC threshold after leaving Tina stack */
    JS_SetGCThreshold(S->rt, old_gc_threshold);
    DEBUG_LOG("[stackful_resume] GC restored (threshold=%zu)\n", old_gc_threshold);

    /* Check completion */
    if (target->coro->completed) {
        DEBUG_LOG("[stackful_resume] Coroutine %d completed, cleaning up\n", id);

        target->status = STACKFUL_STATUS_DEAD;
        tina_finalize(target->coro);
        if (target->coro->buffer) free(target->coro->buffer);
        free(target);
        S->coroutines[id] = NULL;
        S->count--;

        tl_current_coro = NULL;
        DEBUG_LOG("[stackful_resume] Coroutine %d destroyed (count=%d)\n", id, S->count);
    } else {
        target->status = STACKFUL_STATUS_SUSPENDED;
        target->yield_count++;

        DEBUG_LOG("[stackful_resume] Coroutine %d suspended (yield_count=%d)\n",
                id, target->yield_count);
    }

    /* Restore previous current coroutine (for nested resume support) */
    DEBUG_LOG("[stackful_resume] RESTORING: tl_current_coro from %p to %p\n",
              (void*)tl_current_coro, (void*)saved_current_coro);
    tl_current_coro = saved_current_coro;
    DEBUG_LOG("[stackful_resume] RESTORED: tl_current_coro=%p (caller_id=%d)\n",
              (void*)tl_current_coro, caller_id);

    CORO_TRACE_RESUME_RET(-1, id, id, -1, target->status);

    DEBUG_LOG("[stackful_resume] === RESUME END ===\n");
    DEBUG_LOG("[stackful_resume] target_id=%d\n", id);

    return 0;
#endif
}

void* stackful_resume_with_value(stackful_schedule *S, int id, void *value) {
    if (!S || id < 0 || id >= S->cap || !S->coroutines[id]) {
        DEBUG_LOG("[stackful_resume_with_value] ERROR: Invalid coroutine id=%d\n", id);
        return NULL;
    }

    tina_wrapper *target = S->coroutines[id];

    if (target->status == STACKFUL_STATUS_DEAD) {
        DEBUG_LOG("[stackful_resume_with_value] ERROR: Coroutine id=%d already dead\n", id);
        return NULL;
    }

#ifdef __EMSCRIPTEN__
    /* 🔍 wasm/JSPI 调查诊断 trace(2026-04-30 封存).native 路径无影响. */
    fprintf(stderr, "[stackful_resume_with_value CALLER id=%d] caller_ra=%p v=%p\n",
        id, __builtin_return_address(0), value); fflush(stderr);
    /* ✅ fail-fast: 禁止 self-resume,见 stackful_resume 同款注释. */
    if (target == sf_active_top_ptr()) {
        fprintf(stderr,
            "[stackful_resume_with_value FATAL] self-resume detected: id=%d caller_ra=%p\n"
            "  active stack (top→bottom):\n",
            id, __builtin_return_address(0));
        for (int i = sf_active_top - 1; i >= 0; i--) {
            tina_wrapper *w = sf_active_stack[i];
            fprintf(stderr, "    [%d] wrapper=%p coro=%p self_id=%d\n",
                i, (void*)w, (void*)(w ? w->coro : NULL), w ? w->self_id : -1);
        }
        fflush(stderr);
        abort();
    }
    /* wasm/JSPI 路径 */
    if (!target->launched) {
        tina_wasm_launch(target->coro);
        target->launched = 1;
    }
    size_t old_gc_threshold = JS_GetGCThreshold(S->rt);
    JS_SetGCThreshold(S->rt, (size_t)-1);

    sf_active_push(target);
    target->status = STACKFUL_STATUS_RUNNING;
    fprintf(stderr, "[stackful_resume_with_value id=%d] PRE  active_top=%d v=%p\n",
        id, sf_active_top, value); fflush(stderr);

    void *result = tina_wasm_resume(target->coro, value);

    fprintf(stderr, "[stackful_resume_with_value id=%d] POST active_top=%d completed=%d r=%p\n",
        id, sf_active_top, target->coro->completed, result); fflush(stderr);
    sf_active_pop();
    fprintf(stderr, "[stackful_resume_with_value id=%d] AFTER_POP active_top=%d\n",
        id, sf_active_top); fflush(stderr);

    JS_SetGCThreshold(S->rt, old_gc_threshold);
    JS_UpdateStackTop(S->rt);

    if (target->coro->completed) {
        target->status = STACKFUL_STATUS_DEAD;
        tina_finalize(target->coro);
        if (target->coro->buffer) free(target->coro->buffer);
        free(target);
        S->coroutines[id] = NULL;
        S->count--;
        result = NULL;
    } else {
        target->status = STACKFUL_STATUS_SUSPENDED;
        target->yield_count++;
    }
    return result;
#else
    /* native 路径 (原实现保持不变) */
    /* Get caller coroutine ID for nested context tracking */
    int caller_id = -1;
    if (tl_current_coro) {
        tina_wrapper *caller = (tina_wrapper*)tl_current_coro->user_data;
        caller_id = caller ? caller->self_id : -1;
    }

    DEBUG_LOG("[stackful_resume_with_value] === RESUME START ===\n");
    DEBUG_LOG("[stackful_resume_with_value] target_id=%d, value=%p\n", id, value);
    DEBUG_LOG("[stackful_resume_with_value] caller_id=%d, tl_current_coro=%p (BEFORE SAVE)\n",
              caller_id, (void*)tl_current_coro);

    /* Save current coroutine (for nested resume support) */
    tina* saved_current_coro = tl_current_coro;
    DEBUG_LOG("[stackful_resume_with_value] SAVED: saved_current_coro=%p\n", (void*)saved_current_coro);

    /* Disable GC before entering Tina stack to prevent GC from scanning Tina stack */
    size_t old_gc_threshold = JS_GetGCThreshold(S->rt);
    JS_SetGCThreshold(S->rt, (size_t)-1);
    DEBUG_LOG("[stackful_resume_with_value] GC disabled (old_threshold=%zu)\n", old_gc_threshold);

    /* Set thread-local current coroutine for yield */
    tl_current_coro = target->coro;
    DEBUG_LOG("[stackful_resume_with_value] SET: tl_current_coro=%p (target coro %d)\n",
              (void*)tl_current_coro, id);
    target->status = STACKFUL_STATUS_RUNNING;

    /* ========== ASYMMETRIC COROUTINE: Use tina_resume (proper asymmetric API) ========== */
    DEBUG_LOG("[stackful_resume_with_value] >>> CALLING tina_resume(coro %d, value=%p)\n",
            id, value);

    void *result = tina_resume(target->coro, value);

    DEBUG_LOG("[stackful_resume_with_value] <<< RETURNED from tina_resume, result=%p\n", result);
    DEBUG_LOG("[stackful_resume_with_value] tl_current_coro=%p (should still be target)\n",
              (void*)tl_current_coro);

    /* CRITICAL FIX: Update QuickJS stack top after switching to coroutine's C stack
     * QuickJS detects stack overflow by checking C stack pointer against stack_limit,
     * but stackful coroutine uses a different C stack. Must update stack_top/stack_limit
     * to match the current stack position to avoid false "Maximum call stack size exceeded" errors */
    JS_UpdateStackTop(S->rt);

    /* Restore GC threshold after leaving Tina stack */
    JS_SetGCThreshold(S->rt, old_gc_threshold);
    DEBUG_LOG("[stackful_resume_with_value] GC restored (threshold=%zu)\n", old_gc_threshold);

    /* Check completion */
    if (target->coro->completed) {
        DEBUG_LOG("[stackful_resume_with_value] Coroutine %d completed, cleaning up\n", id);

        target->status = STACKFUL_STATUS_DEAD;
        tina_finalize(target->coro);
        if (target->coro->buffer) free(target->coro->buffer);
        free(target);
        S->coroutines[id] = NULL;
        S->count--;

        tl_current_coro = NULL;
        DEBUG_LOG("[stackful_resume_with_value] Coroutine %d destroyed (count=%d)\n", id, S->count);
        result = NULL;
    } else {
        target->status = STACKFUL_STATUS_SUSPENDED;
        target->yield_count++;

        DEBUG_LOG("[stackful_resume_with_value] Coroutine %d suspended (yield_count=%d)\n",
                id, target->yield_count);
    }

    /* Restore previous current coroutine (for nested resume support) */
    DEBUG_LOG("[stackful_resume_with_value] RESTORING: tl_current_coro from %p to %p\n",
              (void*)tl_current_coro, (void*)saved_current_coro);
    tl_current_coro = saved_current_coro;
    DEBUG_LOG("[stackful_resume_with_value] RESTORED: tl_current_coro=%p (caller_id=%d)\n",
              (void*)tl_current_coro, caller_id);

    DEBUG_LOG("[stackful_resume_with_value] === RESUME END ===\n");
    DEBUG_LOG("[stackful_resume_with_value] target_id=%d, result=%p\n", id, result);

    return result;
#endif
}

void stackful_yield(stackful_schedule *S) {
    if (!S) {
        DEBUG_LOG("[stackful_yield] ERROR: NULL scheduler\n");
        return;
    }
#ifdef __EMSCRIPTEN__
    tina_wrapper *self = sf_active_top_ptr();
    if (!self) {
        DEBUG_LOG("[stackful_yield] ERROR: active stack empty (yield outside coro)\n");
        return;
    }
    (void)tina_wasm_yield(self->coro, NULL);
    JS_UpdateStackTop(S->rt);
#else
    if (!tl_current_coro) {
        DEBUG_LOG("[stackful_yield] ERROR: No current coroutine (yield called outside coroutine)\n");
        return;
    }

    DEBUG_LOG("[stackful_yield] Coroutine yielding to dispatcher\n");

    tina_yield(tl_current_coro, NULL);

    DEBUG_LOG("[stackful_yield] Coroutine resumed after yield\n");
#endif
}

void* stackful_yield_with_value(stackful_schedule *S, void *value) {
    if (!S) {
        DEBUG_LOG("[stackful_yield_with_value] ERROR: NULL scheduler\n");
        return NULL;
    }
#ifdef __EMSCRIPTEN__
    tina_wrapper *self = sf_active_top_ptr();
    if (!self) {
        DEBUG_LOG("[stackful_yield_with_value] ERROR: active stack empty\n");
        return NULL;
    }
    /* 🔍 wasm/JSPI 调查诊断 trace(2026-04-30 封存).Result 3 实验:跨
     * yield 的 JSStackFrame 字段 + arg_buf 指针 + arg[0] 内容全保留,
     * 但 JS 层 args[0] 仍变 undefined → 真根因落在 JS_CallInternal C
     * 局部缓存,本层无法干预.详见 hand-off "2026-04-30 调查总结". */
    JS_DiagDumpCurrentFrame(S->main_ctx, "PRE");
    void *r = tina_wasm_yield(self->coro, value);
    JS_DiagDumpCurrentFrame(S->main_ctx, "POST");
    JS_UpdateStackTop(S->rt);
    return r;
#else
    if (!tl_current_coro) {
        DEBUG_LOG("[stackful_yield_with_value] ERROR: No current coroutine (yield called outside coroutine)\n");
        return NULL;
    }

    /* Get current coroutine ID for better logging */
    tina_wrapper *wrapper = (tina_wrapper*)tl_current_coro->user_data;
    int coro_id = wrapper ? wrapper->self_id : -1;

    DEBUG_LOG("[stackful_yield_with_value] === YIELD START ===\n");
    DEBUG_LOG("[stackful_yield_with_value] coro_id=%d, tl_current_coro=%p, value=%p\n",
              coro_id, (void*)tl_current_coro, value);

    void *result = tina_yield(tl_current_coro, value);

    DEBUG_LOG("[stackful_yield_with_value] === YIELD RESUMED ===\n");
    DEBUG_LOG("[stackful_yield_with_value] coro_id=%d, tl_current_coro=%p, got result=%p\n",
              coro_id, (void*)tl_current_coro, result);

    return result;
#endif
}

void stackful_yield_with_flag(stackful_schedule *S, int flag) {
    if (!S) {
        DEBUG_LOG("[stackful_yield_with_flag] ERROR: NULL scheduler\n");
        return;
    }

#ifdef __EMSCRIPTEN__
    tina_wrapper *self = sf_active_top_ptr();
    if (!self) {
        DEBUG_LOG("[stackful_yield_with_flag] ERROR: active stack empty\n");
        return;
    }
    int coro_id = self->self_id;
    if (coro_id < 0 || coro_id >= S->cap) {
        DEBUG_LOG("[stackful_yield_with_flag] ERROR: Invalid coro_id=%d\n", coro_id);
        return;
    }
    tina_storage *storage = &S->storages[coro_id];
    if (tina_storage_push(storage, &flag, sizeof(int)) < 0) {
        DEBUG_LOG("[stackful_yield_with_flag] ERROR: Failed to push flag\n");
        return;
    }
    (void)tina_wasm_yield(self->coro, NULL);
    JS_UpdateStackTop(S->rt);
#else
    if (!tl_current_coro) {
        DEBUG_LOG("[stackful_yield_with_flag] ERROR: No current coroutine (yield called outside coroutine)\n");
        return;
    }

    /* Get current coroutine ID from wrapper */
    tina_wrapper *wrapper = (tina_wrapper*)tl_current_coro->user_data;
    if (!wrapper) {
        DEBUG_LOG("[stackful_yield_with_flag] ERROR: No wrapper found for current coroutine\n");
        return;
    }

    int coro_id = wrapper->self_id;

    if (coro_id < 0 || coro_id >= S->cap) {
        DEBUG_LOG("[stackful_yield_with_flag] ERROR: Invalid coro_id=%d\n", coro_id);
        return;
    }

    tina_storage *storage = &S->storages[coro_id];

    DEBUG_LOG("[stackful_yield_with_flag] coro_id=%d pushing flag=%d before yield\n", coro_id, flag);

    /* Push flag to storage */
    if (tina_storage_push(storage, &flag, sizeof(int)) < 0) {
        DEBUG_LOG("[stackful_yield_with_flag] ERROR: Failed to push flag\n");
        return;
    }

    tina_yield(tl_current_coro, NULL);

    DEBUG_LOG("[stackful_yield_with_flag] coro_id=%d resumed after yield\n", coro_id);
#endif
}

int stackful_pop_continue_flag(stackful_schedule *S, int id) {
    if (!S || id < 0 || id >= S->cap || !S->coroutines[id]) {
        return 0;  /* No flag */
    }
    
    tina_storage *storage = &S->storages[id];
    size_t bytes = tina_storage_bytes(storage);
    
    DEBUG_LOG( "[stackful_pop_continue_flag] coro_id=%d has %zu bytes stored\n",
            id, bytes);
    
    if (bytes != sizeof(int)) {
        return 0;  /* No flag or wrong size */
    }
    
    int flag = 0;
    if (tina_storage_pop(storage, &flag, sizeof(int)) < 0) {
        DEBUG_LOG( "[stackful_pop_continue_flag] ERROR: Failed to pop flag\n");
        return 0;
    }
    
    DEBUG_LOG( "[stackful_pop_continue_flag] coro_id=%d popped flag=%d\n", id, flag);
    return flag;
}

/* ========== Generic data storage API ========== */

int stackful_push_data(stackful_schedule *S, int id, const void *data, size_t len) {
    if (!S || id < 0 || id >= S->cap || !S->coroutines[id]) {
        return -1;
    }
    
    tina_storage *storage = &S->storages[id];
    return tina_storage_push(storage, data, len);
}

int stackful_pop_data(stackful_schedule *S, int id, void *data, size_t len) {
    if (!S || id < 0 || id >= S->cap || !S->coroutines[id]) {
        return -1;
    }
    
    tina_storage *storage = &S->storages[id];
    return tina_storage_pop(storage, data, len);
}

size_t stackful_get_stored_bytes(stackful_schedule *S, int id) {
    if (!S || id < 0 || id >= S->cap || !S->coroutines[id]) {
        return 0;
    }
    
    tina_storage *storage = &S->storages[id];
    return tina_storage_bytes(storage);
}

stackful_status_t stackful_status(stackful_schedule *S, int id) {
    if (!S || id < 0 || id >= S->cap || !S->coroutines[id]) {
        return STACKFUL_STATUS_DEAD;
    }
    
    return S->coroutines[id]->status;
}

/* ========== Coroutine introspection (aligned with Lua coroutine.running) ========== */

/**
 * Get the ID of the currently running coroutine
 * 
 * Equivalent to Lua's coroutine.running() - returns the current coroutine identifier.
 * This is a core part of the stackful coroutine wrapper API.
 * 
 * Implementation: O(1) lookup using thread-local tl_current_coro and wrapper->self_id
 * 
 * @param S  Scheduler (unused but kept for API consistency)
 * @return   Current coroutine ID (>= 0), or -1 if not running in a coroutine
 */
int stackful_running(stackful_schedule *S) {
    (void)S;  /* Unused - kept for API compatibility */

#ifdef __EMSCRIPTEN__
    tina_wrapper *self = sf_active_top_ptr();
    return self ? self->self_id : -1;
#else
    DEBUG_LOG("[stackful_running] CALLED: tl_current_coro=%p\n", (void*)tl_current_coro);

    /* Not running in a coroutine context (dispatcher/main thread) */
    if (!tl_current_coro) {
        DEBUG_LOG("[stackful_running] RETURN: -1 (not in coroutine)\n");
        return -1;
    }

    /* O(1) lookup: tina stores wrapper pointer in coro->user_data */
    tina_wrapper *wrapper = (tina_wrapper*)tl_current_coro->user_data;
    if (wrapper) {
        DEBUG_LOG("[stackful_running] RETURN: %d (tl_current_coro=%p, wrapper=%p)\n",
                  wrapper->self_id, (void*)tl_current_coro, (void*)wrapper);
        return wrapper->self_id;
    }

    /* Inconsistent state - shouldn't happen if tl_current_coro is properly managed */
    DEBUG_LOG("[stackful_running] ERROR: tl_current_coro=%p set but wrapper not found\n",
              (void*)tl_current_coro);
    return -1;
#endif
}

/* ========== QuickJS Integration ========== */

/* Helper: Get stackful scheduler from JSContext opaque (service_ud) */
static stackful_schedule* get_scheduler_from_ctx(JSContext *ctx) {
    // Get service_ud from context opaque
    const void *opaque = JS_GetContextOpaque(ctx);
    if (!opaque) {
        return NULL;
    }
    
    // service_ud contains task and id, we need to get service from service_pool
    // This is a bit complex, so for now we use a simpler approach:
    // Store scheduler pointer directly in JS context using JS_SetContextOpaque
    // But service.c already uses opaque for service_ud...
    
    // Alternative: use thread-local storage or get from service via task pointer
    // For now, return NULL and handle in callers
    return NULL;
}

/* JS: Stackful.yield() */
static JSValue js_stackful_yield(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    // NOTE: This JS API is deprecated and should not be used
    // Use jtask.yield_control() instead which has access to service context
    return JS_ThrowInternalError(ctx, "Stackful.yield() is deprecated, use jtask.yield_control()");
}

/* JS: Stackful.running() */
static JSValue js_stackful_running(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    // NOTE: This JS API is deprecated
    return JS_ThrowInternalError(ctx, "Stackful.running() is deprecated, use jtask APIs");
}

/* JS: Stackful.status(id) */
static JSValue js_stackful_status(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {
    // NOTE: This JS API is deprecated
    return JS_ThrowInternalError(ctx, "Stackful.status() is deprecated, use jtask APIs");
}

int stackful_enable_js_api(JSContext *ctx, stackful_schedule *S) {
    if (!ctx || !S) {
        DEBUG_LOG("[stackful_enable_js_api] ERROR: Invalid arguments\n");
        return -1;
    }
    
    // NOTE: Removed global variable g_stackful_schedule for thread safety
    // Each service has its own stackful_sched in struct service
    // JS APIs should use jtask.* functions which have access to service context

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue stackful_obj = JS_NewObject(ctx);

    /* Register methods */
    JS_SetPropertyStr(ctx, stackful_obj, "yield",
                     JS_NewCFunction(ctx, js_stackful_yield, "yield", 0));

    JS_SetPropertyStr(ctx, stackful_obj, "running",
                     JS_NewCFunction(ctx, js_stackful_running, "running", 0));

    JS_SetPropertyStr(ctx, stackful_obj, "status",
                     JS_NewCFunction(ctx, js_stackful_status, "status", 1));

    /* Status constants */
    JS_SetPropertyStr(ctx, stackful_obj, "DEAD",
                     JS_NewInt32(ctx, STACKFUL_STATUS_DEAD));
    JS_SetPropertyStr(ctx, stackful_obj, "NORMAL",
                     JS_NewInt32(ctx, STACKFUL_STATUS_NORMAL));
    JS_SetPropertyStr(ctx, stackful_obj, "RUNNING",
                     JS_NewInt32(ctx, STACKFUL_STATUS_RUNNING));
    JS_SetPropertyStr(ctx, stackful_obj, "SUSPENDED",
                     JS_NewInt32(ctx, STACKFUL_STATUS_SUSPENDED));

    JS_SetPropertyStr(ctx, global, "Stackful", stackful_obj);
    JS_FreeValue(ctx, global);
    
    DEBUG_LOG("[stackful_enable_js_api] JS API registered (deprecated, use jtask APIs)\n");

    return 0;
}

stackful_schedule* stackful_get_global_schedule(void) {
    // NOTE: This function is deprecated and should not be used
    // It was used for global variable access which causes thread safety issues
    return NULL;
}
