#pragma once

#include "settings.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <span>

// Wi-Fi station + GATT-over-TCP transport (same protocol as the USB CDC GATT interface),
// and a configuration hotspot w/ a captive web portal.
#ifndef NEVERMORE_WIFI
#define NEVERMORE_WIFI 0
#endif

namespace nevermore::wifi {

using namespace std::literals::chrono_literals;

constexpr uint16_t GATT_TCP_PORT = 7700;
// Advertised via mDNS as `<MDNS_HOSTNAME>.local`, service `_nevermore._tcp`.
constexpr char const* MDNS_HOSTNAME = "nevermore";

// Configuration hotspot: `<AP_SSID_PREFIX><last 2 MAC octets>`, portal at http://192.168.4.1/
// Opened when no network is saved, or when none could be joined for `AP_AFTER_FAILING_FOR`.
// Closed once a network is joined: `AP_CLOSE_AFTER_RESULT_SHOWN` after the portal showed the result,
// at most `AP_CLOSE_AFTER_CONNECT` after joining (the phone may not come back after the channel switch).
constexpr char const* AP_SSID_PREFIX = "Nevermore-";
constexpr char const* AP_PASSWORD_DEFAULT = "nevermore";
constexpr auto AP_AFTER_FAILING_FOR = 2min;
constexpr auto AP_CLOSE_AFTER_CONNECT = 1min;
constexpr auto AP_CLOSE_AFTER_RESULT_SHOWN = 5s;

// NB: Values are part of the GATT protocol. Append only.
enum class State : uint8_t {
    unsupported = 0,  // firmware built w/o Wi-Fi support
    disabled = 1,     // no network saved
    connecting = 2,
    connected = 3,
    failed_auth = 4,        // bad password
    failed_no_network = 5,  // SSID not found
    failed = 6,             // other failure
};

// NB: Layout is part of the GATT protocol. Append only.
struct [[gnu::packed]] Status {
    State state = State::unsupported;
    std::array<uint8_t, 4> ipv4{};  // 0.0.0.0 if not connected
    uint16_t port = 0;              // GATT TCP port; 0 if unsupported
    uint8_t ap_active = 0;          // configuration hotspot is up
};

// security reported by a scan
enum class Auth : uint8_t { open, wep, wpa, wpa2, wpa_wpa2 };

struct ScanResult {
    std::array<char, settings::WifiNetwork::SSID_MAX + 1> ssid{};
    int16_t rssi = 0;  // dBm
    Auth auth = Auth::open;
};

// Everything the portal shows. IPv4 addresses in network order.
struct Details {
    State state = State::unsupported;
    std::array<char, settings::WifiNetwork::SSID_MAX + 1> ssid{};  // current/last network
    int32_t rssi = 0;
    uint32_t ip = 0, netmask = 0, gateway = 0, dns = 0;
    std::array<uint8_t, 6> mac{};
    bool ap_active = false;
    std::array<char, settings::WifiNetwork::SSID_MAX + 1> ap_ssid{};
    int ap_clients = 0;
};

// Launches the Wi-Fi, GATT TCP & portal tasks.
// PRECONDITION: `cyw43_arch_init`, `settings::init`, and `gatt_stream::init` are done.
bool init();

[[nodiscard]] Status status();
[[nodiscard]] Details details();
[[nodiscard]] settings::WifiSettings saved();

// Saves `network` as the first one to try (replacing a saved one w/ the same SSID;
// the last one is dropped if full). Doesn't switch networks by itself, see `connect`.
// PRECONDITION: `network.validate() && network.used()`
void network_add(settings::WifiNetwork const& network);
// Returns false if `ssid` isn't saved. Leaves the network if it's the current one.
bool network_remove(char const* ssid);
void networks_clear();
// Join the saved network `ssid` now. Returns false if it isn't saved.
bool connect(char const* ssid);
// PRECONDITION: empty or a valid WPA passphrase. Applies the next time the hotspot opens.
void ap_password_set(char const* password);
// The portal showed a hotspot client that we're connected: the hotspot can close.
void ap_result_shown();

// Blocks a few seconds. Returns # of distinct SSIDs (strongest first), or -1 if busy/failed.
int scan(std::span<ScanResult> results);

}  // namespace nevermore::wifi
