#pragma once

// Generic port reflection for the microexec host. gen_reflection.py emits a
// Fields<T> table per port record and a Target per cited port function; the
// templates here walk a path through those tables, assign or size what it
// ends on, and call a port function by deducing each parameter's source from
// its type. Nothing in here names a port field or function.

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "game/game_state.hpp"
#include "host.hpp"
#include "trace.hpp"

namespace microexec {

// One step of a port path: `.name` or `[index]`.
struct Step {
  std::string_view field;
  std::size_t index = 0;
  bool is_index = false;
};

// "a.b[3].c" -> {a, b, [3], c}. A leading "." or "[" is allowed, so a record
// argument's remainder ("[2].x" or ".x") parses the same way.
[[nodiscard]] std::optional<std::vector<Step>> ParsePath(std::string_view path);

enum class OpKind { kSet, kSize };

struct Op {
  OpKind kind = OpKind::kSet;
  Value value;
  std::size_t count = 0;
};

// Primary template: a type with no generated table. Specializations (from
// port_reflection.gen.hpp) provide kName and Get().
template <typename T> struct Fields {};

template <typename T>
concept Reflected = requires { Fields<T>::Get(); };

template <typename T> struct FieldEntry {
  void (*walk)(T &, std::span<const Step>, const Op &);
  std::string (*kind)();
  void (*collect)(T &, const std::string &, LeafRuns &);
};

template <typename T>
using FieldTable = std::map<std::string_view, FieldEntry<T>, std::less<>>;

template <typename T> struct IsStdArray : std::false_type {};

template <typename E, std::size_t N>
struct IsStdArray<std::array<E, N>> : std::true_type {};

template <typename T> struct IsVector : std::false_type {};

template <typename E, typename A>
struct IsVector<std::vector<E, A>> : std::true_type {};

template <typename T>
concept Text =
    std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>;

template <typename T>
concept Scalar = std::is_arithmetic_v<T> || std::is_enum_v<T>;

template <typename T>
concept Indexable = IsStdArray<T>::value || IsVector<T>::value;

// A bridge value converted to a scalar port field. The original's byte flags
// become bools by testing against zero, as the original code does.
template <Scalar T> [[nodiscard]] T Convert(const Value &value) {
  if constexpr (std::is_same_v<T, bool>) {
    if (value.bytes) {
      throw CaseError("byte string value for a bool port field");
    }
    if (value.is_real) {
      throw CaseError("real value for a bool port field");
    }
    return value.integer != 0;
  } else if constexpr (std::is_enum_v<T>) {
    return static_cast<T>(Narrow<std::underlying_type_t<T>>(value));
  } else {
    return Narrow<T>(value);
  }
}

// The kind the host reports for a type in `describe` (bridge.py parses it).
template <typename T> [[nodiscard]] std::string KindOf() {
  using V = std::remove_cv_t<T>;
  if constexpr (Text<V>) {
    return "string";
  } else if constexpr (std::is_same_v<V, bool>) {
    return "bool";
  } else if constexpr (std::is_enum_v<V>) {
    return "enum:" + KindOf<std::underlying_type_t<V>>();
  } else if constexpr (std::is_integral_v<V>) {
    return std::format("{}{}", std::is_signed_v<V> ? 'i' : 'u', sizeof(V) * 8);
  } else if constexpr (std::is_floating_point_v<V>) {
    return std::format("f{}", sizeof(V) * 8);
  } else if constexpr (IsStdArray<V>::value) {
    return std::format(
        "array:{}:{}", std::tuple_size_v<V>, KindOf<typename V::value_type>());
  } else if constexpr (IsVector<V>::value) {
    return "vector:" + KindOf<typename V::value_type>();
  } else if constexpr (Reflected<V>) {
    return "record:" + std::string(Fields<V>::kName);
  } else {
    return "other";
  }
}

// --- Scalar leaf map (access tracing) ---------------------------------------

// A scalar's value as the text the bridge parses: integers exactly, floats
// as the shortest round-trip double, bools as 0/1, enums as their integers.
template <typename T>
[[nodiscard]] std::string FormatLeaf(const std::byte *bytes) {
  T value;
  std::memcpy(&value, bytes, sizeof(T));
  if constexpr (std::is_same_v<T, bool>) {
    return value ? "1" : "0";
  } else if constexpr (std::is_enum_v<T>) {
    return FormatLeaf<std::underlying_type_t<T>>(bytes);
  } else if constexpr (std::is_integral_v<T>) {
    return std::is_signed_v<T>
               ? std::to_string(static_cast<std::int64_t>(value))
               : std::to_string(static_cast<std::uint64_t>(value));
  } else if constexpr (sizeof(T) == sizeof(float)) {
    return std::format("{}", static_cast<float>(value));
  } else {
    return std::format("{}", static_cast<double>(value));
  }
}

inline void AddRun(LeafRuns &runs,
                   const std::string &path,
                   const void *base,
                   std::size_t element_size,
                   std::size_t count,
                   bool indexed,
                   std::string kind,
                   std::string (*format)(const std::byte *)) {
  if (count == 0) {
    return;
  }
  LeafRun run{.path = path,
              .base = reinterpret_cast<std::uintptr_t>(base),
              .element_size = element_size,
              .count = count,
              .indexed = indexed,
              .kind = std::move(kind),
              .format = format};
  run.initial.resize(element_size * count);
  std::memcpy(run.initial.data(), base, run.initial.size());
  run.flags.assign(count, 0);
  runs.push_back(std::move(run));
}

// Appends a run for every scalar reachable from `value`: a contiguous scalar
// array or vector is one run, each record field its own. Only pre-call
// addresses are meaningful afterwards (a vector the port grows moves).
template <typename T>
void Collect(T &value, const std::string &path, LeafRuns &runs) {
  using V = std::remove_cv_t<T>;
  if constexpr (Scalar<V>) {
    AddRun(runs,
           path,
           std::addressof(value),
           sizeof(V),
           1,
           false,
           KindOf<V>(),
           &FormatLeaf<V>);
  } else if constexpr (Indexable<V>) {
    using E = std::remove_cv_t<typename V::value_type>;
    if constexpr (std::is_same_v<V, std::vector<bool>>) {
      return;
    } else if constexpr (Scalar<E>) {
      AddRun(runs,
             path,
             value.data(),
             sizeof(E),
             value.size(),
             true,
             KindOf<E>(),
             &FormatLeaf<E>);
    } else {
      for (std::size_t i = 0; i < value.size(); ++i) {
        Collect(value[i], std::format("{}[{}]", path, i), runs);
      }
    }
  } else if constexpr (Reflected<V>) {
    for (const auto &[name, entry] : Fields<V>::Get()) {
      entry.collect(value,
                    path.empty() ? std::string(name)
                                 : path + "." + std::string(name),
                    runs);
    }
  }
}

template <typename Container>
[[nodiscard]] auto &At(Container &container, std::size_t index) {
  if (index >= container.size()) {
    throw CaseError(std::format("index {} is past the sized table ({} entries)",
                                index,
                                container.size()));
  }
  return container[index];
}

template <typename T> void Leaf(T &target, const Op &op) {
  using V = std::remove_cv_t<T>;
  constexpr bool kWritable = !std::is_const_v<T>;
  if (op.kind == OpKind::kSet) {
    if constexpr (kWritable && Scalar<V>) {
      target = Convert<V>(op.value);
      return;
    }
  } else if constexpr (IsStdArray<V>::value) {
    if (op.count != std::tuple_size_v<V>) {
      throw CaseError(std::format("the port table is fixed at {} entries",
                                  std::tuple_size_v<V>));
    }
    return;
  } else if constexpr (kWritable && IsVector<V>::value) {
    if constexpr (requires { target.resize(op.count); }) {
      target.resize(op.count);
      return;
    }
  }
  throw Unbound{};
}

// Follows `rest` from `target` and applies `op` where it ends. A path the
// tables cannot follow, or a leaf of an unsupported type, throws Unbound.
template <typename T>
void Walk(T &target, std::span<const Step> rest, const Op &op) {
  using V = std::remove_cv_t<T>;
  if (rest.empty()) {
    Leaf(target, op);
    return;
  }
  const Step &step = rest.front();
  if constexpr (Indexable<V>) {
    if (step.is_index) {
      Walk(At(target, step.index), rest.subspan(1), op);
      return;
    }
  } else if constexpr (Reflected<V>) {
    if (!step.is_index) {
      const auto &table = Fields<V>::Get();
      if (const auto it = table.find(step.field); it != table.end()) {
        it->second.walk(target, rest.subspan(1), op);
        return;
      }
    }
  }
  throw Unbound{};
}

template <typename T> void DescribeRecord(std::ostream &out) {
  out << "record " << Fields<T>::kName << '\n';
  for (const auto &[name, entry] : Fields<T>::Get()) {
    out << "field " << name << ' ' << entry.kind() << '\n';
  }
}

// --- Calling a port function -----------------------------------------------

template <typename F> struct FnTraits;

template <typename R, typename... P> struct FnTraits<R (*)(P...)> {
  using Return = R;
  using Params = std::tuple<P...>;
};

template <typename R, typename... P>
struct FnTraits<R (*)(P...) noexcept> : FnTraits<R (*)(P...)> {};

// A record parameter the host builds fresh from `<name>.<path>` arguments.
// A concept conjunction, so the trait is never asked of an incomplete type.
template <typename V>
concept Buildable = Reflected<V> && std::default_initializable<V>;

template <typename V>
concept Context =
    std::is_same_v<V, game::GameState> || std::is_same_v<V, game::ScenarioData>;

// How a parameter of type P is supplied, or why it cannot be: the whole
// GameState (or its scenario) the bridge populated; a fresh record built from
// the `<name>.<path>` arguments; or one scalar argument.
template <typename P> [[nodiscard]] constexpr std::string_view ParamProblem() {
  using V = std::remove_cvref_t<P>;
  constexpr bool kRef = std::is_lvalue_reference_v<P>;
  constexpr bool kConstRef =
      kRef && std::is_const_v<std::remove_reference_t<P>>;
  if constexpr (std::is_pointer_v<V>) {
    return "pointer parameter";
  } else if constexpr (Context<V>) {
    return kRef ? "" : "context passed by value";
  } else if constexpr (Scalar<V>) {
    return !kRef || kConstRef ? "" : "scalar out-parameter";
  } else if constexpr (Text<V>) {
    return !kRef || kConstRef ? "" : "string out-parameter";
  } else if constexpr (Buildable<V>) {
    return "";
  } else {
    return "unsupported parameter type";
  }
}

template <typename P> [[nodiscard]] std::string ParamKind() {
  using V = std::remove_cvref_t<P>;
  if constexpr (std::is_same_v<V, game::GameState>) {
    return "state";
  } else if constexpr (std::is_same_v<V, game::ScenarioData>) {
    return "scenario";
  } else if constexpr (!ParamProblem<P>().empty()) {
    return "unsupported";
  } else {
    return KindOf<V>();
  }
}

template <typename R> [[nodiscard]] constexpr std::string_view ReturnProblem() {
  if constexpr (std::is_void_v<R>) {
    return "";
  } else if constexpr (!Scalar<std::remove_cvref_t<R>>) {
    return "unsupported return type";
  } else {
    return "";
  }
}

template <typename P> class Param {
public:
  using V = std::remove_cvref_t<P>;

  Param(game::GameState &state,
        const Args &args,
        std::string_view name,
        std::vector<std::string_view> &used) {
    if constexpr (std::is_same_v<V, game::GameState>) {
      context_ = &state;
    } else if constexpr (std::is_same_v<V, game::ScenarioData>) {
      context_ = &state.scenario;
    } else if constexpr (Text<V>) {
      const auto it = args.find(name);
      if (it == args.end() || !it->second.bytes) {
        throw CaseError("missing byte string argument " + std::string(name));
      }
      used.push_back(it->first);
      text_ = *it->second.bytes;
    } else if constexpr (Scalar<V>) {
      const auto it = args.find(name);
      if (it == args.end()) {
        throw CaseError("missing argument " + std::string(name));
      }
      used.push_back(it->first);
      owned_ = Convert<V>(it->second);
    } else {
      owned_.emplace();
      for (const auto &[key, value] : args) {
        if (key == name) {
          throw CaseError("scalar argument " + key + " for a record parameter");
        }
        if (!key.starts_with(name) || key.size() == name.size() ||
            (key[name.size()] != '.' && key[name.size()] != '[')) {
          continue;
        }
        used.push_back(key);
        const auto steps = ParsePath(std::string_view(key).substr(name.size()));
        if (!steps) {
          throw CaseError("bad argument path " + key);
        }
        try {
          Walk(*owned_, *steps, Op{OpKind::kSet, value, 0});
        } catch (const Unbound &) {
          throw Unbound{key};
        }
      }
    }
  }

  // Appends the leaves of a record parameter the host built, under the
  // argument's name (the path the bridge uses for its inputs).
  void CollectLeaves(std::string_view name, LeafRuns &runs) {
    if constexpr (!Context<V> && !Scalar<V> && !Text<V>) {
      Collect(*owned_, std::string(name), runs);
    }
  }

  [[nodiscard]] P Get() {
    if constexpr (Context<V>) {
      return static_cast<P>(*context_);
    } else if constexpr (std::is_same_v<V, std::string_view>) {
      view_ = text_;
      return static_cast<P>(view_);
    } else if constexpr (std::is_same_v<V, std::string>) {
      return static_cast<P>(text_);
    } else {
      return static_cast<P>(*owned_);
    }
  }

private:
  std::string text_;
  std::string_view view_;
  V *context_ = nullptr;
  std::optional<V> owned_;
};

template <typename R> [[nodiscard]] std::string Format(R result) {
  using V = std::remove_cvref_t<R>;
  if constexpr (std::is_same_v<V, bool>) {
    return result ? "1" : "0";
  } else if constexpr (std::is_enum_v<V>) {
    return std::to_string(static_cast<std::int64_t>(result));
  } else if constexpr (std::is_integral_v<V>) {
    return std::to_string(static_cast<std::int64_t>(result));
  } else {
    return std::format("{}", static_cast<double>(result));
  }
}

template <auto Fn> [[nodiscard]] std::string_view Problem() {
  using Traits = FnTraits<decltype(Fn)>;
  if (const auto problem = ReturnProblem<typename Traits::Return>();
      !problem.empty()) {
    return problem;
  }
  return []<std::size_t... I>(std::index_sequence<I...>) {
    std::string_view problem;
    ((problem =
          problem.empty()
              ? ParamProblem<std::tuple_element_t<I, typename Traits::Params>>()
              : problem),
     ...);
    return problem;
  }(std::make_index_sequence<std::tuple_size_v<typename Traits::Params>>{});
}

// Calls the port function with parameters built from `state` and `args`;
// every argument must be consumed by some parameter.
template <auto Fn>
std::string Run(game::GameState &state,
                const Args &args,
                std::span<const std::string_view> names) {
  using Traits = FnTraits<decltype(Fn)>;
  using Params = typename Traits::Params;
  if (const auto problem = Problem<Fn>(); !problem.empty()) {
    throw CaseError("port signature not supported: " + std::string(problem));
  }
  if constexpr (!ReturnProblem<typename Traits::Return>().empty()) {
    return {};
  } else {
    return [&]<std::size_t... I>(std::index_sequence<I...>) -> std::string {
      if constexpr ((ParamProblem<std::tuple_element_t<I, Params>>().empty() &&
                     ...)) {
        std::vector<std::string_view> used;
        std::tuple<Param<std::tuple_element_t<I, Params>>...> params{
            Param<std::tuple_element_t<I, Params>>(
                state, args, names[I], used)...};
        for (const auto &[key, value] : args) {
          if (std::find(used.begin(), used.end(), key) == used.end()) {
            throw CaseError("argument " + key + " matches no port parameter");
          }
        }
        LeafRuns argument_leaves;
        (std::get<I>(params).CollectLeaves(names[I], argument_leaves), ...);
        AddTraceRuns(std::move(argument_leaves));
        std::string result;
        {
          const TraceWindow window;
          if constexpr (std::is_void_v<typename Traits::Return>) {
            Fn(std::get<I>(params).Get()...);
            result = "void";
          } else {
            result = Format(Fn(std::get<I>(params).Get()...));
          }
        }
        LeafRuns final_leaves;
        (std::get<I>(params).CollectLeaves(names[I], final_leaves), ...);
        CaptureTraceRuns(std::move(final_leaves));
        return result;
      } else {
        return {};
      }
    }(std::make_index_sequence<std::tuple_size_v<Params>>{});
  }
}

// One `target` line: address, name, return kind, then `name:kind` per
// parameter, or `unsupported <reason>`.
template <auto Fn>
void Describe(std::ostream &out, std::span<const std::string_view> names) {
  using Traits = FnTraits<decltype(Fn)>;
  using Params = typename Traits::Params;
  if (const auto problem = Problem<Fn>(); !problem.empty()) {
    out << " unsupported " << problem;
    return;
  }
  if constexpr (ReturnProblem<typename Traits::Return>().empty()) {
    if constexpr (std::is_void_v<typename Traits::Return>) {
      out << " returns void";
    } else {
      out << " returns "
          << KindOf<std::remove_cvref_t<typename Traits::Return>>();
    }
  }
  [&]<std::size_t... I>(std::index_sequence<I...>) {
    ((out << ' ' << names[I] << ':'
          << ParamKind<std::tuple_element_t<I, Params>>()),
     ...);
  }(std::make_index_sequence<std::tuple_size_v<Params>>{});
}

struct Target {
  std::uint32_t addr;
  std::string_view name;
  std::span<const std::string_view> params;
  std::string (*run)(game::GameState &,
                     const Args &,
                     std::span<const std::string_view>);
  void (*describe)(std::ostream &, std::span<const std::string_view>);
};

// A cited function the host cannot call at all, and why (listed by describe).
struct UnboundTarget {
  std::uint32_t addr;
  std::string_view name;
  std::string_view why;
};

// Defined in the generated port_reflection.gen.cpp.
std::span<const Target> Targets();
std::span<const UnboundTarget> UnboundTargets();
void DescribeRecords(std::ostream &out);

} // namespace microexec
