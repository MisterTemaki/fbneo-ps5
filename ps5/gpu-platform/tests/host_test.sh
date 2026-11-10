#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 MisterTemaki
#
# Host tests of the platform layer: its real sources built for Linux under AddressSanitizer, every function they define
# renamed t_<name> (glibc has its own), linked with the RADV link's --wrap flags (tools/ps5rt-radv-link.sh --flags);
# compiler-rt's emutls.c (built as for the console) and x86.c behind -femulated-tls and __builtin_cpu_supports; and
# tools/check_link.py on fixture traces. Needs cc (gcc or clang), clang, objcopy, nm, python3.
#   platform/tests/host_test.sh [work-dir]
set -eu
P=$(cd "$(dirname "$0")/.." && pwd)
W=${1:-$(mktemp -d)}
mkdir -p "$W"
CC=${HOST_CC:-cc}
CLANG=${HOST_CLANG:-clang}
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
# glibc marks some of these parameters nonnull; the console's headers don't, and the null checks stay
KEEP_NULL_CHECKS="-fno-delete-null-pointer-checks -Wno-nonnull-compare"
rc=0

# the platform objects, renamed
objs=
for s in qsort_r misc fs memstream stdio_wrap thread locale; do
	$CC -std=gnu11 -O1 -g -Wall -Wextra -Werror $SAN $KEEP_NULL_CHECKS -I"$P/include" -c -o "$W/$s.o" "$P/src/ps5rt_$s.c"
	ren=$(nm -g --defined-only "$W/$s.o" | awk 'NF == 3 {print $3}' | grep -v -e '^ps5rt_' -e '^__wrap_' |
		sed 's/^\(__isoc23_\)\{0,1\}\(.*\)$/--redefine-sym &=t_\2/' | tr '\n' ' ')   # glibc's C23 names too
	# shellcheck disable=SC2086
	objcopy $ren "$W/$s.o" "$W/$s-t.o"
	objs="$objs $W/$s-t.o"
done
# shellcheck disable=SC2086
$CC -std=gnu11 -O1 -g -Wall -Wextra $SAN -o "$W/test_platform" "$P/tests/test_platform.c" $objs \
	$(sh "$P/tools/ps5rt-radv-link.sh" --flags | sed 's/--wrap=/-Wl,--wrap=/g') -Wl,--wrap=realloc -lpthread
ASAN_OPTIONS=detect_leaks=1 "$W/test_platform" "$W" || rc=1

# compiler-rt: emulated TLS (with the console's delayed destructor round) and __cpu_model
# (no sanitizer: the host's clang may have no ASan runtime)
$CLANG -std=gnu11 -O1 -g -D__PROSPERO__ -c -o "$W/emutls.o" "$P/third_party/compiler-rt-18.1.8/emutls.c"
$CLANG -std=gnu11 -O1 -g -c -o "$W/x86.o" "$P/third_party/compiler-rt-18.1.8/x86.c"
$CLANG -std=gnu11 -O1 -g -femulated-tls -o "$W/test_crt" "$P/tests/test_crt.c" "$W/emutls.o" "$W/x86.o" -lpthread
if nm "$W/test_crt" | grep -qE ' [TW] __emutls_get_address$' && nm "$W/test_crt" | grep -q '__emutls_v\.g_value'; then
	echo "ok   test_crt: thread-locals go through this emutls.o"
else
	echo "FAIL test_crt: the thread-locals don't use this emutls.o"; rc=1
fi
"$W/test_crt" || rc=1

# tools/check_link.py on fixture traces
sh "$P/tests/test_check_link.sh" "$W/check_link" || rc=1

[ $rc = 0 ] && echo "platform host_test: ok" || echo "platform host_test: FAILED"
exit $rc
