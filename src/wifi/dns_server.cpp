#include "dns_server.hpp"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace std;

namespace nevermore::wifi::dns_server {

namespace {

constexpr uint16_t DNS_PORT = 53;
constexpr size_t DNS_MSG_MAX = 512;  // classic UDP DNS limit
constexpr size_t HEADER_SIZE = 12;
constexpr uint16_t TYPE_A = 1;
constexpr uint16_t CLASS_IN = 1;
constexpr uint32_t ANSWER_TTL_SEC = 60;

udp_pcb* g_pcb = nullptr;
ip4_addr_t g_self{};

uint16_t be16(uint8_t const* p) {
    return uint16_t((p[0] << 8) | p[1]);
}

uint8_t* put_be16(uint8_t* p, uint16_t x) {
    p[0] = uint8_t(x >> 8);
    p[1] = uint8_t(x);
    return p + 2;
}

// Runs in the lwIP thread.
void on_recv(void*, udp_pcb* pcb, pbuf* p, ip_addr_t const* addr, u16_t port) {
    array<uint8_t, DNS_MSG_MAX> msg{};
    auto const len = pbuf_copy_partial(p, msg.data(), msg.size(), 0);
    pbuf_free(p);
    if (len < HEADER_SIZE) return;

    bool const is_query = (msg[2] & 0x80) == 0;
    auto const opcode = (msg[2] >> 3) & 0x0F;
    if (!is_query || opcode != 0 || be16(&msg[4]) != 1) return;  // only standard, single question

    // walk the question's name
    size_t i = HEADER_SIZE;
    while (i < len && msg[i] != 0) {
        if ((msg[i] & 0xC0) != 0) return;  // no compression in questions
        i += msg[i] + 1;
    }
    if (len < i + 1 + 4) return;  // name terminator + type + class
    auto const qtype = be16(&msg[i + 1]);
    auto const qclass = be16(&msg[i + 3]);
    size_t const question_end = i + 5;

    bool const answer = qtype == TYPE_A && qclass == CLASS_IN;
    size_t const reply_len = question_end + (answer ? 16 : 0);
    if (msg.size() < reply_len) return;

    msg[2] = 0x84 | (msg[2] & 0x01);  // response, authoritative, keep RD
    msg[3] = 0x00;                    // no error (other types: empty answer, no AAAA)
    put_be16(&msg[6], answer ? 1 : 0);  // ANCOUNT
    put_be16(&msg[8], 0);               // NSCOUNT
    put_be16(&msg[10], 0);              // ARCOUNT

    if (answer) {
        auto* q = &msg[question_end];
        q = put_be16(q, 0xC000 | HEADER_SIZE);  // pointer to the question's name
        q = put_be16(q, TYPE_A);
        q = put_be16(q, CLASS_IN);
        q = put_be16(q, uint16_t(ANSWER_TTL_SEC >> 16));
        q = put_be16(q, uint16_t(ANSWER_TTL_SEC));
        q = put_be16(q, 4);
        memcpy(q, &g_self.addr, 4);  // already network order
    }

    auto* reply = pbuf_alloc(PBUF_TRANSPORT, u16_t(reply_len), PBUF_RAM);
    if (!reply) return;
    memcpy(reply->payload, msg.data(), reply_len);
    udp_sendto(pcb, reply, addr, port);
    pbuf_free(reply);
}

}  // namespace

bool start(ip4_addr_t const& self) {
    if (g_pcb) return true;

    g_self = self;
    g_pcb = udp_new();
    if (!g_pcb) return false;

    if (udp_bind(g_pcb, IP_ANY_TYPE, DNS_PORT) != ERR_OK) {
        printf("ERR - WiFi - captive DNS failed to bind\n");
        udp_remove(g_pcb);
        g_pcb = nullptr;
        return false;
    }

    udp_recv(g_pcb, on_recv, nullptr);
    return true;
}

void stop() {
    if (!g_pcb) return;

    udp_remove(g_pcb);
    g_pcb = nullptr;
}

}  // namespace nevermore::wifi::dns_server
