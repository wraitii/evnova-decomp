#pragma once

// Native host that runs C++ port functions on state translated from a
// microexec run of the original (../microexec/src/bridge.py). It only knows
// port-side names: the bridge maps Ghidra slot keys to GameState paths, which
// reflect.hpp walks through the field tables gen_reflection.py generates from
// the port headers; the generated target table calls the port function cited
// for an original address. See tools/microexec_evnova/README.md.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "game/game_state.hpp"

namespace microexec {

// A value from the bridge. Integers are carried exactly; floats keep double
// precision (the bridge decodes float/double slots before sending them).
struct Value {
  std::int64_t integer = 0;
  double real = 0.0;
  bool is_real = false;
  std::optional<std::string> bytes;
};

// A case the host cannot run as described: an unknown path, an index past a
// sized table, or a value that does not fit the port field. These are bridge
// or rules errors, never port behaviour, so they are reported separately.
struct CaseError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// A path the reflection tables cannot follow or assign (a field the port
// lacks, or one of a type the host cannot set). Reported as `unbound`.
struct Unbound {
  std::string path;
};

using Args = std::map<std::string, Value, std::less<>>;

// Narrows a bridge value into a port field type, rejecting values that do not
// fit (a rules or decode error would otherwise wrap silently).
template <typename T> [[nodiscard]] T Narrow(const Value &value) {
  static_assert(!std::is_same_v<T, bool>, "use Convert for bools");
  if (value.bytes) {
    throw CaseError("byte string value for a numeric port field");
  }
  if constexpr (std::is_floating_point_v<T>) {
    return static_cast<T>(value.is_real ? value.real
                                        : static_cast<double>(value.integer));
  } else {
    if (value.is_real || !std::in_range<T>(value.integer)) {
      throw CaseError("value does not fit the port field");
    }
    return static_cast<T>(value.integer);
  }
}

// Sets or sizes the GameState member at a port path
// ("scenario.governments[3].classes[0]"). Throws Unbound when the path does
// not resolve to a settable field (or a sizable table).
void SetPath(game::GameState &state, std::string_view path, const Value &value);
void SizePath(game::GameState &state, std::string_view path, std::size_t count);

// Runs the port of the original function at `target`, returning the result
// as the text the bridge compares ("1"/"0" for bool, decimal otherwise), or
// nullopt when no port function is cited for `target`.
std::optional<std::string>
RunTarget(std::uint32_t target, game::GameState &state, const Args &args);

// Prints every bound target and every reflected record (the `describe` verb).
void DescribeHost(std::ostream &out);

} // namespace microexec
