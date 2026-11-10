<!-- SPDX-License-Identifier: MIT -->
# compiler-rt 18.1.8: emulated TLS and `__cpu_model`

From LLVM's own repository, tag `llvmorg-18.1.8`, `compiler-rt/lib/builtins/` (fetched 2026-11-07 from
`https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-18.1.8/compiler-rt/...`). Licence: Apache-2.0 WITH
LLVM-exception (`LICENSE.TXT`, the same file's text from `compiler-rt/LICENSE.TXT`), as for the SDK's libc++.

| file | upstream path | sha256 of the upstream file |
|---|---|---|
| `emutls.c` | `lib/builtins/emutls.c` | `a4437f3e329a0835adbc7df38e36df12f358d05c073b11813c409f849c2345d2` (changed: below) |
| `x86.c` | `lib/builtins/cpu_model/x86.c` | `76e43483ff498d8849b933d04e63d9710bca1ae7624057a044bb117b6477aa51` |
| `cpu_model.h` | `lib/builtins/cpu_model/cpu_model.h` | `cfb51c855bbffc0216e21f64a0f792d0354a6f07e693c56564a309bbe2e17757` |
| `int_lib.h` | `lib/builtins/int_lib.h` | `b31130f10484636895daa13b91b63d47ea82b45547f29e4dd136b48a67854d30` |
| `int_types.h` | `lib/builtins/int_types.h` | `26517abcfb05e989f4ac18b38779bc118637e0a5b3e44536c3733cc47b33026e` |
| `int_endianness.h` | `lib/builtins/int_endianness.h` | `ec5175561075845b7b16a92ad5ecdfd42a284de2736dd07e4ca7011b75c37709` |
| `int_util.h` | `lib/builtins/int_util.h` | `63d06198ab720a75c20bfd84be895c2746581b129168bae6201e7384bbb0473b` |

Why: the SDK compiles with `-femulated-tls`, so RADV's `thread_local` / `__thread` variables need
`__emutls_get_address`, and RADV's addrlib asks `__builtin_cpu_supports`, which reads `__cpu_model`. The console's
libraries export neither. (The payload SDK's `libc.a` has an `emutls.o`; it isn't used: this one comes from LLVM itself.)

One change, in `emutls.c`: the condition that delays freeing a thread's TLS block to the second pthread-key
destructor round also holds on the console (`|| defined(__PROSPERO__)`). The reason is upstream's own, written next to
it for Bionic: without `__cxa_thread_atexit_impl` (the console's libc doesn't export it), libc++abi runs `thread_local`
destructors from a pthread-key destructor, and they may still use TLS variables. If the console ran a single
destructor round, the block would stay allocated when its thread ends (a leak, never a use after free).
