#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 MisterTemaki
#
# tools/check_link.py against fixtures: traces written here (each rule passes and fails once), and small Linux ELFs
# linked to fake system-module stubs (sonames libkernel.sprx ...) for the imports check.
#   platform/tests/test_check_link.sh work-dir
set -eu
P=$(cd "$(dirname "$0")/.." && pwd)
W=$1
rm -rf "$W"
mkdir -p "$W/sdk" "$W/own"
check_link() { python3 -B "$P/tools/check_link.py" "$@"; }
CC=${HOST_CC:-cc}
rc=0
expect() { # expect pass|fail description command...
	want=$1 what=$2
	shift 2
	if "$@" >"$W/out.txt" 2>&1; then got=pass; else got=fail; fi
	if [ "$got" = "$want" ]; then echo "ok   check_link: $what"; else
		echo "FAIL check_link: $what (expected $want)"; cat "$W/out.txt"; rc=1; fi
}

S=$W/sdk
for f in libc.a libc++.a libc++abi.a libunwind.a libSceLibcInternal.so libkernel.so libkernel_sys.so crt1.o \
	libps5platform.a; do : >"$S/$f"; done
: >"$W/own/main.o"
: >"$W/libvulkan_radeon.ps5.a"
: >"$W/libps5rt_platform.a"
good() {
	cat <<EOF
$W/own/main.o
$W/libvulkan_radeon.ps5.a(radv_device.c.o)
$W/libps5rt_platform.a(ps5rt_fs.o)
$S/libc++.a(string.cpp.o)
$S/libc++abi.a(cxa_guard.cpp.o)
$S/libunwind.a(libunwind.cpp.o)
$S/libSceLibcInternal.so
$S/libkernel.so
$S/libc.a(assert.o)
$S/libc.a(regcomp.o)
$S/libc.a(syscalls.o): lazy definition of readlink
$W/libvulkan_radeon.ps5.a(ac_gpu_info.c.o): reference to readlink
$W/libps5rt_platform.a(ps5rt_fs.o): definition of readlink
EOF
}
trace() { check_link trace "$1" --sdk-lib "$S" --libc-allow "$P/libc-allow.txt" --own "$W/own" --archive "$W/libvulkan_radeon.ps5.a" \
	--archive "$W/libps5rt_platform.a" --require main.o; }
good >"$W/good.trace"
expect pass "trace: allowed inputs" trace "$W/good.trace"
for bad in "$S/libc.a(netdb.o)" "$S/libc.a(qsort_r.o)" "$S/libps5platform.a(heap.o)" "$S/crt1.o" \
	"$W/proto/native/app_crt.o" "$W/elsewhere/other.o" "$W/other.a(x.o)" "$S/libkernel_sys.so"; do
	{ good; echo "$bad"; } >"$W/bad.trace"
	expect fail "trace refuses $bad" trace "$W/bad.trace"
done
good | grep -v main.o >"$W/nomain.trace"
expect fail "trace: a required object missing" trace "$W/nomain.trace"

expect pass "syscalls: only lazy definitions" check_link syscalls "$W/good.trace"
{ good; echo "$S/libc.a(syscalls.o)"; } >"$W/sys1.trace"
expect fail "syscalls: syscalls.o loaded" check_link syscalls "$W/sys1.trace"
{ good; echo "$S/libc.a(syscalls.o): definition of link"; echo "$W/own/main.o: reference to link"; } >"$W/sys2.trace"
expect fail "syscalls: a call into syscalls.o" check_link syscalls "$W/sys2.trace"

# imports: fake stubs with the system modules' sonames, and programs linked against them
mkdir -p "$W/stubs"
mkstub() { # mkstub name.sprx 'C source'
	echo "$2" >"$W/stubs/$1.c"
	$CC -shared -fPIC -o "$W/stubs/${1%.sprx}.so" -Wl,-soname,"$1" "$W/stubs/$1.c"
}
mkstub libkernel.sprx 'int sceKernelUsleep(int u) { return u; }'
mkstub libSceAgc.sprx 'int sceAgcInit(void) { return 0; }'
mkstub libkernel_sys.sprx 'int sceKernelUsleep(int u) { return u; } int fstatat(void) { return 0; }'
mkstub libSceLibcInternal.sprx 'int _ZSt9terminatev(void) { return 0; } int __cxa_atexit(void) { return 0; } int _DefaultRuneLocale[4];'
prog() { # prog name 'C source' libs...
	n=$1 src=$2
	shift 2
	echo "$src" >"$W/$n.c"
	$CC -nostdlib -fno-stack-protector -Wl,-e,main -o "$W/$n" "$W/$n.c" -L"$W/stubs" "$@"
}
prog ok 'int sceKernelUsleep(int); int sceAgcInit(void); int main(void) { return sceKernelUsleep(0) + sceAgcInit(); }' \
	-Wl,--no-as-needed -lkernel -lSceAgc
prog sys 'int fstatat(void); int main(void) { return fstatat(); }' -Wl,--no-as-needed -lkernel_sys
prog cxx 'int _ZSt9terminatev(void); int __cxa_atexit(void); int main(void) { return _ZSt9terminatev() + __cxa_atexit(); }' \
	-Wl,--no-as-needed -lSceLibcInternal
prog rune 'extern int _DefaultRuneLocale[4]; int main(void) { return _DefaultRuneLocale[1]; }' -fPIC \
	-Wl,--no-as-needed -lSceLibcInternal
imports() { check_link imports "$1" "$W/stubs"; }
expect pass "imports: title modules" imports "$W/ok"
expect fail "imports: libkernel_sys" imports "$W/sys"
expect fail "imports: a C++ runtime symbol from the system" imports "$W/cxx"
expect fail "imports: the C locale's table from the system" imports "$W/rune"

[ $rc = 0 ] && echo "check_link tests: ok" || echo "check_link tests: FAILED"
exit $rc
