// No-op ProbeServer transport used when EVNOVA_ENABLE_PROBE=OFF.
//
// The real implementation (probe_server.cpp) is a POSIX socket server; this
// translation unit keeps the same class surface so every game-side call site
// (PublishProbeUi, ConsumeAutomationRequest, ...) still compiles but does
// nothing. EVN_PROBE=1 is ignored in this build. See docs/probe_harness.md.

#include "probe_server.hpp"

ProbeServer::~ProbeServer() = default;

bool ProbeServer::Start(int) { return false; }

void ProbeServer::Stop() {}

void ProbeServer::OnPresent(SDL_Renderer *) {}

bool ProbeServer::Pump() { return false; }

bool ProbeServer::VirtualKey(SDL_Scancode) const { return false; }

bool ProbeServer::ConsumeAccelerationRequest(bool &, std::uint32_t &, bool &) {
  return false;
}

std::optional<ProbeAutomationRequest> ProbeServer::ConsumeAutomationRequest() {
  return std::nullopt;
}

bool ProbeServer::HasPendingAutomationRequest() { return false; }

void ProbeServer::PublishAutomationStatus(std::string) {}

void ProbeServer::AutomationObservedDocked() {}

bool ProbeServer::ConsumeAutomationDocked() { return false; }

void ProbeServer::SetStateProvider(
    std::function<std::string(const std::string &)>) {}

void ProbeServer::SetQuitLatch(std::function<void()>) {}

std::int16_t ProbeServer::ConsumePendingTradeQuantity() { return 0; }

void ProbeServer::PublishUi(std::string, std::vector<ProbeNamedRect>) {}

void ProbeServer::ClearUi() {}

void ProbeServer::SetGeometry(SDL_FPoint, SDL_FRect) {}

bool ProbeServer::UiActive() const { return false; }

std::optional<SDL_FRect> ProbeServer::UiElementRect(const std::string &) const {
  return std::nullopt;
}

std::optional<std::string> ProbeServer::UiElementAt(SDL_FPoint) const {
  return std::nullopt;
}

std::string ProbeServer::UiJson() const { return "{}"; }
