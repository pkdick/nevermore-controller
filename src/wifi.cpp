#include "wifi.hpp"

#if !NEVERMORE_WIFI

namespace nevermore::wifi {

bool init() {
    return true;
}

Status status() {
    return {};
}

Details details() {
    return {};
}

settings::WifiSettings saved() {
    return {};
}

void network_add(settings::WifiNetwork const&) {}

bool network_remove(char const*) {
    return false;
}

void networks_clear() {}

bool connect(char const*) {
    return false;
}

void ap_password_set(char const*) {}

void ap_result_shown() {}

int scan(std::span<ScanResult>) {
    return -1;
}

}  // namespace nevermore::wifi

#else

#include "FreeRTOS.h"  // IWYU pragma: keep
#include "lwip/apps/mdns.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/sys.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "semphr.h"  // IWYU pragma: keep [doesn't notice `SemaphoreHandle_t`]
#include "task.h"    // IWYU pragma: keep
#include "utility/scope_guard.hpp"
#include "utility/task.hpp"
#include "wifi/dns_server.hpp"
#include "wifi/gatt_tcp.hpp"
#include "wifi/portal.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>

extern "C" {
#include "wifi/dhcpserver.h"  // C, from MicroPython
}

using namespace std;

// lwIP time source. See `LWIP_FREERTOS_SYS_NOW_FROM_FREERTOS` in `lwipopts.h`.
// Wraps at 2^32 ms, as lwIP expects.
extern "C" u32_t sys_now() {
    return to_ms_since_boot(get_absolute_time());
}

namespace nevermore::wifi {

namespace {

constexpr auto JOIN_TIMEOUT = 20s;
constexpr auto RETRY_DELAY = 10s;
// Joining makes the radio leave the hotspot's channel; don't keep kicking portal users off.
constexpr auto RETRY_DELAY_HOTSPOT = 60s;
constexpr auto LINK_POLL_PERIOD = 500ms;
constexpr auto SCAN_TIMEOUT = 10s;
constexpr size_t SCAN_RESULTS_MAX = 32;

constexpr uint32_t WIFI_STACK_SIZE = 1024;

// `wifi_task` notification bits
constexpr uint32_t NOTIFY_CONNECT = 1 << 0;   // `connect` requested a network
constexpr uint32_t NOTIFY_NETWORKS = 1 << 1;  // saved networks changed

template <typename A, typename P>
TickType_t ticks(chrono::duration<A, P> const& d) {
    return pdMS_TO_TICKS(chrono::duration_cast<chrono::milliseconds>(d).count());
}

using Ssid = array<char, settings::WifiNetwork::SSID_MAX + 1>;

template <size_t N>
void copy_str(array<char, N>& dst, char const* src) {
    dst = {};
    strncpy(dst.data(), src, N - 1);
}

// Guards `settings::g_active.wifi` and the state below.
SemaphoreHandle_t g_lock;
Status g_status{.state = State::disabled, .port = GATT_TCP_PORT};
Ssid g_current{};    // network being joined or joined
Ssid g_requested{};  // `connect` request, consumed by `wifi_task`
Ssid g_ap_ssid{};
optional<TickType_t> g_ap_result_shown;  // since joining the current network
TaskHandle_t g_wifi_task;

[[nodiscard]] auto lock_guard() {
    xSemaphoreTake(g_lock, portMAX_DELAY);
    return ScopeGuard{[&] { xSemaphoreGive(g_lock); }};
}

void notify(uint32_t bits) {
    xTaskNotify(g_wifi_task, bits, eSetBits);
}

// Returns the notification bits received before `timeout` (0 if none).
uint32_t wait_for_change(TickType_t timeout) {
    uint32_t bits = 0;
    xTaskNotifyWait(0, UINT32_MAX, &bits, timeout);
    return bits;
}

netif& netif_sta() {
    return cyw43_state.netif[CYW43_ITF_STA];
}

void status_set(State state, ip4_addr_t const* addr = nullptr) {
    auto _ = lock_guard();
    g_status.state = state;
    g_status.ipv4 = {};
    if (addr) memcpy(g_status.ipv4.data(), &addr->addr, sizeof(g_status.ipv4));  // already network order
}

void mdns_announce() {
    static bool s_started = false;

    cyw43_arch_lwip_begin();
    if (!s_started) {
        mdns_resp_init();
        if (mdns_resp_add_netif(&netif_sta(), MDNS_HOSTNAME) != ERR_OK) {
            printf("ERR - WiFi - mDNS failed to add netif\n");
        } else if (mdns_resp_add_service(&netif_sta(), MDNS_HOSTNAME, "_nevermore", DNSSD_PROTO_TCP,
                           GATT_TCP_PORT, nullptr, nullptr) < 0) {
            printf("ERR - WiFi - mDNS failed to add service\n");
        }
        s_started = true;
    } else {
        mdns_resp_restart(&netif_sta());
    }
    cyw43_arch_lwip_end();
}

////////////////////////////////////////////////////////////////////////////////
// Configuration hotspot

dhcp_server_t g_dhcp;

bool ap_active() {
    auto _ = lock_guard();
    return g_status.ap_active != 0;
}

void ap_start() {
    if (ap_active()) return;

    array<uint8_t, 6> mac{};
    cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac.data());
    Ssid ssid{};
    snprintf(ssid.data(), ssid.size(), "%s%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);

    array<char, settings::WifiNetwork::PASSWORD_MAX + 1> password{};
    {
        auto _ = lock_guard();
        password = settings::g_active.wifi.ap_password;
    }
    if (password[0] == '\0') copy_str(password, AP_PASSWORD_DEFAULT);

    cyw43_arch_enable_ap_mode(ssid.data(), password.data(), CYW43_AUTH_WPA2_AES_PSK);

    ip_addr_t ip, netmask;
    ip_addr_set_ip4_u32(&ip, PP_HTONL(CYW43_DEFAULT_IP_AP_ADDRESS));
    ip_addr_set_ip4_u32(&netmask, PP_HTONL(CYW43_DEFAULT_IP_MASK));
    cyw43_arch_lwip_begin();
    dhcp_server_init(&g_dhcp, &ip, &netmask);
    dns_server::start(*ip_2_ip4(&ip));
    cyw43_arch_lwip_end();

    {
        auto _ = lock_guard();
        g_status.ap_active = 1;
        g_ap_ssid = ssid;
    }
    printf("WiFi - hotspot `%s` up; portal at http://%s/\n", ssid.data(), ipaddr_ntoa(&ip));
}

void ap_stop() {
    if (!ap_active()) return;

    cyw43_arch_lwip_begin();
    dns_server::stop();
    dhcp_server_deinit(&g_dhcp);
    cyw43_arch_lwip_end();
    cyw43_arch_disable_ap_mode();

    {
        auto _ = lock_guard();
        g_status.ap_active = 0;
    }
    printf("WiFi - hotspot down\n");
}

////////////////////////////////////////////////////////////////////////////////
// Station

struct Saved {
    settings::WifiSettings settings{};
    size_t count = 0;
};

Saved saved_networks() {
    Saved x{.settings = saved()};
    for (auto const& network : x.settings.networks)
        x.count += network.used() ? 1 : 0;
    return x;
}

// Index of `ssid` in `saved`, or `saved.count` if missing.
size_t find_saved(Saved const& saved, char const* ssid) {
    for (size_t i = 0; i < saved.count; ++i)
        if (strcmp(saved.settings.networks[i].ssid.data(), ssid) == 0) return i;
    return saved.count;
}

enum class JoinResult { joined, failed, interrupted };

JoinResult join(settings::WifiNetwork const& network, bool wpa3, int& link) {
    {
        auto _ = lock_guard();
        g_current = network.ssid;
    }
    status_set(State::connecting);
    printf("WiFi - connecting to `%s`\n", network.ssid.data());

    auto const* password = network.password[0] != '\0' ? network.password.data() : nullptr;
    uint32_t const auth = !password ? CYW43_AUTH_OPEN
                          : wpa3    ? CYW43_AUTH_WPA3_WPA2_AES_PSK
                                    : CYW43_AUTH_WPA2_MIXED_PSK;

    link = CYW43_LINK_FAIL;
    if (auto err = cyw43_arch_wifi_connect_async(network.ssid.data(), password, auth)) {
        printf("ERR - WiFi - failed to start connecting, err=%d\n", err);
        return JoinResult::failed;
    }

    auto const start = xTaskGetTickCount();
    for (;;) {
        link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (link == CYW43_LINK_UP) return JoinResult::joined;
        if (link < 0) return JoinResult::failed;
        if (ticks(JOIN_TIMEOUT) <= xTaskGetTickCount() - start) return JoinResult::failed;
        // Only an explicit request interrupts a join; edits to the saved list can wait.
        // `g_requested` stays set, `wifi_task` picks it up.
        if (wait_for_change(ticks(LINK_POLL_PERIOD)) & NOTIFY_CONNECT) return JoinResult::interrupted;
    }
}

void failed_status(int link) {
    status_set(link == CYW43_LINK_BADAUTH  ? State::failed_auth
               : link == CYW43_LINK_NONET ? State::failed_no_network
                                          : State::failed);
}

// Stays here while connected. Returns when the link drops or another network is wanted.
void connected(settings::WifiNetwork const& network) {
    // Latency matters more than power here (we're USB powered).
    // Power-save adds up to ~200ms per request/response exchange.
    cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
    mdns_announce();

    ip4_addr_t addr = *netif_ip4_addr(&netif_sta());
    status_set(State::connected, &addr);
    printf("WiFi - connected to `%s`; IP %s, GATT TCP port %u, mDNS `%s.local`\n", network.ssid.data(),
            ip4addr_ntoa(&addr), unsigned(GATT_TCP_PORT), MDNS_HOSTNAME);

    // keep the hotspot a little while so the portal can show the result
    bool const close_ap = ap_active();
    auto const connected_at = xTaskGetTickCount();
    {
        auto _ = lock_guard();
        g_ap_result_shown.reset();
    }

    for (;;) {
        auto const bits = wait_for_change(ticks(LINK_POLL_PERIOD));
        if (bits) {
            Ssid requested{};
            {
                auto _ = lock_guard();
                requested = g_requested;
            }
            auto const saved = saved_networks();
            bool const switch_requested = (bits & NOTIFY_CONNECT) && requested != network.ssid;
            bool const removed = find_saved(saved, network.ssid.data()) == saved.count;
            if (switch_requested || removed) {
                // `g_requested` (if any) stays set, `wifi_task` picks it up
                printf("WiFi - leaving `%s`\n", network.ssid.data());
                return;
            }
            if (bits & NOTIFY_CONNECT) {
                auto _ = lock_guard();
                g_requested = {};  // already on it
            }
        }

        if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP) {
            printf("WARN - WiFi - link lost, reconnecting\n");
            return;
        }

        if (close_ap && ap_active()) {
            auto const now = xTaskGetTickCount();
            optional<TickType_t> shown;
            {
                auto _ = lock_guard();
                shown = g_ap_result_shown;
            }
            if (ticks(AP_CLOSE_AFTER_CONNECT) <= now - connected_at ||
                    (shown && ticks(AP_CLOSE_AFTER_RESULT_SHOWN) <= now - *shown))
                ap_stop();
        }

        // DHCP lease renewals may hand out a new address.
        if (auto curr = *netif_ip4_addr(&netif_sta()); !ip4_addr_eq(&curr, &addr)) {
            addr = curr;
            status_set(State::connected, &addr);
            printf("WiFi - IP changed to %s\n", ip4addr_ntoa(&addr));
        }
    }
}

void wifi_task() {
    cyw43_arch_enable_sta_mode();  // also needed for scanning

    auto failing_since = xTaskGetTickCount();
    size_t next = 0;
    // Alternate WPA2 & WPA3 transitional auth until one works.
    bool wpa3 = false;

    for (;;) {
        auto const saved = saved_networks();
        if (saved.count == 0) {
            status_set(State::disabled);
            printf("WiFi - no network saved\n");
            ap_start();
            wait_for_change(portMAX_DELAY);
            next = 0;
            failing_since = xTaskGetTickCount();
            continue;
        }

        // a requested network jumps the queue
        {
            auto _ = lock_guard();
            if (g_requested[0] != '\0') next = find_saved(saved, g_requested.data());
            g_requested = {};
        }
        if (saved.count <= next) next = 0;

        auto const& network = saved.settings.networks[next];
        int link = CYW43_LINK_FAIL;
        auto const result = join(network, wpa3 && network.password[0] != '\0', link);

        if (result == JoinResult::joined) {
            connected(network);
            failing_since = xTaskGetTickCount();
        } else if (result == JoinResult::failed) {
            failed_status(link);
            printf("ERR - WiFi - `%s` failed, link status=%d\n", network.ssid.data(), link);
        }

        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        if (result != JoinResult::failed) continue;  // `next` was requested or stays the same

        if (++next < saved.count) continue;  // try the next saved network right away

        // whole list failed
        next = 0;
        wpa3 = !wpa3;
        if (ticks(AP_AFTER_FAILING_FOR) <= xTaskGetTickCount() - failing_since) ap_start();
        wait_for_change(ticks(ap_active() ? RETRY_DELAY_HOTSPOT : RETRY_DELAY));
    }
}

////////////////////////////////////////////////////////////////////////////////
// Scanning

SemaphoreHandle_t g_scan_lock;  // one scan at a time
array<ScanResult, SCAN_RESULTS_MAX> g_scan_results;
size_t g_scan_count = 0;

Auth auth_from_scan(uint8_t mode) {
    // bit 0: WEP/privacy, bit 1: WPA IE, bit 2: RSN IE (WPA2/WPA3)
    if ((mode & 0b110) == 0b110) return Auth::wpa_wpa2;
    if (mode & 0b100) return Auth::wpa2;
    if (mode & 0b010) return Auth::wpa;
    if (mode & 0b001) return Auth::wep;
    return Auth::open;
}

// Runs in the CYW43 driver's context.
int on_scan_result(void*, cyw43_ev_scan_result_t const* r) {
    if (!r || r->ssid_len == 0 || settings::WifiNetwork::SSID_MAX < r->ssid_len) return 0;  // hidden

    Ssid ssid{};
    memcpy(ssid.data(), r->ssid, r->ssid_len);
    auto const end = g_scan_results.begin() + g_scan_count;
    auto it = find_if(g_scan_results.begin(), end, [&](auto const& x) { return x.ssid == ssid; });
    if (it == end) {
        if (g_scan_count == g_scan_results.size()) return 0;
        ++g_scan_count;
        *it = {.ssid = ssid, .rssi = r->rssi, .auth = auth_from_scan(r->auth_mode)};
    } else if (it->rssi < r->rssi) {  // several APs w/ the same SSID: keep the strongest
        it->rssi = r->rssi;
    }
    return 0;
}

}  // namespace

bool init() {
    g_lock = xSemaphoreCreateMutex();
    g_scan_lock = xSemaphoreCreateMutex();
    g_wifi_task = mk_task("wifi", Priority::Communication, WIFI_STACK_SIZE)(wifi_task).release();
    gatt_tcp::init();
    portal::init();
    return true;
}

Status status() {
    auto _ = lock_guard();
    return g_status;
}

Details details() {
    Details d;
    {
        auto _ = lock_guard();
        d.state = g_status.state;
        d.ssid = g_current;
        d.ap_active = g_status.ap_active != 0;
        d.ap_ssid = g_ap_ssid;
    }

    cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, d.mac.data());
    if (d.state == State::connected) {
        cyw43_wifi_get_rssi(&cyw43_state, &d.rssi);

        cyw43_arch_lwip_begin();
        d.ip = ip4_addr_get_u32(netif_ip4_addr(&netif_sta()));
        d.netmask = ip4_addr_get_u32(netif_ip4_netmask(&netif_sta()));
        d.gateway = ip4_addr_get_u32(netif_ip4_gw(&netif_sta()));
        d.dns = ip4_addr_get_u32(ip_2_ip4(dns_getserver(0)));
        cyw43_arch_lwip_end();
    }

    if (d.ap_active) {
        int max_stas = 0;
        cyw43_wifi_ap_get_max_stas(&cyw43_state, &max_stas);
        array<uint8_t, 6 * 8> macs{};
        d.ap_clients = min(max_stas, int(macs.size() / 6));
        cyw43_wifi_ap_get_stas(&cyw43_state, &d.ap_clients, macs.data());
    }

    return d;
}

settings::WifiSettings saved() {
    auto _ = lock_guard();
    return settings::g_active.wifi;
}

void network_add(settings::WifiNetwork const& network) {
    assert(network.validate() && network.used());
    {
        auto _ = lock_guard();
        auto& networks = settings::g_active.wifi.networks;
        // drop any older entry for this SSID (else the last one), then insert at the front
        auto it = find_if(networks.begin(), networks.end(), [&](auto const& x) { return x.ssid == network.ssid; });
        if (it == networks.end()) it = networks.end() - 1;
        move_backward(networks.begin(), it, it + 1);
        networks.front() = network;
    }

    printf("WiFi - network `%s` saved\n", network.ssid.data());
    notify(NOTIFY_NETWORKS);
}

bool network_remove(char const* ssid) {
    {
        auto _ = lock_guard();
        auto& networks = settings::g_active.wifi.networks;
        auto it = find_if(networks.begin(), networks.end(),
                [&](auto const& x) { return x.used() && strcmp(x.ssid.data(), ssid) == 0; });
        if (it == networks.end()) return false;

        move(it + 1, networks.end(), it);
        networks.back() = {};
    }

    printf("WiFi - network `%s` forgotten\n", ssid);
    notify(NOTIFY_NETWORKS);
    return true;
}

void networks_clear() {
    {
        auto _ = lock_guard();
        settings::g_active.wifi.networks = {};
    }

    printf("WiFi - all networks forgotten\n");
    notify(NOTIFY_NETWORKS);
}

bool connect(char const* ssid) {
    if (auto const saved = saved_networks(); find_saved(saved, ssid) == saved.count) return false;

    {
        auto _ = lock_guard();
        copy_str(g_requested, ssid);
    }
    notify(NOTIFY_CONNECT);
    return true;
}

void ap_password_set(char const* password) {
    auto _ = lock_guard();
    copy_str(settings::g_active.wifi.ap_password, password);
}

void ap_result_shown() {
    auto _ = lock_guard();
    if (g_status.state == State::connected && !g_ap_result_shown) g_ap_result_shown = xTaskGetTickCount();
}

int scan(span<ScanResult> results) {
    xSemaphoreTake(g_scan_lock, portMAX_DELAY);
    SCOPE_GUARD {
        xSemaphoreGive(g_scan_lock);
    };

    if (cyw43_wifi_scan_active(&cyw43_state)) return -1;  // a previous scan timed out & is still going

    g_scan_count = 0;
    cyw43_wifi_scan_options_t opts{};
    if (auto err = cyw43_wifi_scan(&cyw43_state, &opts, nullptr, on_scan_result)) {
        printf("ERR - WiFi - scan failed to start, err=%d\n", err);
        return -1;
    }

    auto const start = xTaskGetTickCount();
    while (cyw43_wifi_scan_active(&cyw43_state)) {
        if (ticks(SCAN_TIMEOUT) <= xTaskGetTickCount() - start) return -1;
        vTaskDelay(ticks(100ms));
    }

    auto const n = min(results.size(), g_scan_count);
    sort(g_scan_results.begin(), g_scan_results.begin() + g_scan_count,
            [](auto const& a, auto const& b) { return b.rssi < a.rssi; });
    copy_n(g_scan_results.begin(), n, results.begin());
    return int(n);
}

}  // namespace nevermore::wifi

#endif
