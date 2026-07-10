/**
 * test_fiber_backtrace.c
 * 复现 + 回归测试: fiber (tina 协程) 内 throw 时 build_backtrace 走坏帧链
 *
 * 事故取证 (arc-mapgen 2026-07-11, wear_loop 偶发挂死 199% CPU):
 *   主线程栈 js_jtask_create_coroutine → fork_coro_entry → JS_Call → …
 *   → js_error_constructor → build_backtrace 空转。
 *
 * 机理: rt->current_stack_frame 是全局单链。协程首个 JS 帧的 prev_frame
 * 指向 fork 那一刻主栈深处的帧; 协程 yield 后主栈退栈、内存被后续调用
 * 覆写; 再 resume 协程内一 throw, build_backtrace 顺着悬垂 prev_frame
 * 走进垃圾内存 → 乱走/成环/挂死 (偶发性 = 主栈内存复用内容随机)。
 *
 * 复现步骤 (本测试确定性放大):
 *   1. 主栈深递归 (~200 层 JS 帧) 中创建并首跑协程, 协程 yield;
 *   2. 主栈完全退栈, 再跑一轮深递归把旧栈内存覆写成新帧再退掉;
 *   3. 浅栈处 resume, 协程 throw new Error → 构造期走 backtrace。
 * 修复前: backtrace 含垃圾帧/挂死/崩溃; 修复后: 干净的 "boom" +
 * 栈迹止于协程入口 (帧链按协程隔离, 语义对齐 Lua per-coroutine CallInfo)。
 *
 * 构建/运行: ./build_fiber_backtrace_test.sh
 */

#include "quickjs_stackful_mini.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

typedef struct {
    JSContext *ctx;
    stackful_schedule *S;
    JSValue coro_fn;
    int coro_id;
    int coro_threw;
    char stack_text[4096];
} test_ctx_t;

static test_ctx_t g;

/* 协程入口: 对齐 jtask fork_coro_entry 的骨架 (UpdateStackTop + JS_Call) */
static void coro_entry(void *ud, void *resume_value) {
    (void)resume_value;
    test_ctx_t *t = (test_ctx_t *)ud;
    JS_UpdateStackTop(JS_GetRuntime(t->ctx));
    JSValue r = JS_Call(t->ctx, t->coro_fn, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(r)) {
        t->coro_threw = 1;
        JSValue exc = JS_GetException(t->ctx);
        JSValue stack = JS_GetPropertyStr(t->ctx, exc, "stack");
        const char *msg = JS_ToCString(t->ctx, exc);
        const char *st = JS_ToCString(t->ctx, stack);
        snprintf(g.stack_text, sizeof(g.stack_text), "%s\n%s",
                 msg ? msg : "?", st ? st : "?");
        JS_FreeCString(t->ctx, msg);
        JS_FreeCString(t->ctx, st);
        JS_FreeValue(t->ctx, stack);
        JS_FreeValue(t->ctx, exc);
    }
    JS_FreeValue(t->ctx, r);
}

/* native: 在当前 (深) 主栈处创建协程并首跑到 yield */
static JSValue js_fork_here(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv) {
    (void)this_val;
    assert(argc >= 1);
    g.coro_fn = JS_DupValue(ctx, argv[0]);
    g.coro_id = stackful_new(g.S, coro_entry, &g);
    assert(g.coro_id >= 0);
    stackful_resume(g.S, g.coro_id);   /* 首跑: 协程内部 yield 后回来 */
    return JS_UNDEFINED;
}

/* native: 协程内 yield (对齐 jtask.yield_control 骨架) */
static JSValue js_cyield(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    stackful_yield(g.S);
    JS_UpdateStackTop(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}

/* 把主栈上已退栈的死帧内存砸成 0xAA (模拟引擎里后续代码覆写该区域) —
 * 修复前协程帧的 prev_frame 悬垂指进这片, backtrace 将 deref 0xAAAA… */
static void __attribute__((noinline)) smash_dead_stack(size_t bytes) {
    volatile unsigned char *p = (volatile unsigned char *)alloca(bytes);
    memset((void *)p, 0xAA, bytes);
    p[0] = p[bytes - 1];
}

static JSValue eval_ck(JSContext *ctx, const char *src, const char *name) {
    JSValue v = JS_Eval(ctx, src, strlen(src), name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue exc = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, exc);
        fprintf(stderr, "EVAL FAIL %s: %s\n", name, s ? s : "?");
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, exc);
        exit(2);
    }
    return v;
}

int main(void) {
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    assert(rt && ctx);
    JS_UpdateStackTop(rt);

    g.ctx = ctx;
    g.S = stackful_open(rt, ctx);
    assert(g.S);

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "fork_here",
                      JS_NewCFunction(ctx, js_fork_here, "fork_here", 1));
    JS_SetPropertyStr(ctx, global, "cyield",
                      JS_NewCFunction(ctx, js_cyield, "cyield", 0));
    JS_FreeValue(ctx, global);

    /* 步骤 1: 深主栈 (200 层 JS 帧) 中 fork; 协程 yield 一次后控制权回来 */
    JS_FreeValue(ctx, eval_ck(ctx,
        "function deep(n, f) { if (n <= 0) return f(); return deep(n - 1, f) + 1; }\n"
        "deep(200, function atFork() {\n"
        "  fork_here(function coroBody() {\n"
        "    cyield();\n"
        "    throw new Error('boom');\n"
        "  });\n"
        "  return 0;\n"
        "});\n", "step1.js"));

    /* 步骤 2: 主栈已退; 把死帧内存砸花 (2MB 覆盖 deep(200) 的全部 JS 帧) */
    smash_dead_stack(2u << 20);

    /* 步骤 3: 浅栈 resume → 协程 throw → Error 构造期 build_backtrace */
    fprintf(stderr, "[test] resuming coroutine (throw inside fiber)...\n");
    stackful_resume(g.S, g.coro_id);

    if (!g.coro_threw) {
        fprintf(stderr, "FAIL: coroutine did not throw\n");
        return 1;
    }
    fprintf(stderr, "[test] fiber exception captured:\n%s\n", g.stack_text);

    if (!strstr(g.stack_text, "boom") || !strstr(g.stack_text, "coroBody")) {
        fprintf(stderr, "FAIL: backtrace missing boom/coroBody\n");
        return 1;
    }
    /* 帧链按协程隔离后, 栈迹不得含主栈的 deep/atFork/clobber 帧 */
    if (strstr(g.stack_text, "deep") || strstr(g.stack_text, "atFork") ||
        strstr(g.stack_text, "clobber")) {
        fprintf(stderr, "FAIL: fiber backtrace leaked main-stack frames "
                        "(dangling prev_frame walk)\n");
        return 1;
    }

    JS_FreeValue(ctx, g.coro_fn);
    stackful_close(g.S);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    fprintf(stderr, "PASS: fiber-isolated backtrace OK\n");
    return 0;
}
