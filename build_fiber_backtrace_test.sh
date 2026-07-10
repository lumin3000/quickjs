#!/bin/bash
# 构建并运行 fiber backtrace 回归测试 (test_fiber_backtrace.c)
set -e
cd "$(dirname "$0")"
CC="${CC:-clang}"
CFLAGS="-O1 -g -Wall -D_GNU_SOURCE -DCONFIG_VERSION=\"fibertest\""
$CC $CFLAGS -c quickjs.c -o /tmp/qjs_fbt_quickjs.o
$CC $CFLAGS -c quickjs_stackful_mini.c -o /tmp/qjs_fbt_stackful.o
$CC $CFLAGS -c cutils.c -o /tmp/qjs_fbt_cutils.o
$CC $CFLAGS -c libregexp.c -o /tmp/qjs_fbt_libregexp.o
$CC $CFLAGS -c libunicode.c -o /tmp/qjs_fbt_libunicode.o
$CC $CFLAGS -c dtoa.c -o /tmp/qjs_fbt_dtoa.o
$CC $CFLAGS -c test_fiber_backtrace.c -o /tmp/qjs_fbt_test.o
$CC /tmp/qjs_fbt_*.o -lm -lpthread -o /tmp/test_fiber_backtrace
echo "=== running (10s timeout: 修复前可能挂死) ==="
/tmp/test_fiber_backtrace &
PID=$!
for i in $(seq 1 100); do
  kill -0 $PID 2>/dev/null || break
  sleep 0.1
done
if kill -0 $PID 2>/dev/null; then
  echo "HANG: test still running after 10s (bug reproduced), killing"
  kill -9 $PID
  exit 3
fi
wait $PID
