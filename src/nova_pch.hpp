// Precompiled header for evnova_runtime (EVNOVA_ENABLE_PCH; see root
// CMakeLists).
//
// Contains only third-party and standard-library headers that appear in a large
// fraction of translation units. Project headers are deliberately excluded:
// they change often and would invalidate the PCH on every edit, turning
// incremental builds into full rebuilds.
//
// Chosen from a -ftime-trace survey of all 121 TUs (see analysis notes): libc++
// headers accounted for ~84% of header self-time, dominated by <string>,
// <string_view>, <vector> (which drags in the C++23 <format> formatter stack
// and Unicode tables), <filesystem> (via <iomanip> into <locale>), and
// <random>. fmt/format.h reaches every TU through log.hpp, and SDL3/SDL.h is in
// ~32 TUs.
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <fmt/format.h>
