#include "wifi.hpp"

#if !NEVERMORE_WIFI

namespace nevermore::wifi {

bool init() {
    return true;
}

Status status() {
    return {};
}

void credentials_set(settings::WifiCredentials const&) {}

}  // namespace nevermore::wifi

#else

#include "FreeRTOS.h"  // IWYU pragma: keep
#include "gatt_stream.hpp"
#include "lwip/apps/mdns.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "semphr.h"  // IWYU pragma: keep [doesn't notice `SemaphoreHandle_t`]
#include "task.h"    // IWYU pragma: keep
#include "utility/scope_guard.hpp"
#include "utility/task.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <span>

using namespace std;

// lwIP time source. See `LWIP_FREERTOS_SYS_NOW_FROM_FREERTOS` in `lwipopts.h`.
// Wraps at 2^32 ms, as lwIP expects.
extern "C" u32_t sys_now() {
    return to_ms_since_boot(get_absolute_time());
}

namespace nevermore::wifi {

namespace {

constexpr auto JOIN_TIMEOUT = 30s;
constexpr auto RETRY_DELAY = 10s;
constexpr auto LINK_POLL_PERIOD = 500ms;
// Max stall mid-command before the client is dropped.
constexpr auto TCP_IO_TIMEOUT = 5s;
// Detect dead clients (e.g. host lost power) so a new one isn't kept waiting.
constexpr int TCP_KEEPALIVE_IDLE_SEC = 10;
constexpr int TCP_KEEPALIVE_INTERVAL_SEC = 5;
constexpr int TCP_KEEPALIVE_COUNT = 3;

constexpr uint32_t WIFI_STACK_SIZE = 1024;
constexpr uint32_t TCP_STACK_SIZE = 1024;
static_assert(gatt::ATTR_SIZE_MAX + configMINIMAL_STACK_SIZE <= TCP_STACK_SIZE);

template <typename A, typename P>
TickType_t ticks(chrono::duration<A, P> const& d) {
    return pdMS_TO_TICKS(chrono::duration_cast<chrono::milliseconds>(d).count());
}

SemaphoreHandle_t g_lock;  // guards `settings::g_active.wifi` & `g_status`
Status g_status{.state = State::disabled, .port = GATT_TCP_PORT};
TaskHandle_t g_wifi_task;

[[nodiscard]] auto lock_guard() {
    xSemaphoreTake(g_lock, portMAX_DELAY);
    return ScopeGuard{[&] { xSemaphoreGive(g_lock); }};
}

settings::WifiCredentials credentials() {
    auto _ = lock_guard();
    return settings::g_active.wifi;
}

void status_set(State state, ip4_addr_t const* addr = nullptr) {
    auto _ = lock_guard();
    g_status.state = state;
    g_status.ipv4 = {};
    if (addr) memcpy(g_status.ipv4.data(), &addr->addr, sizeof(g_status.ipv4));  // already network order
}

// Returns true if the credentials changed before `timeout`.
bool wait_for_change(TickType_t timeout) {
    return ulTaskNotifyTake(pdTRUE, timeout) != 0;
}

netif& netif_sta() {
    return cyw43_state.netif[CYW43_ITF_STA];
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

void wifi_task() {
    bool sta_enabled = false;
    // Alternate WPA2 & WPA3 transitional auth until one works.
    bool auth_wpa3 = false;

    for (;;) {
        auto const creds = credentials();
        if (!creds.enabled()) {
            status_set(State::disabled);
            printf("WiFi - disabled (no credentials)\n");
            wait_for_change(portMAX_DELAY);
            continue;
        }

        if (!sta_enabled) {
            cyw43_arch_enable_sta_mode();
            sta_enabled = true;
        }

        printf("WiFi - connecting to `%s`\n", creds.ssid.data());
        status_set(State::connecting);

        auto const* password = creds.password[0] != '\0' ? creds.password.data() : nullptr;
        uint32_t const auth = !password ? CYW43_AUTH_OPEN
                              : auth_wpa3 ? CYW43_AUTH_WPA3_WPA2_AES_PSK
                                          : CYW43_AUTH_WPA2_MIXED_PSK;

        bool changed = false;
        int link = CYW43_LINK_FAIL;
        if (auto err = cyw43_arch_wifi_connect_async(creds.ssid.data(), password, auth)) {
            printf("ERR - WiFi - failed to start connecting, err=%d\n", err);
        } else {
            auto const start = xTaskGetTickCount();
            for (;;) {
                link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
                if (link == CYW43_LINK_UP || link < 0) break;
                if (ticks(JOIN_TIMEOUT) <= xTaskGetTickCount() - start) break;
                if (wait_for_change(ticks(LINK_POLL_PERIOD))) {
                    changed = true;
                    break;
                }
            }
        }

        if (link == CYW43_LINK_UP) {
            // Latency matters more than power here (we're USB powered).
            // Power-save adds up to ~200ms per request/response exchange.
            cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
            mdns_announce();

            ip4_addr_t addr = *netif_ip4_addr(&netif_sta());
            status_set(State::connected, &addr);
            printf("WiFi - connected; IP %s, GATT TCP port %u, mDNS `%s.local`\n", ip4addr_ntoa(&addr),
                    unsigned(GATT_TCP_PORT), MDNS_HOSTNAME);

            while (!changed) {
                if (wait_for_change(ticks(LINK_POLL_PERIOD))) {
                    changed = true;
                    break;
                }

                if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP) {
                    printf("WARN - WiFi - link lost, reconnecting\n");
                    break;
                }

                // DHCP lease renewals may hand out a new address.
                if (auto curr = *netif_ip4_addr(&netif_sta()); !ip4_addr_eq(&curr, &addr)) {
                    addr = curr;
                    status_set(State::connected, &addr);
                    printf("WiFi - IP changed to %s\n", ip4addr_ntoa(&addr));
                }
            }
        } else if (!changed) {
            auto const state = link == CYW43_LINK_BADAUTH ? State::failed_auth
                               : link == CYW43_LINK_NONET ? State::failed_no_network
                                                          : State::failed;
            status_set(state);
            printf("ERR - WiFi - connection failed, link status=%d; retrying in %llds\n", link,
                    chrono::duration_cast<chrono::seconds>(RETRY_DELAY).count());
            if (password) auth_wpa3 = !auth_wpa3;
        }

        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);

        // back off after a failed join, unless the user just gave us new credentials
        if (!changed && link != CYW43_LINK_UP) wait_for_change(ticks(RETRY_DELAY));
    }
}

struct TcpStream {
    int fd = -1;
    bool failed = false;
    size_t used = 0;
    array<char, 512> buffer{};  // coalesce small writes into few segments

    [[nodiscard]] char const* name() const {
        return "GATT TCP";
    }

    [[nodiscard]] bool recv(span<uint8_t> dst) {
        while (!dst.empty()) {
            auto const n = lwip_recv(fd, dst.data(), dst.size(), 0);
            if (n <= 0) {
                if (!failed) printf("ERR - GATT TCP - lost connection mid-recv\n");
                failed = true;
                return false;
            }

            dst = dst.subspan(size_t(n));
        }

        return true;
    }

    void send(span<char const> xs) {
        while (!xs.empty()) {
            if (used == buffer.size()) flush();

            auto const n = min(buffer.size() - used, xs.size());
            memcpy(buffer.data() + used, xs.data(), n);
            used += n;
            xs = xs.subspan(n);
        }
    }

    void flush() {
        for (size_t sent = 0; !failed && sent < used;) {
            auto const n = lwip_send(fd, buffer.data() + sent, used - sent, 0);
            if (n <= 0) {
                printf("ERR - GATT TCP - lost connection mid-send\n");
                failed = true;
                break;
            }

            sent += size_t(n);
        }

        used = 0;
    }

    void open(int fd_) {
        assert(fd < 0);
        fd = fd_;
        failed = false;
        used = 0;

        int const yes = 1;
        lwip_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        lwip_setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
        lwip_setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &TCP_KEEPALIVE_IDLE_SEC, sizeof(int));
        lwip_setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &TCP_KEEPALIVE_INTERVAL_SEC, sizeof(int));
        lwip_setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &TCP_KEEPALIVE_COUNT, sizeof(int));

        timeval const timeout{
                .tv_sec = chrono::duration_cast<chrono::seconds>(TCP_IO_TIMEOUT).count(),
                .tv_usec = 0,
        };
        lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        lwip_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    }

    void close() {
        if (fd < 0) return;

        lwip_close(fd);
        fd = -1;
    }
};

// One client at a time. A new client replaces the current one; the host
// reconnecting after a network hiccup must not be locked out by a half-open
// connection.
void tcp_server_task() {
    int const listener = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    sockaddr_in addr{};
    addr.sin_len = sizeof(addr);
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(GATT_TCP_PORT);
    addr.sin_addr.s_addr = PP_HTONL(INADDR_ANY);
    if (listener < 0 || lwip_bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
            lwip_listen(listener, 1) < 0) {
        printf("ERR - GATT TCP - failed to listen on port %u\n", unsigned(GATT_TCP_PORT));
        vTaskDelete(nullptr);
        return;
    }

    static TcpStream s_stream;  // static: keep the buffer off the stack
    auto& stream = s_stream;

    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listener, &fds);
        if (0 <= stream.fd) FD_SET(stream.fd, &fds);

        if (lwip_select(max(listener, stream.fd) + 1, &fds, nullptr, nullptr, nullptr) <= 0) continue;

        // handle the current client before accepting, an accepted fd may reuse its slot
        if (0 <= stream.fd && FD_ISSET(stream.fd, &fds)) {
            uint8_t c = 0;
            if (lwip_recv(stream.fd, &c, 1, 0) <= 0) {
                printf("GATT TCP - client disconnected\n");
                stream.close();
            } else {
                gatt_stream::dispatch(stream, c);
                if (stream.failed) stream.close();
            }
        }

        if (FD_ISSET(listener, &fds)) {
            if (int const fd = lwip_accept(listener, nullptr, nullptr); 0 <= fd) {
                if (0 <= stream.fd) {
                    printf("GATT TCP - new client replaces the current one\n");
                    stream.close();
                }

                stream.open(fd);
                printf("GATT TCP - client connected\n");
            }
        }
    }
}

}  // namespace

bool init() {
    g_lock = xSemaphoreCreateMutex();
    g_wifi_task = mk_task("wifi", Priority::Communication, WIFI_STACK_SIZE)(wifi_task).release();
    mk_task("gatt-tcp", Priority::Communication, TCP_STACK_SIZE)(tcp_server_task).release();
    return true;
}

Status status() {
    auto _ = lock_guard();
    return g_status;
}

void credentials_set(settings::WifiCredentials const& credentials) {
    assert(credentials.validate());
    {
        auto _ = lock_guard();
        settings::g_active.wifi = credentials;
    }

    printf("WiFi - credentials updated (SSID `%s`)\n", credentials.ssid.data());
    xTaskNotifyGive(g_wifi_task);
}

}  // namespace nevermore::wifi

#endif
