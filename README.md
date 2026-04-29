# QuickJS (jtask fork)

This directory contains QuickJS plus the integration shims used by jtask.

## Files

| File | Purpose |
|------|---------|
| `quickjs.c` / `quickjs.h` | Upstream QuickJS interpreter |
| `quickjs-libc.c` / `quickjs-libc.h` | Upstream QuickJS libc helpers |
| `quickjs_searchpath.c` / `quickjs_searchpath.h` | Module resolution helper used by jtask |
| `quickjs_stackful_mini.c` / `quickjs_stackful_mini.h` | Stackful coroutine layer (Tina backend, used by jtask) |
| `quickjs_stackful.c` / `quickjs_stackful.h` | Earlier ucontext-based stackful prototype (kept for reference, not built) |
| `tina.h` | Tina coroutine library header |

## Build

QuickJS is built as part of the jtask build (see `external/jtask/Makefile`). The
sources currently compiled are listed in jtask's `QUICKJS_SRCS` variable.

## Coroutines

jtask uses the stackful coroutine API exposed by `quickjs_stackful_mini.h`
(`stackful_open` / `stackful_resume` / `stackful_yield` / ...). The current
backend is Tina; a future Lua-VM-based backend is planned for wasm.

A previous Generator-based coroutine layer (`quickjs_coroutine.c`) was removed
on 2026-04-29 — it had been registered but was not actually used by jtask.
