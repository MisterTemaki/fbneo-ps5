# Netplay validation — 2.4.0-netplay1

This development build is based on commit `4bb1fa58b4400ba6b03e75c5b6b6276f4eeeec80`. It implements direct two-console netplay; it has not been tested between physical PS5 consoles.

## Completed checks

| Check | Result |
| --- | --- |
| Native PS5 build, SDK 0.45 and Clang/LLVM 18 | Passed; signed native application container integrity check passed. |
| Native import/runtime preflight | Passed the project's import checker during linkage. |
| Payload | ELF64 x86-64 PIE, executable entry point, three load segments aligned to at least 16 KiB, no Linux interpreter. |
| Embedded application files | All six match the built files byte for byte. The two backgrounds share one packed image. |
| Source fingerprint | Delivery checkout and Linux build tree match: `ca0a414aec0be5bf7a403ba2b73d17d310838292e28298e8cbd096ded425700d`. |
| Network module | 35 checks passed with ASan/UBSan over real TCP loopback. |
| Complete frontend integration | Two host processes connected through the actual menu, synchronized the actual FBNeo core, executed rollback, paused/resumed, refused online state saving, disconnected and restored their offline states. Both exited with status 0; no ASan/UBSan report. |
| Existing offline frontend tests | Groups 1, 2, 6, 29 and 32: 95 checks passed (picture orientation/scaling, controls, save/load state, menu navigation, translations, network-share settings and clean exit). |
| Existing compressed state API | Ponpoko test state replay produced the same picture hash. |

The complete frontend test uses the repository's stand-in Ponpoko set with its homebrew Z80 test program. It does not use or distribute commercial game ROMs. It does not establish compatibility with every game or driver.

The network checks cover initial state synchronization, 480-frame delayed-input convergence against an offline reference, real rollback/replay, periodic state verification, divergent cores, pause/disconnect, mismatched builds/content, partial one-MiB transfers, state-size bounds, changing state layout, occupied ports, oversized packets and excessive message rates.

## Reproduce on Linux

From `ps5/`, with the host dependencies installed:

```sh
make test-netplay
make headless build/host/fbneo-ps5-host
make test-netplay-frontend
ONLY="1 2 6 29 32" bash tests/run-tests.sh
```

The recorded host build used `HOST_CC=clang-18`, `HOST_CXX='clang++-18 -Wno-c++11-narrowing'`, `HOST_CXX_NATIVE=clang++-18` and `AR=llvm-ar-18`. The existing core contains GNU C++11 narrowing initializers, so the additional host Clang flag is needed. The PS5 build already has that exception. A host-only unused constant is annotated and the archive namespace check ignores Clang ASan's common bookkeeping symbol; neither adjustment changes emulation behavior.

The normal native build is `make -j6 ps5 PS5_PAYLOAD_SDK=/absolute/path/to/sdk`, with LLVM 18 on PATH. No private SDK or release-signing key was added.

## Test on hardware

Use the **same** ELF on both PS5 consoles, matching ROM/BIOS files and DIP settings. Start on a LAN, following [the setup guide](NETPLAY.md). Verify connection, both controller roles, coins/start/buttons, audio, pause/resume, disconnection and preservation of offline saves. Then test Internet connectivity and latency separately. Native socket permissions and firmware-specific behavior remain unverified until this test is performed.

## Artifact

- File: `FBNeoPS5-v2.4.0-netplay1.elf`
- Size: 24,182,792 bytes
- SHA-256: `72b028b90711cf25af322409bd5c75cbe18e457caefdcd2fdccc97b2aee95f1a`
- Title ID: `PPSA99924`
- Internal development version: `2.4.0-netplay1`

This is a console-test build, not a claim of production-ready netplay or compatibility with RetroArch rooms.
