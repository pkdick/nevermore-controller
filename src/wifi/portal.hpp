#pragma once

// Captive web portal for the configuration hotspot (http://192.168.4.1/).
// UI: `portal/` (esp_wifi_manager's web UI, adapted), REST API under `/api/wifi`.
// Only answers clients of the hotspot; the home network doesn't see it.
namespace nevermore::wifi::portal {

// Launches the HTTP server task.
// PRECONDITION: lwIP is up (`cyw43_arch_init`).
void init();

}  // namespace nevermore::wifi::portal
