// Access tracing: the SanitizerCoverage hooks and the trace report
// (trace.hpp). This file is never itself instrumented.

#include "trace.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "port_reflection.gen.hpp"

namespace microexec {

namespace {

// The hooks are plain C functions, so the armed map is process state. Only
// the host's single thread touches it.
struct TraceState {
  bool armed = false;
  bool in_hook = false;
  LeafRuns runs;     // sorted by base
  LeafRuns captured; // after the call: record arguments
  std::unordered_set<std::string> set_paths;
};

TraceState g_trace;

void Record(const void *address, std::size_t size, bool is_store) {
  auto &trace = g_trace;
  if (!trace.armed || trace.in_hook) {
    return;
  }
  trace.in_hook = true;
  const auto at = reinterpret_cast<std::uintptr_t>(address);
  const auto next = std::upper_bound(
      trace.runs.begin(),
      trace.runs.end(),
      at,
      [](std::uintptr_t a, const LeafRun &run) { return a < run.base; });
  if (next != trace.runs.begin()) {
    auto &run = *(next - 1);
    const auto offset = at - run.base;
    if (size <= run.element_size &&
        offset + size <= run.element_size * run.count) {
      auto &flags = run.flags[offset / run.element_size];
      if (is_store) {
        flags |= LeafRun::kWritten;
      } else if ((flags & LeafRun::kWritten) == 0) {
        flags |= LeafRun::kRead;
      }
    }
  }
  trace.in_hook = false;
}

} // namespace

std::string LeafRun::ElementPath(std::size_t index) const {
  return indexed ? std::format("{}[{}]", path, index) : path;
}

std::string CanonicalPath(std::string_view path) {
  const auto steps = ParsePath(path);
  if (!steps || steps->empty()) {
    return {};
  }
  std::string out;
  for (const auto &step : *steps) {
    if (step.is_index) {
      out += std::format("[{}]", step.index);
    } else {
      out +=
          out.empty() ? std::string(step.field) : "." + std::string(step.field);
    }
  }
  return out;
}

void PrepareTrace(game::GameState &state,
                  const std::vector<std::string> &set_paths) {
  ResetTrace();
  Collect(state, "", g_trace.runs);
  std::ranges::sort(g_trace.runs, {}, &LeafRun::base);
  for (const auto &path : set_paths) {
    g_trace.set_paths.insert(CanonicalPath(path));
  }
}

void AddTraceRuns(LeafRuns runs) {
  for (auto &run : runs) {
    g_trace.runs.push_back(std::move(run));
  }
  std::ranges::sort(g_trace.runs, {}, &LeafRun::base);
}

void CaptureTraceRuns(LeafRuns runs) { g_trace.captured = std::move(runs); }

void ResetTrace() {
  g_trace.armed = false;
  g_trace.captured.clear();
  g_trace.runs.clear();
  g_trace.set_paths.clear();
}

TraceWindow::TraceWindow() {
  g_trace.armed = kTraceInstrumented && !g_trace.runs.empty();
}

TraceWindow::~TraceWindow() { g_trace.armed = false; }

void ReportTrace(game::GameState &state, std::ostream &out) {
  LeafRuns current;
  Collect(state, "", current);
  for (auto &run : g_trace.captured) {
    current.push_back(std::move(run));
  }
  std::unordered_map<std::string_view, const LeafRun *> by_path;
  for (const auto &run : current) {
    by_path.emplace(run.path, &run);
  }
  for (const auto &run : g_trace.runs) {
    const auto found = by_path.find(run.path);
    if (found == by_path.end()) {
      continue;
    }
    const auto &now = *found->second;
    const auto count = std::min(run.count, now.count);
    for (std::size_t i = 0; i < count; ++i) {
      const auto *before = run.initial.data() + i * run.element_size;
      const auto *after = now.initial.data() + i * run.element_size;
      const auto flags = run.flags[i];
      const auto changed = std::memcmp(before, after, run.element_size) != 0;
      if (changed || (flags & LeafRun::kWritten) != 0) {
        out << "write " << run.ElementPath(i) << ' ' << run.format(after) << ' '
            << run.format(before) << '\n';
      }
      if ((flags & LeafRun::kRead) != 0 &&
          !g_trace.set_paths.contains(run.ElementPath(i))) {
        out << "read-unset " << run.ElementPath(i) << '\n';
      }
    }
  }
  ResetTrace();
}

} // namespace microexec

// SanitizerCoverage hooks (-fsanitize-coverage=trace-loads,trace-stores).
extern "C" {
void __sanitizer_cov_load1(const void *a) { microexec::Record(a, 1, false); }

void __sanitizer_cov_load2(const void *a) { microexec::Record(a, 2, false); }

void __sanitizer_cov_load4(const void *a) { microexec::Record(a, 4, false); }

void __sanitizer_cov_load8(const void *a) { microexec::Record(a, 8, false); }

void __sanitizer_cov_load16(const void *a) { microexec::Record(a, 16, false); }

void __sanitizer_cov_store1(const void *a) { microexec::Record(a, 1, true); }

void __sanitizer_cov_store2(const void *a) { microexec::Record(a, 2, true); }

void __sanitizer_cov_store4(const void *a) { microexec::Record(a, 4, true); }

void __sanitizer_cov_store8(const void *a) { microexec::Record(a, 8, true); }

void __sanitizer_cov_store16(const void *a) { microexec::Record(a, 16, true); }
}
