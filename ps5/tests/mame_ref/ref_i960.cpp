// FBNeo PS5 2.3 tests: MAME's i960.cpp (unmodified, beside this file) built in namespace mame, so that it can be linked
// with the port's core, whose header declares the same register names (tests/test_cpus.cpp includes MAME's headers the
// same way). The headers MAME's file includes come first, outside the namespace.
// SPDX-License-Identifier: BSD-3-Clause
#include "emu.h"
#include "corefloat.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>

namespace mame {
#include "i960.cpp"
}
