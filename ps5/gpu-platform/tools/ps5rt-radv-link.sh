#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 MisterTemaki
#
# What a RADV title's link needs from ps5-native-runtime-mit's platform layer.
#
#   ps5rt-radv-link.sh --flags [--own-stdio-wrap]   the linker flags: --wrap=pthread_create (src/ps5rt_thread.c),
#                                                   --wrap=fclose --wrap=fflush (open_memstream, src/ps5rt_stdio_wrap.c).
#                                                   With --own-stdio-wrap, only the first: the program wraps fclose /
#                                                   fflush itself and its wrappers call ps5rt_fclose / ps5rt_fflush.
#   ps5rt-radv-link.sh --scripts                    the linker script and version script (platform/link: MIT):
#                                                   -T .../ps5rt-pie.ld --version-script .../ps5rt-local.map
#   ps5rt-radv-link.sh --archive                    the platform archive (build/libps5rt_platform.a, or $PS5RT_PLATFORM_LIB)
#   ps5rt-radv-link.sh --libc-allow                 the libc.a members such a link may take (platform/libc-allow.txt)
#   ps5rt-radv-link.sh --notices                    their notices, to ship with the program (platform/libc-notices.txt)
#
# The link (ld.lld -m elf_x86_64 -pie -z max-page-size=0x4000 --hash-style=gnu --eh-frame-hdr -e _start
# --no-dynamic-linker, and --scripts), inputs in this order:
#   1. the program's objects, with ps5rt_start.o and ps5rt_new.o (ps5-native-runtime-mit)
#   2. RADV (libvulkan_radeon.ps5.a) and the program's other archives
#   3. the platform archive (--archive): before the system stubs, so that its definitions win
#   4. the SDK's libc++.a, libc++abi.a, libunwind.a (the eboot's own C++ runtime, whole)
#   5. --as-needed and the SDK's system-module stubs, never libkernel_sys / libkernel_web
#   6. the SDK's libc.a (only the members of --libc-allow may be loaded: tools/check_link.py trace), -lpthread
# and the flags of --flags anywhere before the inputs.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$here/.." && pwd)
own_stdio=0
what=
for arg in "$@"; do
	case $arg in
	--own-stdio-wrap) own_stdio=1 ;;
	--flags | --scripts | --archive | --libc-allow | --notices) what=$arg ;;
	*) echo "ps5rt-radv-link.sh: unknown argument $arg" >&2; exit 2 ;;
	esac
done
case $what in
--flags)
	if [ $own_stdio = 1 ]; then
		echo "--wrap=pthread_create"
	else
		echo "--wrap=pthread_create --wrap=fclose --wrap=fflush"
	fi
	;;
--scripts) echo "-T $here/link/ps5rt-pie.ld --version-script $here/link/ps5rt-local.map" ;;
--archive) echo "${PS5RT_PLATFORM_LIB:-$root/build/libps5rt_platform.a}" ;;
--libc-allow) echo "$here/libc-allow.txt" ;;
--notices) echo "$here/libc-notices.txt" ;;
*) echo "usage: ps5rt-radv-link.sh --flags [--own-stdio-wrap] | --scripts | --archive | --libc-allow | --notices" >&2; exit 2 ;;
esac
