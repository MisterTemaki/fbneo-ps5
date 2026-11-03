#!/bin/sh
# FBNeo PS5 2.2: runs the four builds of the MIPS III test (Makefile, test-mips3): each must pass its 43 known answers,
# and all four must print the same results for the random programs.
#
#   run_mips3.sh <build/host>
#
# SPDX-License-Identifier: MIT
out=$1
fail=0
for v in c1i1 c1i0 c0i1 c0i0; do
	ASAN_OPTIONS=detect_leaks=1 "$out/mips3-test-$v" >"$out/mips3-$v.txt" 2>&1 || fail=1
	grep '^random' "$out/mips3-$v.txt" | sed 's/, [0-9]* exception(s)$/&/' >"$out/mips3-$v.random"
	if grep -q '^mips3 test: 43 known answers ok, 0 failed' "$out/mips3-$v.txt"; then
		echo "  ok: mips3 build $v: 43 known answers"
	else
		echo "  FAIL: mips3 build $v: $(grep -c ' FAIL' "$out/mips3-$v.txt") known answer(s) failed:"
		grep ' FAIL' "$out/mips3-$v.txt" | sed 's/^/    /'
		fail=1
	fi
done
n=$(wc -l <"$out/mips3-c1i1.random")
same=1
for v in c1i0 c0i1 c0i0; do
	cmp -s "$out/mips3-c1i1.random" "$out/mips3-$v.random" || { same=0; echo "    $v differs:"; diff "$out/mips3-c1i1.random" "$out/mips3-$v.random" | head -5; }
done
if [ "$n" = 20 ] && [ $same = 1 ]; then
	echo "  ok: mips3: 20 random programs give the same registers and memory in all four builds"
else
	echo "  FAIL: mips3: the random programs ($n) differ between the builds"
	fail=1
fi
exit $fail
