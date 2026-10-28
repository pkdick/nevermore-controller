#include "portal.hpp"
#include "FreeRTOS.h"  // IWYU pragma: keep
#include "lwip/sockets.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "task.h"  // IWYU pragma: keep
#include "utility/task.hpp"
#include "wifi.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>

// Generated from `portal/dist` (see `nevermore_embed_portal` in CMakeLists.txt).
extern "C" {
extern uint8_t const nevermore_portal_index_html[];
extern size_t const nevermore_portal_index_html_size;
extern uint8_t const nevermore_portal_app_js_gz[];
extern size_t const nevermore_portal_app_js_gz_size;
extern uint8_t const nevermore_portal_index_css_gz[];
extern size_t const nevermore_portal_index_css_gz_size;
}

using namespace std;
using namespace std::literals::chrono_literals;

namespace nevermore::wifi::portal {

namespace {

constexpr uint16_t HTTP_PORT = 80;
constexpr auto IO_TIMEOUT = 3s;
constexpr size_t REQUEST_MAX = 2048;  // request line + headers + body
constexpr size_t BODY_MAX = 512;
constexpr size_t RESPONSE_MAX = 4096;
constexpr char const* API = "/api/wifi";
constexpr uint32_t PORTAL_STACK_SIZE = 2048;

uint32_t ap_ip() {
    return PP_HTONL(CYW43_DEFAULT_IP_AP_ADDRESS);
}

////////////////////////////////////////////////////////////////////////////////
// JSON (just enough for the portal's requests & responses)

struct Json {
    array<char, RESPONSE_MAX> buf{};
    size_t len = 0;

    void raw(char const* fmt, ...) __attribute__((format(printf, 2, 3))) {
        va_list args;
        va_start(args, fmt);
        auto const n = vsnprintf(buf.data() + len, buf.size() - len, fmt, args);
        va_end(args);
        if (0 < n) len = min(buf.size() - 1, len + size_t(n));
    }

    void str(string_view s) {
        raw("\"");
        for (unsigned char c : s) {
            if (c == '"' || c == '\\')
                raw("\\%c", c);
            else if (c < 0x20)
                raw("\\u%04x", c);
            else
                raw("%c", c);
        }
        raw("\"");
    }

    void key(char const* k) {
        raw("\"%s\":", k);
    }

    void ip(uint32_t addr) {  // network order
        auto const* b = reinterpret_cast<uint8_t const*>(&addr);
        raw("\"%u.%u.%u.%u\"", b[0], b[1], b[2], b[3]);
    }
};

// The portal serves one request at a time, so responses share one buffer (off the stack).
Json& reply() {
    static Json s_json;
    s_json.len = 0;
    s_json.buf[0] = '\0';
    return s_json;
}

bool iequals(string_view a, string_view b) {
    return a.size() == b.size() &&
           equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return tolower(x) == tolower(y); });
}

// Value of the string field `key` in a flat JSON object. Handles the usual escapes.
optional<string> json_string(string_view body, char const* key) {
    auto const needle = string("\"") + key + "\"";
    auto pos = body.find(needle);
    if (pos == string_view::npos) return {};
    pos = body.find_first_not_of(" \t\r\n", pos + needle.size());
    if (pos == string_view::npos || body[pos] != ':') return {};
    pos = body.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == string_view::npos || body[pos] != '"') return {};

    string out;
    for (++pos; pos < body.size(); ++pos) {
        char c = body[pos];
        if (c == '"') return out;
        if (c != '\\') {
            out += c;
            continue;
        }

        if (++pos == body.size()) return {};
        switch (body[pos]) {
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
            if (body.size() < pos + 5) return {};
            auto const cp = strtoul(string(body.substr(pos + 1, 4)).c_str(), nullptr, 16);
            pos += 4;
            if (cp < 0x80) {
                out += char(cp);
            } else if (cp < 0x800) {
                out += char(0xC0 | (cp >> 6));
                out += char(0x80 | (cp & 0x3F));
            } else {  // BMP only; surrogate pairs aren't valid in SSIDs we'd care about
                out += char(0xE0 | (cp >> 12));
                out += char(0x80 | ((cp >> 6) & 0x3F));
                out += char(0x80 | (cp & 0x3F));
            }
        } break;
        default: out += body[pos]; break;  // `"`, `\`, `/`
        }
    }

    return {};  // unterminated
}

string url_decode(string_view s) {
    string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += char(strtoul(string(s.substr(i + 1, 2)).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += s[i] == '+' ? ' ' : s[i];
        }
    }
    return out;
}

////////////////////////////////////////////////////////////////////////////////
// HTTP

struct Request {
    string_view method;
    string_view path;
    string_view host;
    string_view body;
};

bool send_all(int fd, void const* data, size_t len) {
    auto const* p = static_cast<uint8_t const*>(data);
    while (len) {
        auto const n = lwip_send(fd, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= size_t(n);
    }
    return true;
}

void respond(int fd, int code, char const* reason, char const* type, void const* body, size_t len,
        char const* extra_headers = "") {
    array<char, 384> head{};
    auto const n = snprintf(head.data(), head.size(),
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %u\r\n"
            "Connection: close\r\n"
            "%s\r\n",
            code, reason, type, unsigned(len), extra_headers);
    if (send_all(fd, head.data(), size_t(n))) send_all(fd, body, len);
}

void respond_json(int fd, Json const& json, int code = 200, char const* reason = "OK") {
    respond(fd, code, reason, "application/json", json.buf.data(), json.len, "Cache-Control: no-store\r\n");
}

void respond_error(int fd, int code, char const* reason, char const* message) {
    auto& json = reply();
    json.raw("{");
    json.key("error");
    json.str(message);
    json.raw("}");
    respond_json(fd, json, code, reason);
}

void respond_ok(int fd) {
    auto& json = reply();
    json.raw("{\"ok\":true}");
    respond_json(fd, json);
}

void redirect_to_portal(int fd) {
    array<char, 96> location{};
    auto const ip = ap_ip();
    auto const* o = reinterpret_cast<uint8_t const*>(&ip);
    snprintf(location.data(), location.size(), "Location: http://%u.%u.%u.%u/\r\n", o[0], o[1], o[2], o[3]);
    respond(fd, 302, "Found", "text/plain", "", 0, location.data());
}

////////////////////////////////////////////////////////////////////////////////
// API

char const* state_name(State s) {
    switch (s) {
    case State::connected: return "connected";
    case State::connecting: return "connecting";
    default: return "disconnected";
    }
}

char const* auth_name(Auth a) {
    switch (a) {
    case Auth::open: return "OPEN";
    case Auth::wep: return "WEP";
    case Auth::wpa: return "WPA";
    case Auth::wpa2: return "WPA2";
    case Auth::wpa_wpa2: return "WPA/WPA2";
    }
    return "?";
}

int rssi_quality(int32_t rssi) {
    return clamp(2 * (int(rssi) + 100), 0, 100);
}

void api_status(int fd) {
    auto const d = details();
    auto& j = reply();
    j.raw("{");
    j.key("state"), j.str(state_name(d.state)), j.raw(",");
    j.key("ssid"), j.str(d.ssid.data()), j.raw(",");
    j.key("rssi"), j.raw("%d,", int(d.rssi));
    j.key("quality"), j.raw("%d,", d.state == State::connected ? rssi_quality(d.rssi) : 0);
    j.key("channel"), j.raw("0,");
    j.key("ip"), j.ip(d.ip), j.raw(",");
    j.key("netmask"), j.ip(d.netmask), j.raw(",");
    j.key("gateway"), j.ip(d.gateway), j.raw(",");
    j.key("dns"), j.ip(d.dns), j.raw(",");
    j.key("mac"), j.raw("\"%02X:%02X:%02X:%02X:%02X:%02X\",", d.mac[0], d.mac[1], d.mac[2], d.mac[3],
                          d.mac[4], d.mac[5]);
    j.key("hostname"), j.str(MDNS_HOSTNAME), j.raw(",");
    j.key("uptime_ms"), j.raw("%llu,", (unsigned long long)to_ms_since_boot(get_absolute_time()));
    j.key("ap_active"), j.raw(d.ap_active ? "true" : "false");
    j.raw("}");
    respond_json(fd, j);
}

void api_scan(int fd) {
    static array<ScanResult, 24> s_results;  // static: keep it off the stack
    auto const n = scan(s_results);
    if (n < 0) return respond_error(fd, 503, "Busy", "Radio occupée, réessaie dans quelques secondes");

    auto& j = reply();
    j.raw("{\"networks\":[");
    for (int i = 0; i < n; ++i) {
        auto const& r = s_results[i];
        j.raw(i ? ",{" : "{");
        j.key("ssid"), j.str(r.ssid.data()), j.raw(",");
        j.key("rssi"), j.raw("%d,", int(r.rssi));
        j.key("auth"), j.str(auth_name(r.auth));
        j.raw("}");
    }
    j.raw("]}");
    respond_json(fd, j);
}

void api_networks(int fd) {
    auto const s = saved();
    size_t count = 0;
    for (auto const& n : s.networks)
        count += n.used() ? 1 : 0;

    auto& j = reply();
    j.raw("{\"networks\":[");
    for (size_t i = 0; i < count; ++i) {
        j.raw(i ? ",{" : "{");
        j.key("ssid"), j.str(s.networks[i].ssid.data()), j.raw(",");
        j.key("priority"), j.raw("%u", unsigned(count - i));  // tried first == highest
        j.raw("}");
    }
    j.raw("]}");
    respond_json(fd, j);
}

// Fills & validates a network. Returns an error message, or nullptr if OK.
char const* make_network(settings::WifiNetwork& network, string const& ssid, string const& password) {
    if (ssid.empty()) return "Nom de réseau manquant";
    if (settings::WifiNetwork::SSID_MAX < ssid.size()) return "Nom de réseau trop long (32 octets max)";
    if (!password.empty() && (password.size() < settings::WifiNetwork::PASSWORD_MIN ||
                                     settings::WifiNetwork::PASSWORD_MAX < password.size()))
        return "Le mot de passe doit faire de 8 à 63 caractères";
    if (ssid.find('\0') != string::npos || password.find('\0') != string::npos) return "Caractère invalide";

    network = {};
    memcpy(network.ssid.data(), ssid.data(), ssid.size());
    memcpy(network.password.data(), password.data(), password.size());
    return network.validate() ? nullptr : "Réseau invalide";
}

void api_network_add(int fd, string_view body) {
    settings::WifiNetwork network{};
    auto const ssid = json_string(body, "ssid").value_or("");
    auto const password = json_string(body, "password").value_or("");
    if (auto const* err = make_network(network, ssid, password)) return respond_error(fd, 400, "Bad Request", err);

    network_add(network);
    respond_ok(fd);
}

void api_network_remove(int fd, string_view encoded_ssid) {
    auto const ssid = url_decode(encoded_ssid);
    if (!network_remove(ssid.c_str())) return respond_error(fd, 404, "Not Found", "Réseau non mémorisé");
    respond_ok(fd);
}

void api_connect(int fd, string_view body) {
    auto ssid = json_string(body, "ssid").value_or("");
    if (ssid.empty()) {  // "connect to the best saved network"
        auto const s = saved();
        if (!s.networks[0].used()) return respond_error(fd, 404, "Not Found", "Aucun réseau mémorisé");
        ssid = s.networks[0].ssid.data();
    }

    if (!connect(ssid.c_str())) {
        // The UI only connects w/o saving first for open networks.
        settings::WifiNetwork network{};
        if (auto const* err = make_network(network, ssid, "")) return respond_error(fd, 400, "Bad Request", err);
        network_add(network);
        connect(ssid.c_str());
    }
    respond_ok(fd);
}

void api_ap_status(int fd) {
    auto const d = details();
    auto& j = reply();
    j.raw("{");
    j.key("active"), j.raw(d.ap_active ? "true," : "false,");
    j.key("ssid"), j.str(d.ap_ssid.data()), j.raw(",");
    j.key("ip"), j.ip(ap_ip()), j.raw(",");
    j.key("channel"), j.raw("0,");
    j.key("sta_count"), j.raw("%d,", d.ap_clients);
    j.key("clients"), j.raw("[]");
    j.raw("}");
    respond_json(fd, j);
}

void api_ap_config_put(int fd, string_view body) {
    auto const password = json_string(body, "password");
    if (!password) return respond_error(fd, 400, "Bad Request", "Champ `password` manquant");
    if (!password->empty() && (password->size() < settings::WifiNetwork::PASSWORD_MIN ||
                                      settings::WifiNetwork::PASSWORD_MAX < password->size()))
        return respond_error(fd, 400, "Bad Request", "Le mot de passe doit faire de 8 à 63 caractères");

    ap_password_set(password->c_str());
    respond_ok(fd);
}

void route(int fd, Request const& req) {
    auto const& m = req.method;
    string_view path = req.path.substr(0, req.path.find('?'));

    // Captive portal: anything aimed at another host (OS connectivity checks, any site)
    // is sent to the portal. Makes phones pop it up on their own.
    array<char, 24> self{};
    auto const ip = ap_ip();
    auto const* o = reinterpret_cast<uint8_t const*>(&ip);
    snprintf(self.data(), self.size(), "%u.%u.%u.%u", o[0], o[1], o[2], o[3]);
    if (req.host.substr(0, req.host.find(':')) != self.data()) return redirect_to_portal(fd);

    if (m == "GET" && (path == "/" || path == "/index.html"))
        return respond(fd, 200, "OK", "text/html; charset=utf-8", nevermore_portal_index_html,
                nevermore_portal_index_html_size, "Cache-Control: no-store\r\n");
    // assets aren't cached either: a firmware update must not leave phones w/ a stale UI (they're small)
    if (m == "GET" && path == "/assets/app.js")
        return respond(fd, 200, "OK", "application/javascript", nevermore_portal_app_js_gz,
                nevermore_portal_app_js_gz_size, "Content-Encoding: gzip\r\nCache-Control: no-store\r\n");
    if (m == "GET" && path == "/assets/index.css")
        return respond(fd, 200, "OK", "text/css", nevermore_portal_index_css_gz, nevermore_portal_index_css_gz_size,
                "Content-Encoding: gzip\r\nCache-Control: no-store\r\n");

    if (!path.starts_with(API)) return redirect_to_portal(fd);
    auto const api = path.substr(strlen(API));
    constexpr string_view NETWORKS = "/networks/";

    if (m == "GET" && api == "/status") return api_status(fd);
    if (m == "GET" && api == "/scan") return api_scan(fd);
    if (m == "GET" && api == "/networks") return api_networks(fd);
    if (m == "POST" && api == "/networks") return api_network_add(fd, req.body);
    if (m == "DELETE" && api.starts_with(NETWORKS)) return api_network_remove(fd, api.substr(NETWORKS.size()));
    if (m == "POST" && api == "/connect") return api_connect(fd, req.body);
    if (m == "GET" && api == "/ap/status") return api_ap_status(fd);
    if (m == "PUT" && api == "/ap/config") return api_ap_config_put(fd, req.body);
    if (m == "GET" && api == "/vars") {
        auto& j = reply();
        j.raw("{\"vars\":[]}");
        return respond_json(fd, j);
    }
    if (m == "POST" && api == "/factory_reset") {
        networks_clear();
        ap_password_set("");
        return respond_ok(fd);
    }

    respond_error(fd, 404, "Not Found", "Inconnu");
}

// Reads one request into `buf`. Returns false on malformed/oversized/timed-out requests.
bool read_request(int fd, span<char> buf, Request& req) {
    size_t len = 0;
    size_t header_end = string_view::npos;
    for (;;) {
        if (len == buf.size()) return false;
        auto const n = lwip_recv(fd, buf.data() + len, buf.size() - len, 0);
        if (n <= 0) return false;
        len += size_t(n);

        string_view const data{buf.data(), len};
        if (header_end == string_view::npos) {
            auto const end = data.find("\r\n\r\n");
            if (end == string_view::npos) continue;
            header_end = end + 4;
        }

        string_view const head = data.substr(0, header_end);
        size_t content_length = 0;
        req.host = {};
        for (size_t line = head.find("\r\n") + 2; line < header_end;) {
            auto const eol = head.find("\r\n", line);
            auto const h = head.substr(line, eol - line);
            auto const colon = h.find(':');
            if (colon != string_view::npos) {
                auto const name = h.substr(0, colon);
                auto value = h.substr(colon + 1);
                value.remove_prefix(min(value.find_first_not_of(' '), value.size()));
                if (iequals(name, "Host")) req.host = value;
                if (iequals(name, "Content-Length"))
                    content_length = strtoul(string(value).c_str(), nullptr, 10);
            }
            line = eol + 2;
        }

        if (BODY_MAX < content_length) return false;
        if (len < header_end + content_length) continue;

        auto const request_line = head.substr(0, head.find("\r\n"));
        auto const sp1 = request_line.find(' ');
        auto const sp2 = request_line.find(' ', sp1 + 1);
        if (sp1 == string_view::npos || sp2 == string_view::npos) return false;
        req.method = request_line.substr(0, sp1);
        req.path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
        req.body = data.substr(header_end, content_length);
        return true;
    }
}

void portal_task() {
    int const listener = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    sockaddr_in addr{};
    addr.sin_len = sizeof(addr);
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(HTTP_PORT);
    addr.sin_addr.s_addr = PP_HTONL(INADDR_ANY);
    if (listener < 0 || lwip_bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
            lwip_listen(listener, 4) < 0) {
        printf("ERR - WiFi portal - failed to listen on port %u\n", unsigned(HTTP_PORT));
        vTaskDelete(nullptr);
        return;
    }

    static array<char, REQUEST_MAX> s_request;  // static: keep it off the stack

    for (;;) {
        int const fd = lwip_accept(listener, nullptr, nullptr);
        if (fd < 0) continue;

        timeval const timeout{
                .tv_sec = chrono::duration_cast<chrono::seconds>(IO_TIMEOUT).count(),
                .tv_usec = 0,
        };
        lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        lwip_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        // Hotspot clients only: the portal can change the network config & has no auth.
        sockaddr_in local{};
        socklen_t local_len = sizeof(local);
        bool const from_hotspot = lwip_getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_len) == 0 &&
                                  local.sin_addr.s_addr == ap_ip();

        Request req;
        if (from_hotspot && read_request(fd, s_request, req)) route(fd, req);

        lwip_close(fd);
    }
}

}  // namespace

void init() {
    mk_task("wifi-portal", Priority::Communication, PORTAL_STACK_SIZE)(portal_task).release();
}

}  // namespace nevermore::wifi::portal
