#pragma once

#include "settings.hpp"
#include <array>
#include <cstdint>

// Wi-Fi station + GATT-over-TCP transport (same protocol as the USB CDC GATT interface).
#ifndef NEVERMORE_WIFI
#define NEVERMORE_WIFI 0
#endif

namespace nevermore::wifi {

constexpr uint16_t GATT_TCP_PORT = 7700;
// Advertised via mDNS as `<MDNS_HOSTNAME>.local`, service `_nevermore._tcp`.
constexpr char const* MDNS_HOSTNAME = "nevermore";

// NB: Values are part of the GATT protocol. Append only.
enum class State : uint8_t {
    unsupported = 0,  // firmware built w/o Wi-Fi support
    disabled = 1,     // no credentials configured
    connecting = 2,
    connected = 3,
    failed_auth = 4,        // bad password
    failed_no_network = 5,  // SSID not found
    failed = 6,             // other failure
};

struct [[gnu::packed]] Status {
    State state = State::unsupported;
    std::array<uint8_t, 4> ipv4{};  // 0.0.0.0 if not connected
    uint16_t port = 0;              // GATT TCP port; 0 if unsupported
};

// Launches the Wi-Fi & TCP server tasks.
// PRECONDITION: `cyw43_arch_init`, `settings::init`, and `gatt_stream::init` are done.
bool init();

[[nodiscard]] Status status();

// Updates `settings::g_active.wifi` and (re)connects.
// PRECONDITION: `credentials.validate()`
void credentials_set(settings::WifiCredentials const& credentials);

}  // namespace nevermore::wifi
