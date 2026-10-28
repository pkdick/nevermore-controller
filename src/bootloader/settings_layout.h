#pragma once

// Layout of the persisted settings (`settings::SettingsV0`, `settings.hpp`) as seen by the
// picowota bootloader (C), which reads the saved Wi-Fi networks. Checked by `settings.cpp`.
// The layout is persisted & append-only, so these never change.

#define NEVERMORE_SETTINGS_SLOT_SIZE 4096  // one settings slot per flash sector
#define NEVERMORE_SETTINGS_HEADER_SIZE 12  // u32 crc, u32 version, u32 size
#define NEVERMORE_SETTINGS_VERSION_V0 0
#define NEVERMORE_SETTINGS_OFFSET_SAVE_COUNTER 68
#define NEVERMORE_SETTINGS_OFFSET_WIFI 289
#define NEVERMORE_SETTINGS_WIFI_NETWORKS_MAX 5
#define NEVERMORE_SETTINGS_WIFI_SSID_SIZE 33      // NUL terminated
#define NEVERMORE_SETTINGS_WIFI_PASSWORD_SIZE 64  // NUL terminated; empty -> open network
