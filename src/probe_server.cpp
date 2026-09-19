#include "probe_server.hpp"
#include "game/mission_trace.hpp"
#include "game/nova_font.hpp"
#include "log.hpp"

#include <SDL3/SDL.h>

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <utility>

namespace {

// Bounded waits so a paused main thread cannot hang the server thread.
constexpr auto kJobWait = std::chrono::seconds(10);
constexpr auto kFrameWait = std::chrono::seconds(2);

void WriteAll(int fd, const std::string &data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
    if (n <= 0) {
      return;
    }
    sent += static_cast<std::size_t>(n);
  }
}

// Minimal URL decoding for query values (%XX and '+').
std::string UrlDecode(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '+') {
      out += ' ';
    } else if (in[i] == '%' && i + 2 < in.size()) {
      const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
          return c - '0';
        if (c >= 'a' && c <= 'f')
          return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
          return c - 'A' + 10;
        return -1;
      };
      const int hi = hex(in[i + 1]);
      const int lo = hex(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>((hi << 4) | lo);
        i += 2;
        continue;
      }
      out += in[i];
    } else {
      out += in[i];
    }
  }
  return out;
}

// Extracts "name":"value" / "name":123 scalars from a small JSON body. The
// command bodies are flat and machine-generated, so substring scans are
// sufficient (no general JSON parser).
std::optional<std::string> JsonStringField(const std::string &body,
                                           const std::string &name) {
  const std::string needle = "\"" + name + "\"";
  const auto at = body.find(needle);
  if (at == std::string::npos) {
    return std::nullopt;
  }
  const auto colon = body.find(':', at + needle.size());
  if (colon == std::string::npos) {
    return std::nullopt;
  }
  const auto quote = body.find('"', colon + 1);
  if (quote == std::string::npos) {
    return std::nullopt;
  }
  const auto end = body.find('"', quote + 1);
  if (end == std::string::npos) {
    return std::nullopt;
  }
  return body.substr(quote + 1, end - quote - 1);
}

std::optional<int> JsonIntField(const std::string &body,
                                const std::string &name) {
  const std::string needle = "\"" + name + "\"";
  const auto at = body.find(needle);
  if (at == std::string::npos) {
    return std::nullopt;
  }
  const auto colon = body.find(':', at + needle.size());
  if (colon == std::string::npos) {
    return std::nullopt;
  }
  return std::atoi(body.c_str() + colon + 1);
}

// Booleans must be parsed as tokens: atoi("true") == 0, so routing
// "down":true through JsonIntField silently reads it as false/0 — which made
// /probe/hold erase the very key it was asked to hold (and still reply ok).
std::optional<bool> JsonBoolField(const std::string &body,
                                  const std::string &name) {
  const std::string needle = "\"" + name + "\"";
  const auto at = body.find(needle);
  if (at == std::string::npos) {
    return std::nullopt;
  }
  const auto colon = body.find(':', at + needle.size());
  if (colon == std::string::npos) {
    return std::nullopt;
  }
  const char *value = body.c_str() + colon + 1;
  while (*value == ' ') {
    ++value;
  }
  if (std::strncmp(value, "true", 4) == 0) {
    return true;
  }
  if (std::strncmp(value, "false", 5) == 0) {
    return false;
  }
  if (*value == '0' || *value == '1') {
    return *value == '1';
  }
  return std::nullopt;
}

// "W", "SPACE", "RETURN"... -> scancode, via SDL's own name table.
std::optional<SDL_Scancode> ScancodeFromName(const std::string &name) {
  const SDL_Scancode scancode = SDL_GetScancodeFromName(name.c_str());
  if (scancode == SDL_SCANCODE_UNKNOWN) {
    return std::nullopt;
  }
  return scancode;
}

[[nodiscard]] std::string JsonEscape(std::string_view text) {
  // JSON must be valid UTF-8; game resource strings (UI labels, log lines) are
  // raw MacRoman while probe-authored strings are already UTF-8.
  const std::string encoded = game::NovaText_EncodeUtf8(text);
  std::string out;
  out.reserve(encoded.size());
  for (const char c : encoded) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        out += ' ';
      } else {
        out += c;
      }
    }
  }
  return out;
}

[[nodiscard]] std::string_view LevelName(NovaLog::Level level) {
  switch (level) {
  case NovaLog::Level::debug:
    return "DEBUG";
  case NovaLog::Level::info:
    return "INFO";
  case NovaLog::Level::warn:
    return "WARN";
  case NovaLog::Level::error:
    return "ERROR";
  case NovaLog::Level::todo:
    return "TODO(decomp)";
  }
  return "UNKNOWN";
}

} // namespace

ProbeServer::~ProbeServer() { Stop(); }

bool ProbeServer::Start(int port) {
  if (running_.load()) {
    return true;
  }
  port_.store(port);
  running_.store(true);
  thread_ = std::thread([this, port] { AcceptLoop(port); });
  NovaLog::Info("probe: HTTP harness listening on 127.0.0.1:{} "
                "(docs/probe_harness.md)",
                port);
  return true;
}

void ProbeServer::Stop() {
  if (!running_.exchange(false)) {
    return;
  }
  // Wake the accept loop with a dummy connection, then join. If the listener
  // never bound (startup failure) there is nothing to wake.
  if (port_.load() != 0) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      address.sin_port = htons(static_cast<std::uint16_t>(port_.load()));
      (void)::connect(
          fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
      ::close(fd);
    }
  }
  sync_cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void ProbeServer::AcceptLoop(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    NovaLog::Error("probe: socket() failed");
    running_.store(false);
    return;
  }
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
      ::listen(fd, 4) < 0) {
    NovaLog::Error("probe: bind/listen on 127.0.0.1:{} failed", port);
    ::close(fd);
    running_.store(false);
    return;
  }

  while (running_.load()) {
    const int conn = ::accept(fd, nullptr, nullptr);
    if (conn < 0) {
      continue;
    }
    if (!running_.load()) { // Stop()'s wake-up connection
      ::close(conn);
      break;
    }
    // Read until the end of the headers, then Content-Length body bytes.
    std::string raw;
    char buffer[4096];
    const auto header_end = [&raw] {
      return raw.find("\r\n\r\n") != std::string::npos;
    };
    while (!header_end() && raw.size() < 64 * 1024) {
      const ssize_t n = ::recv(conn, buffer, sizeof(buffer), 0);
      if (n <= 0) {
        break;
      }
      raw.append(buffer, static_cast<std::size_t>(n));
    }
    std::string status = "500 Internal Server Error";
    std::string content_type = "text/plain";
    std::string body_out = "probe: malformed request";
    const auto head_end = raw.find("\r\n\r\n");
    if (head_end != std::string::npos) {
      const std::string head = raw.substr(0, head_end);
      std::string rest = raw.substr(head_end + 4);
      // Request line: METHOD /path?query HTTP/1.1
      const auto sp1 = head.find(' ');
      const auto sp2 = head.find(' ', sp1 + 1);
      std::string method = sp1 == std::string::npos ? "" : head.substr(0, sp1);
      std::string target =
          (sp1 == std::string::npos || sp2 == std::string::npos)
              ? ""
              : head.substr(sp1 + 1, sp2 - sp1 - 1);
      // Body: Content-Length header.
      std::size_t content_length = 0;
      std::size_t pos = head.find("Content-Length:");
      if (pos == std::string::npos) {
        pos = head.find("content-length:");
      }
      if (pos != std::string::npos) {
        content_length =
            static_cast<std::size_t>(std::atoi(head.c_str() + pos + 15));
      }
      while (rest.size() < content_length) {
        const ssize_t n = ::recv(conn, buffer, sizeof(buffer), 0);
        if (n <= 0) {
          break;
        }
        rest.append(buffer, static_cast<std::size_t>(n));
      }
      rest.resize(std::min(rest.size(), content_length));

      std::string path = target;
      std::string query;
      const auto qmark = target.find('?');
      if (qmark != std::string::npos) {
        path = target.substr(0, qmark);
        query = target.substr(qmark + 1);
      }
      HandleRequest(method, path, query, rest, status, content_type, body_out);
    }
    std::string response = "HTTP/1.1 " + status + "\r\n";
    response += "Content-Type: " + content_type + "\r\n";
    response += "Content-Length: " + std::to_string(body_out.size()) + "\r\n";
    response += "Connection: close\r\n\r\n";
    response += body_out;
    WriteAll(conn, response);
    ::close(conn);
  }
  ::close(fd);
}

void ProbeServer::HandleRequest(const std::string &method,
                                const std::string &path,
                                const std::string &query,
                                const std::string &body,
                                std::string &status,
                                std::string &content_type,
                                std::string &body_out) {
  content_type = "text/plain";
  const auto query_value =
      [&](const std::string &name) -> std::optional<std::string> {
    std::size_t pos = 0;
    while (pos <= query.size()) {
      const auto amp = query.find('&', pos);
      const std::string pair = query.substr(
          pos, amp == std::string::npos ? std::string::npos : amp - pos);
      const auto eq = pair.find('=');
      if (eq != std::string::npos && pair.substr(0, eq) == name) {
        return UrlDecode(pair.substr(eq + 1));
      }
      if (amp == std::string::npos) {
        break;
      }
      pos = amp + 1;
    }
    return std::nullopt;
  };

  if (path == "/probe/ping" && method == "GET") {
    status = "200 OK";
    body_out = "ok";
    return;
  }

  if (path == "/probe/state" && method == "GET") {
    const std::string q = query_value("query").value_or("summary");
    std::function<std::string(const std::string &)> provider;
    {
      const std::lock_guard lock(provider_mutex_);
      provider = state_provider_;
    }
    if (!provider) {
      status = "503 Service Unavailable";
      body_out = "probe: no state provider registered";
      return;
    }
    bool timed_out = false;
    body_out = SubmitJob([&provider, q] { return provider(q); }, timed_out);
    if (timed_out) {
      status = "504 Gateway Timeout";
      body_out = "probe: main thread did not service the request in time";
      return;
    }
    status = "200 OK";
    content_type = "application/json";
    return;
  }

  if (path == "/probe/automation" && method == "GET") {
    const std::lock_guard lock(automation_mutex_);
    status = "200 OK";
    content_type = "application/json";
    body_out = automation_status_;
    return;
  }

  if (path == "/probe/screenshot" && method == "GET") {
    std::uint64_t known_seq;
    {
      const std::lock_guard lock(frame_mutex_);
      known_seq = frame_seq_;
    }
    want_capture_.store(true);
    std::unique_lock lock(frame_mutex_);
    // While paused no new Present happens; fall back to the last frame.
    frame_cv_.wait_for(
        lock, kFrameWait, [&] { return frame_seq_ > known_seq; });
    if (frame_bytes_.empty()) {
      status = "503 Service Unavailable";
      body_out = "probe: no frame captured yet";
      return;
    }
    status = "200 OK";
    content_type = "image/bmp";
    body_out.assign(frame_bytes_.begin(), frame_bytes_.end());
    return;
  }

  if (path == "/probe/logs" && method == "GET") {
    // ?since=SEQ tails the in-memory log ring (log.hpp double-write);
    // without since, the whole retained buffer comes back.
    std::uint64_t since = 0;
    if (const auto value = query_value("since")) {
      since = std::strtoull(value->c_str(), nullptr, 10);
    }
    const auto lines = NovaLog::ReadLogSince(since);
    body_out =
        "{\"tail\":" + std::to_string(NovaLog::LogTailSeq()) + ",\"lines\":[";
    bool first = true;
    for (const auto &line : lines) {
      if (!first) {
        body_out += ",";
      }
      first = false;
      body_out += "{\"seq\":" + std::to_string(line.seq) + ",\"level\":\"" +
                  std::string{LevelName(line.level)} + "\",\"message\":\"" +
                  JsonEscape(line.text) + "\"}";
    }
    body_out += "]}";
    status = "200 OK";
    content_type = "application/json";
    return;
  }

  if (path == "/probe/ui" && method == "GET") {
    // Without ?at=x,y: the published layout of the active modal. With it: the
    // name of the published element under that window point (hit-test oracle).
    if (const auto at = query_value("at")) {
      float x = 0.0F;
      float y = 0.0F;
      if (std::sscanf(at->c_str(), "%f,%f", &x, &y) != 2) {
        status = "400 Bad Request";
        body_out = "probe: at must be x,y window points";
        return;
      }
      const auto element = UiElementAt({x, y});
      status = "200 OK";
      content_type = "application/json";
      body_out = "{\"element\":";
      body_out += element ? "\"" + JsonEscape(*element) + "\"" : "null";
      body_out += "}";
      return;
    }
    status = "200 OK";
    content_type = "application/json";
    body_out = UiJson();
    return;
  }

  if (path == "/probe/click" && method == "POST") {
    // {"element":"accept"} clicks the named rect published by the active
    // modal (see /probe/ui); {"x":553,"y":508} clicks raw window points.
    // Either way a motion event (updates the platform's tracked cursor)
    // followed by a left button-down is enqueued, so modal loops see the same
    // primary/mouse_position() pair a real click produces.
    float x = 0.0F;
    float y = 0.0F;
    if (const auto element = JsonStringField(body, "element")) {
      const auto rect = UiElementRect(*element);
      if (!rect) {
        status = "409 Conflict";
        body_out = "probe: element '" + *element +
                   "' not published (GET /probe/ui for the active layout)";
        return;
      }
      x = rect->x + rect->w / 2.0F;
      y = rect->y + rect->h / 2.0F;
    } else if (const auto px = JsonIntField(body, "x")) {
      const auto py = JsonIntField(body, "y");
      if (!py) {
        status = "400 Bad Request";
        body_out = "probe: missing y";
        return;
      }
      x = static_cast<float>(*px);
      y = static_cast<float>(*py);
    } else {
      status = "400 Bad Request";
      body_out = "probe: missing element or x/y";
      return;
    }
    InjectClick(x, y);
    status = "200 OK";
    content_type = "application/json";
    body_out = "{\"ok\":true}";
    return;
  }

  if (path == "/probe/command" && method == "POST") {
    const auto cmd = JsonStringField(body, "cmd");
    if (!cmd) {
      status = "400 Bad Request";
      body_out = "probe: missing cmd";
      return;
    }
    if (*cmd == "accelerate") {
      const bool enabled = JsonBoolField(body, "enabled").value_or(true);
      const int multiplier = JsonIntField(body, "speed_multiplier").value_or(1);
      const bool suppress_audio =
          JsonBoolField(body, "suppress_audio").value_or(false);
      if (multiplier < 1 || multiplier > 1000) {
        status = "400 Bad Request";
        body_out = "probe: speed_multiplier must be between 1 and 1000";
        return;
      }
      acceleration_requested_.store(enabled);
      speed_multiplier_requested_.store(static_cast<std::uint32_t>(multiplier));
      audio_suppression_requested_.store(enabled && suppress_audio);
      acceleration_request_pending_.store(true);
      sync_cv_.notify_all();
      status = "200 OK";
      content_type = "application/json";
      body_out = std::string{"{\"ok\":true,\"accelerated\":"} +
                 (enabled ? "true" : "false") + ",\"speed_multiplier\":" +
                 std::to_string(enabled ? multiplier : 1) +
                 ",\"audio_suppressed\":" +
                 (enabled && suppress_audio ? "true" : "false") + "}";
      return;
    }
    if (*cmd == "mission_trace") {
      const bool enabled = JsonBoolField(body, "enabled").value_or(true);
      auto mode = game::MissionTrace::Mode::off;
      if (enabled) {
        const auto requested = JsonStringField(body, "mode");
        mode = (requested && *requested == "full")
                   ? game::MissionTrace::Mode::full
                   : game::MissionTrace::Mode::commands;
      }
      game::MissionTrace::SetMode(mode);
      status = "200 OK";
      content_type = "application/json";
      body_out = std::string{"{\"ok\":true,\"mode\":\""} +
                 std::string{game::MissionTrace::ModeName(
                     game::MissionTrace::GetMode())} +
                 "\"}";
      return;
    }
    if (*cmd == "land_at" || *cmd == "jump_to" || *cmd == "destroy_ship") {
      const auto target = JsonStringField(body, "target");
      const int timeout_ms = JsonIntField(body, "timeout_ms").value_or(180000);
      const int ship_id = JsonIntField(body, "ship_id").value_or(-1);
      const bool allow_missing =
          JsonBoolField(body, "allow_missing").value_or(false);
      if (!target || target->empty() || timeout_ms < 1) {
        status = "400 Bad Request";
        body_out = "probe: automation requires target and positive timeout_ms";
        return;
      }
      const ProbeAutomationRequest::Kind kind =
          *cmd == "land_at"   ? ProbeAutomationRequest::Kind::kLandAt
          : *cmd == "jump_to" ? ProbeAutomationRequest::Kind::kJumpTo
                              : ProbeAutomationRequest::Kind::kDestroyShip;
      {
        const std::lock_guard lock(automation_mutex_);
        automation_request_ =
            ProbeAutomationRequest{kind,
                                   *target,
                                   static_cast<std::uint64_t>(timeout_ms),
                                   ship_id,
                                   allow_missing};
        // Immediately move the published status off any previous "complete".
        // The flight loop only switches the controller when it consumes this
        // request, so leaving the old phase visible lets a harness wait on
        // "complete" match the goal that just finished and race ahead of the
        // new one (observed as a click landing in the wrong window).
        const char *goal_name =
            kind == ProbeAutomationRequest::Kind::kLandAt   ? "land_at"
            : kind == ProbeAutomationRequest::Kind::kJumpTo ? "jump_to"
                                                            : "destroy";
        automation_status_ = std::string{"{\"goal\":\""} + goal_name +
                             "\",\"phase\":\"select\",\"target\":\"" +
                             JsonEscape(*target) +
                             "\",\"detail\":\"requested\"}";
      }
      sync_cv_.notify_all();
      status = "202 Accepted";
      content_type = "application/json";
      body_out = "{\"ok\":true}";
      return;
    }
    if (*cmd == "trade") {
      // Clean input-only trade: select a published commodity row and press the
      // Buy/Sell button with the same synthetic clicks a real user makes. The
      // modal's normal handler runs the transaction, so no game state is
      // touched here. Only valid while the trade center has published its
      // rows (`trade.row.<n>`) and action buttons.
      const auto commodity = JsonIntField(body, "commodity");
      const auto side = JsonStringField(body, "side");
      if (!commodity || !side || (*side != "buy" && *side != "sell")) {
        status = "400 Bad Request";
        body_out = "probe: trade requires commodity and side (buy|sell)";
        return;
      }
      // Optional quantity: the trade modal consumes it inside its own handler
      // (the synthesized clicks below stay the transaction trigger). `tons`
      // requests an exact amount; `max` requests the whole affordable/held
      // amount, matching the alt quantity prompt. Neither leaves 0, meaning
      // the original hardcoded click quantity (up to 10 tons).
      const auto tons_field = JsonIntField(body, "tons");
      const bool want_max = JsonBoolField(body, "max").value_or(false);
      if (tons_field && want_max) {
        status = "400 Bad Request";
        body_out = "probe: trade accepts either tons or max, not both";
        return;
      }
      std::int16_t tons = 0;
      if (tons_field) {
        if (*tons_field <= 0 || *tons_field > 32000) {
          status = "400 Bad Request";
          body_out = "probe: trade tons must be between 1 and 32000";
          return;
        }
        tons = static_cast<std::int16_t>(*tons_field);
      } else if (want_max) {
        // Negative sentinel: the modal maps it to its live BuyMax/SellMax.
        tons = -1;
      }
      const std::string row_name = "trade.row." + std::to_string(*commodity);
      const std::string button_name = *side == "buy" ? "buy" : "sell";
      const auto row_rect = UiElementRect(row_name);
      const auto button_rect = UiElementRect(button_name);
      if (!row_rect) {
        status = "409 Conflict";
        body_out =
            "probe: trade row '" + row_name + "' not published (GET /probe/ui)";
        return;
      }
      if (!button_rect) {
        status = "409 Conflict";
        body_out = "probe: trade center has no published '" + button_name +
                   "' button (GET /probe/ui)";
        return;
      }
      // Publish the tonnage only once the target controls exist, so a failed
      // command cannot leave a value for a later real click to consume. The
      // modal clears it on consumption.
      trade_quantity_consumed_.store(false);
      pending_trade_tons_.store(tons);
      InjectClick(row_rect->x + row_rect->w / 2.0F,
                  row_rect->y + row_rect->h / 2.0F);
      InjectClick(button_rect->x + button_rect->w / 2.0F,
                  button_rect->y + button_rect->h / 2.0F);
      // The main thread applies the transaction when the modal handles the
      // injected click. Wait for that before returning, so a follow-up trade
      // cannot overwrite the queued quantity before this one lands. This keeps
      // a `tons`/`max` trade ordered without the scenario polling an exact
      // post-trade cargo count (which credit-limited `max` makes unknowable).
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (!trade_quantity_consumed_.load() &&
             std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (!trade_quantity_consumed_.load()) {
        pending_trade_tons_.store(0);
        status = "504 Gateway Timeout";
        body_out =
            "probe: trade was not applied (trade center did not consume it)";
        return;
      }
      status = "200 OK";
      content_type = "application/json";
      body_out = "{\"ok\":true}";
      return;
    }
    if (*cmd == "cancel_automation") {
      const std::lock_guard lock(automation_mutex_);
      automation_request_ = ProbeAutomationRequest{};
      automation_status_ = "{\"goal\":\"none\",\"phase\":\"cancelled\"}";
      sync_cv_.notify_all();
      status = "200 OK";
      content_type = "application/json";
      body_out = "{\"ok\":true}";
      return;
    }
    if (*cmd == "pause") {
      paused_.store(true);
    } else if (*cmd == "resume") {
      step_remaining_.store(0);
      paused_.store(false);
    } else if (*cmd == "step") {
      const int frames = JsonIntField(body, "frames").value_or(1);
      step_remaining_.store(std::max(1, frames));
      paused_.store(true);
    } else if (*cmd == "quit") {
      quit_requested_.store(true);
    } else {
      status = "400 Bad Request";
      body_out = "probe: unknown cmd "
                 "(pause|resume|step|accelerate|mission_trace|land_at|jump_to|"
                 "destroy_ship|trade|cancel_automation|quit)";
      return;
    }
    sync_cv_.notify_all();
    status = "200 OK";
    content_type = "application/json";
    body_out = "{\"ok\":true}";
    return;
  }

  if (path == "/probe/key" && method == "POST") {
    const auto key_name = JsonStringField(body, "key");
    if (!key_name) {
      status = "400 Bad Request";
      body_out = "probe: missing key";
      return;
    }
    const auto down_field = JsonBoolField(body, "down");
    const SDL_Keycode keycode = SDL_GetKeyFromName(key_name->c_str());
    if (keycode == SDLK_UNKNOWN) {
      status = "400 Bad Request";
      body_out = "probe: unknown key name";
      return;
    }
    const bool down_specified = down_field.has_value();
    const bool down = down_field.value_or(false);
    const auto make_event = [](bool is_down, SDL_Keycode code) {
      SDL_Event event{};
      event.type = is_down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
      event.key.key = code;
      event.key.down = is_down;
      return event;
    };
    const std::lock_guard lock(sync_);
    if (down_specified) {
      key_events_.push_back(make_event(down, keycode));
    } else {
      // Default: tap (down + up).
      key_events_.push_back(make_event(true, keycode));
      key_events_.push_back(make_event(false, keycode));
    }
    sync_cv_.notify_all();
    status = "200 OK";
    content_type = "application/json";
    body_out = "{\"ok\":true}";
    return;
  }

  if (path == "/probe/hold" && method == "POST") {
    // {"keys": ["W","SPACE"], "down": true} — virtual held keys merged into
    // PollFlightInput (SDL_GetKeyboardState cannot see injected events).
    const auto down_field = JsonBoolField(body, "down");
    if (!down_field) {
      status = "400 Bad Request";
      body_out = "probe: missing down flag";
      return;
    }
    const int down = *down_field ? 1 : 0;
    const auto array_begin = body.find('[');
    const auto array_end = body.find(']', array_begin);
    if (array_begin == std::string::npos || array_end == std::string::npos) {
      status = "400 Bad Request";
      body_out = "probe: missing keys array";
      return;
    }
    std::size_t cursor = array_begin + 1;
    const std::lock_guard lock(keys_mutex_);
    while (cursor < array_end) {
      const auto quote = body.find('"', cursor);
      if (quote == std::string::npos || quote > array_end) {
        break;
      }
      const auto end = body.find('"', quote + 1);
      if (end == std::string::npos || end > array_end) {
        break;
      }
      if (const auto scancode =
              ScancodeFromName(body.substr(quote + 1, end - quote - 1))) {
        if (down != 0) {
          virtual_keys_.insert(*scancode);
        } else {
          virtual_keys_.erase(*scancode);
        }
      }
      cursor = end + 1;
    }
    status = "200 OK";
    content_type = "application/json";
    body_out = "{\"ok\":true}";
    return;
  }

  status = "404 Not Found";
  body_out = "probe: unknown endpoint (see docs/probe_harness.md)";
}

std::optional<ProbeAutomationRequest> ProbeServer::ConsumeAutomationRequest() {
  const std::lock_guard lock(automation_mutex_);
  auto request = std::move(automation_request_);
  automation_request_.reset();
  return request;
}

bool ProbeServer::HasPendingAutomationRequest() {
  const std::lock_guard lock(automation_mutex_);
  return automation_request_.has_value();
}

void ProbeServer::PublishAutomationStatus(std::string json) {
  const std::lock_guard lock(automation_mutex_);
  automation_status_ = std::move(json);
}

void ProbeServer::AutomationObservedDocked() {
  const std::lock_guard lock(automation_mutex_);
  automation_docked_ = true;
  if (automation_status_.find("\"goal\":\"land_at\"") != std::string::npos) {
    automation_status_ = "{\"goal\":\"land_at\",\"phase\":\"complete\","
                         "\"detail\":\"Spaceport observed\"}";
  }
}

bool ProbeServer::ConsumeAutomationDocked() {
  const std::lock_guard lock(automation_mutex_);
  return std::exchange(automation_docked_, false);
}

std::int16_t ProbeServer::ConsumePendingTradeQuantity() {
  const std::int16_t quantity = pending_trade_tons_.exchange(0);
  trade_quantity_consumed_.store(true);
  return quantity;
}

std::string ProbeServer::SubmitJob(const std::function<std::string()> &work,
                                   bool &timed_out) {
  std::packaged_task<std::string()> task(work);
  std::future<std::string> future = task.get_future();
  {
    const std::lock_guard lock(sync_);
    jobs_.push_back(std::move(task));
  }
  sync_cv_.notify_all();
  timed_out = future.wait_for(kJobWait) != std::future_status::ready;
  if (timed_out) {
    return {};
  }
  return future.get();
}

void ProbeServer::RunJobsLocked() {
  while (!jobs_.empty()) {
    std::packaged_task<std::string()> task = std::move(jobs_.front());
    jobs_.pop_front();
    task();
  }
}

bool ProbeServer::Pump() {
  if (!running_.load()) {
    return false;
  }
  bool waited_while_paused = false;
  {
    std::unique_lock lock(sync_);
    // Inject queued synthetic key events (the modal-loop channel).
    while (!key_events_.empty()) {
      SDL_PushEvent(&key_events_.front());
      key_events_.pop_front();
    }
    // Pause latch: sleep here (still servicing requests) until resumed or a
    // step budget is granted. Stepping counts frames in OnPresent.
    while (paused_.load() && step_remaining_.load() == 0 && running_.load()) {
      waited_while_paused = true;
      RunJobsLocked();
      sync_cv_.wait_for(lock, std::chrono::milliseconds(50));
    }
    RunJobsLocked();
  }
  if (quit_requested_.exchange(false)) {
    std::function<void()> latch;
    {
      const std::lock_guard lock(provider_mutex_);
      latch = quit_latch_;
    }
    if (latch) {
      latch();
    }
  }
  return waited_while_paused;
}

void ProbeServer::OnPresent(SDL_Renderer *renderer) {
  if (!running_.load()) {
    return;
  }
  // Step budget: after the counted frames, latch the pause again.
  int remaining = step_remaining_.load();
  if (remaining > 0 &&
      step_remaining_.compare_exchange_strong(remaining, remaining - 1) &&
      remaining == 1) {
    paused_.store(true);
    sync_cv_.notify_all();
  }
  if (!want_capture_.exchange(false)) {
    return;
  }
  // SDL3 reads the current render target into a fresh surface; called here,
  // before SDL_RenderPresent, that is exactly the frame being swapped in.
  SDL_Surface *surface = SDL_RenderReadPixels(renderer, nullptr);
  if (surface == nullptr) {
    NovaLog::Error("probe: SDL_RenderReadPixels failed: {}", SDL_GetError());
    return;
  }
  SDL_IOStream *io = SDL_IOFromDynamicMem();
  if (io == nullptr) {
    SDL_DestroySurface(surface);
    NovaLog::Error("probe: dynamic memory IO failed: {}", SDL_GetError());
    return;
  }
  const bool saved = SDL_SaveBMP_IO(surface, io, false);
  const std::int64_t size = saved ? SDL_GetIOSize(io) : 0;
  SDL_DestroySurface(surface);
  if (!saved || size <= 0) {
    NovaLog::Error("probe: BMP encode failed: {}", SDL_GetError());
    SDL_CloseIO(io);
    return;
  }
  std::vector<std::uint8_t> bmp(static_cast<std::size_t>(size));
  SDL_SeekIO(io, 0, SDL_IO_SEEK_SET);
  (void)SDL_ReadIO(io, bmp.data(), bmp.size());
  SDL_CloseIO(io);
  {
    const std::lock_guard lock(frame_mutex_);
    frame_bytes_ = std::move(bmp);
    ++frame_seq_;
    frame_cv_.notify_all();
  }
}

bool ProbeServer::VirtualKey(SDL_Scancode scancode) const {
  const std::lock_guard lock(keys_mutex_);
  return virtual_keys_.count(scancode) != 0;
}

bool ProbeServer::ConsumeAccelerationRequest(bool &enabled,
                                             std::uint32_t &speed_multiplier,
                                             bool &suppress_audio) {
  if (!acceleration_request_pending_.exchange(false)) {
    return false;
  }
  enabled = acceleration_requested_.load();
  speed_multiplier = speed_multiplier_requested_.load();
  suppress_audio = audio_suppression_requested_.load();
  return true;
}

void ProbeServer::SetStateProvider(
    std::function<std::string(const std::string &query)> provider) {
  const std::lock_guard lock(provider_mutex_);
  state_provider_ = std::move(provider);
}

void ProbeServer::SetQuitLatch(std::function<void()> latch) {
  const std::lock_guard lock(provider_mutex_);
  quit_latch_ = std::move(latch);
}

void ProbeServer::InjectClick(float x, float y) {
  SDL_Event motion{};
  motion.type = SDL_EVENT_MOUSE_MOTION;
  motion.motion.x = x;
  motion.motion.y = y;
  SDL_Event down{};
  down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  down.button.button = SDL_BUTTON_LEFT;
  down.button.down = true;
  down.button.clicks = 1;
  down.button.x = x;
  down.button.y = y;
  const std::lock_guard lock(sync_);
  key_events_.push_back(motion);
  key_events_.push_back(down);
  sync_cv_.notify_all();
}

void ProbeServer::PublishUi(std::string window_name,
                            std::vector<ProbeNamedRect> rects) {
  if (!running_.load()) {
    return;
  }
  const std::lock_guard lock(ui_mutex_);
  ui_window_ = std::move(window_name);
  ui_rects_ = std::move(rects);
}

void ProbeServer::ClearUi() {
  const std::lock_guard lock(ui_mutex_);
  ui_window_.clear();
  ui_rects_.clear();
}

void ProbeServer::SetGeometry(SDL_FPoint window_points, SDL_FRect playfield) {
  const std::lock_guard lock(ui_mutex_);
  geom_window_points_ = window_points;
  geom_playfield_ = playfield;
}

bool ProbeServer::UiActive() const {
  const std::lock_guard lock(ui_mutex_);
  return !ui_rects_.empty();
}

std::optional<SDL_FRect>
ProbeServer::UiElementRect(const std::string &element) const {
  const std::lock_guard lock(ui_mutex_);
  for (const auto &named : ui_rects_) {
    if (named.name == element) {
      return named.rect;
    }
  }
  return std::nullopt;
}

std::optional<std::string> ProbeServer::UiElementAt(SDL_FPoint point) const {
  const std::lock_guard lock(ui_mutex_);
  // Reverse order (hit-test oracle for /probe/ui?at=x,y): modals publish
  // their container rect ("window") first and their controls after, so the
  // innermost (last-published) rect must win over the container.
  for (auto it = ui_rects_.rbegin(); it != ui_rects_.rend(); ++it) {
    const auto &r = it->rect;
    if (point.x >= r.x && point.x < r.x + r.w && point.y >= r.y &&
        point.y < r.y + r.h) {
      return it->name;
    }
  }
  return std::nullopt;
}

std::string ProbeServer::UiJson() const {
  const std::lock_guard lock(ui_mutex_);
  char geom[160];
  std::snprintf(
      geom,
      sizeof(geom),
      "\"window_size\":[%.1f,%.1f],\"playfield\":[%.1f,%.1f,%.1f,%.1f],",
      geom_window_points_.x,
      geom_window_points_.y,
      geom_playfield_.x,
      geom_playfield_.y,
      geom_playfield_.w,
      geom_playfield_.h);
  std::string out = "{" + std::string(geom);
  out += "\"window\":\"" + JsonEscape(ui_window_) + "\",";
  out += "\"rects\":{";
  bool first = true;
  for (const auto &named : ui_rects_) {
    if (!first) {
      out += ",";
    }
    first = false;
    char rect[96];
    std::snprintf(rect,
                  sizeof(rect),
                  "[%.1f,%.1f,%.1f,%.1f]",
                  named.rect.x,
                  named.rect.y,
                  named.rect.w,
                  named.rect.h);
    out += "\"" + named.name + "\":" + rect;
  }
  out += "},\"items\":[";
  bool first_item = true;
  for (const auto &named : ui_rects_) {
    if (!named.has_value) {
      continue;
    }
    if (!first_item) {
      out += ",";
    }
    first_item = false;
    char rect[96];
    std::snprintf(rect,
                  sizeof(rect),
                  "[%.1f,%.1f,%.1f,%.1f]",
                  named.rect.x,
                  named.rect.y,
                  named.rect.w,
                  named.rect.h);
    out += "{\"name\":\"" + JsonEscape(named.name) + "\",\"rect\":" + rect +
           ",\"label\":\"" + JsonEscape(named.label) +
           "\",\"price\":" + std::to_string(named.value) +
           ",\"selected\":" + (named.selected ? "true" : "false") + "}";
  }
  out += "]}";
  return out;
}
