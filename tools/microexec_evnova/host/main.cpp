// evnova_microexec_host: reads cases on stdin, prints one result line each.
//
//   describe                      list bound targets and reflected records,
//                                 then "end"
//   case <id> <target 0x...>      start a case (fresh GameState)
//   size <path> <count>           size a port table ("scenario.governments")
//   set <path> <value>            set a port field
//                                 ("scenario.governments[3].classes[0]")
//   arg <name> <value>            a target argument ("govt_a",
//   "ship.is_active")
//   arg-bytes <name> <hex|->     raw string bytes; - means empty
//   end                           run the port and print the
//   result
//
//   result <id> ok <value>
//   result <id> ok void                      the port function returns void:
//   only
//                                            the effect lines are meaningful
//   result <id> unbound <path> [<path>...]   paths with no setter; not run
//   result <id> no-adapter                   no port function cited for the
//   target result <id> error <message>              bad case (rules or decode
//   error)
//
// Before an `ok` result the host prints the port's side effects, one per line
// (none for other results; bridges that do not know them can skip every line
// that is not `result`):
//
//   write <path> <final> <initial>   a scalar the port stored to, or whose
//   value
//                                    differs after the call: its value after
//                                    the call and before it (equal when a store
//                                    rewrote the same value); per array element
//   read-unset <path>                a scalar the port read before writing it
//                                    that no `set` of the case assigned (it
//                                    saw the default-constructed value);
//                                    only with an instrumented build
//
// `describe` lists `trace instrumented` (loads and stores are traced, see
// trace.hpp) or `trace snapshot` (writes only, found by comparing memory).

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "host.hpp"
#include "trace.hpp"

namespace {

using microexec::Args;
using microexec::CaseError;
using microexec::Value;

[[nodiscard]] std::optional<Value> ParseValue(std::string_view text) {
  Value value;
  const auto *end = text.data() + text.size();
  if (const auto [ptr, ec] = std::from_chars(text.data(), end, value.integer);
      ec == std::errc{} && ptr == end) {
    return value;
  }
  const std::string copy(text);
  char *parsed = nullptr;
  value.real = std::strtod(copy.c_str(), &parsed);
  if (parsed != copy.c_str() + copy.size() || copy.empty()) {
    return std::nullopt;
  }
  value.is_real = true;
  return value;
}

struct Case {
  std::string id;
  std::uint32_t target = 0;
  std::unique_ptr<game::GameState> state = std::make_unique<game::GameState>();
  Args args;
  std::vector<std::string> set_paths;
  std::vector<std::string> unbound;
  std::optional<std::string> error;
};

void Apply(Case &c,
           std::string_view verb,
           std::string_view path,
           std::string_view operand) {
  if (verb == "arg-bytes") {
    Value value;
    value.bytes.emplace();
    if (operand != "-") {
      if (operand.empty() || operand.size() % 2 != 0) {
        throw CaseError("bad byte string for arg " + std::string(path));
      }
      for (std::size_t i = 0; i < operand.size(); i += 2) {
        unsigned int byte = 0;
        const auto *begin = operand.data() + i;
        const auto [end, ec] = std::from_chars(begin, begin + 2, byte, 16);
        if (ec != std::errc{} || end != begin + 2) {
          throw CaseError("bad byte string for arg " + std::string(path));
        }
        value.bytes->push_back(static_cast<char>(byte));
      }
    }
    c.args[std::string(path)] = std::move(value);
    c.set_paths.emplace_back(path);
    return;
  }
  if (verb == "arg") {
    const auto value = ParseValue(operand);
    if (!value) {
      throw CaseError("bad value for arg " + std::string(path));
    }
    c.args[std::string(path)] = *value;
    c.set_paths.emplace_back(path);
    return;
  }
  try {
    if (verb == "size") {
      const auto count = ParseValue(operand);
      if (!count || count->is_real || count->integer < 0) {
        throw CaseError("bad size for " + std::string(path));
      }
      microexec::SizePath(
          *c.state, path, static_cast<std::size_t>(count->integer));
      return;
    }
    if (verb == "set") {
      const auto value = ParseValue(operand);
      if (!value) {
        throw CaseError("bad value for " + std::string(path));
      }
      microexec::SetPath(*c.state, path, *value);
      c.set_paths.emplace_back(path);
      return;
    }
  } catch (const microexec::Unbound &unbound) {
    c.unbound.push_back(unbound.path);
    return;
  }
  throw CaseError("unknown verb " + std::string(verb));
}

// Runs the port; on success prints the trace lines and returns the result
// text (after "result <id> ").
std::string RunCase(const Case &c) {
  try {
    microexec::PrepareTrace(*c.state, c.set_paths);
    const auto result = microexec::RunTarget(c.target, *c.state, c.args);
    if (!result) {
      microexec::ResetTrace();
      return "no-adapter";
    }
    microexec::ReportTrace(*c.state, std::cout);
    return "ok " + *result;
  } catch (const CaseError &e) {
    microexec::ResetTrace();
    return std::string("error ") + e.what();
  } catch (const microexec::Unbound &unbound) {
    microexec::ResetTrace();
    return "unbound " + unbound.path;
  }
}

void Finish(const Case &c) {
  if (c.error) {
    std::cout << "result " << c.id << " error " << *c.error << '\n';
  } else if (!c.unbound.empty()) {
    std::cout << "result " << c.id << " unbound";
    for (const auto &path : c.unbound) {
      std::cout << ' ' << path;
    }
    std::cout << '\n';
  } else {
    const auto text = RunCase(c);
    std::cout << "result " << c.id << ' ' << text << '\n';
  }
  std::cout.flush();
}

} // namespace

int main() {
  std::optional<Case> current;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream words(line);
    std::string verb;
    std::string path;
    std::string operand;
    words >> verb >> path >> operand;
    if (verb.empty()) {
      continue;
    }
    if (verb == "describe") {
      microexec::DescribeHost(std::cout);
      std::cout << "end" << std::endl;
      continue;
    }
    if (verb == "case") {
      current.emplace();
      current->id = path;
      current->target =
          static_cast<std::uint32_t>(std::strtoul(operand.c_str(), nullptr, 0));
      continue;
    }
    if (!current) {
      std::cerr << "ignored outside a case: " << line << '\n';
      continue;
    }
    if (verb == "end") {
      Finish(*current);
      current.reset();
      continue;
    }
    if (current->error) {
      continue;
    }
    try {
      std::string extra;
      if (verb == "arg-bytes" && words >> extra) {
        throw CaseError("extra operand for byte string argument " + path);
      }
      Apply(*current, verb, path, operand);
    } catch (const CaseError &e) {
      current->error = e.what();
    }
  }
  return 0;
}
