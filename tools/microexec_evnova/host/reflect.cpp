// Path parsing and the host entry points over the generated reflection
// tables (port_reflection.gen.*, from gen_reflection.py).

#include <charconv>
#include <string>

#include "port_reflection.gen.hpp"

namespace microexec {

std::optional<std::vector<Step>> ParsePath(std::string_view path) {
  std::vector<Step> steps;
  while (!path.empty()) {
    if (path.front() == '.') {
      path.remove_prefix(1);
      continue;
    }
    if (path.front() == '[') {
      const auto close = path.find(']');
      if (close == std::string_view::npos) {
        return std::nullopt;
      }
      Step step{.is_index = true};
      const auto digits = path.substr(1, close - 1);
      const auto [ptr, ec] = std::from_chars(
          digits.data(), digits.data() + digits.size(), step.index);
      if (ec != std::errc{} || ptr != digits.data() + digits.size()) {
        return std::nullopt;
      }
      steps.push_back(step);
      path.remove_prefix(close + 1);
      continue;
    }
    const auto end = path.find_first_of(".[");
    steps.push_back(Step{.field = path.substr(0, end)});
    path.remove_prefix(end == std::string_view::npos ? path.size() : end);
  }
  return steps;
}

namespace {

void Apply(game::GameState &state, std::string_view path, const Op &op) {
  const auto steps = ParsePath(path);
  if (!steps || steps->empty()) {
    throw CaseError("bad path " + std::string(path));
  }
  try {
    Walk(state, *steps, op);
  } catch (const Unbound &) {
    throw Unbound{std::string(path)};
  }
}

} // namespace

void SetPath(game::GameState &state,
             std::string_view path,
             const Value &value) {
  Apply(state, path, Op{OpKind::kSet, value, 0});
}

void SizePath(game::GameState &state,
              std::string_view path,
              std::size_t count) {
  Apply(state, path, Op{OpKind::kSize, {}, count});
}

std::optional<std::string>
RunTarget(std::uint32_t target, game::GameState &state, const Args &args) {
  for (const auto &entry : Targets()) {
    if (entry.addr == target) {
      return entry.run(state, args, entry.params);
    }
  }
  return std::nullopt;
}

void DescribeHost(std::ostream &out) {
  out << "root " << Fields<game::GameState>::kName << '\n';
  out << "trace " << (kTraceInstrumented ? "instrumented" : "snapshot") << '\n';
  for (const auto &entry : Targets()) {
    out << std::format("target 0x{:08x} {}", entry.addr, entry.name);
    entry.describe(out, entry.params);
    out << '\n';
  }
  for (const auto &entry : UnboundTargets()) {
    out << std::format("target 0x{:08x} {} unsupported {}\n",
                       entry.addr,
                       entry.name,
                       entry.why);
  }
  DescribeRecords(out);
}

} // namespace microexec
