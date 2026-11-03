MAME's i960 and MB86233 cores, unmodified (mamedev/mame b7f483f, src/devices/cpu/i960/i960.cpp/.h and
src/devices/cpu/mb86233/mb86233.cpp/.h, BSD-3-Clause, Farfetch'd, R. Belmont, Olivier Galibert), with a minimal emu.h
so they build on their own: tests/test_cpus.cpp runs them beside the port's cores (src/cpu/i960, src/cpu/mb86233) on
the same programs and compares the results. Not part of the app.
