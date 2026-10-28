#include "gatt_tcp.hpp"
#include "FreeRTOS.h"  // IWYU pragma: keep
#include "gatt_stream.hpp"
#include "lwip/sockets.h"
#include "task.h"  // IWYU pragma: keep
#include "utility/task.hpp"
#include "wifi.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <span>

using namespace std;
using namespace std::literals::chrono_literals;

namespace nevermore::wifi::gatt_tcp {

namespace {

// Max stall mid-command before the client is dropped.
constexpr auto TCP_IO_TIMEOUT = 5s;
// Detect dead clients (e.g. host lost power) so a new one isn't kept waiting.
constexpr int TCP_KEEPALIVE_IDLE_SEC = 10;
constexpr int TCP_KEEPALIVE_INTERVAL_SEC = 5;
constexpr int TCP_KEEPALIVE_COUNT = 3;

constexpr uint32_t TCP_STACK_SIZE = 1024;
static_assert(gatt::ATTR_SIZE_MAX + configMINIMAL_STACK_SIZE <= TCP_STACK_SIZE);

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

void init() {
    mk_task("gatt-tcp", Priority::Communication, TCP_STACK_SIZE)(tcp_server_task).release();
}

}  // namespace nevermore::wifi::gatt_tcp
