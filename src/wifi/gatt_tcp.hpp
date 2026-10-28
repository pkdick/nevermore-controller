#pragma once

namespace nevermore::wifi::gatt_tcp {

// Launches the GATT-over-TCP server task (port `wifi::GATT_TCP_PORT`).
// PRECONDITION: lwIP is up (`cyw43_arch_init`) & `gatt_stream::init` is done.
void init();

}  // namespace nevermore::wifi::gatt_tcp
