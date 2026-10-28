#pragma once

#include "lwip/ip4_addr.h"

// Captive-portal DNS: answers every A query w/ the hotspot's own address,
// so phones & laptops open the portal by themselves.
namespace nevermore::wifi::dns_server {

// PRECONDITION: lwIP core lock held (`cyw43_arch_lwip_begin`).
bool start(ip4_addr_t const& self);
// PRECONDITION: lwIP core lock held (`cyw43_arch_lwip_begin`).
void stop();

}  // namespace nevermore::wifi::dns_server
