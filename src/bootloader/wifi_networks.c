// Linked into the picowota bootloader (not the app): offers the Wi-Fi networks the app saved in
// its settings, so OTA updates work over Wi-Fi w/o putting credentials in the image.
// Mirrors `settings.cpp`'s slot validation & selection.

#include "hardware/regs/addressmap.h"
#include "settings_layout.h"
#include "wifi_networks.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// defined by the bootloader's linker script
extern char app_store_offset[];
extern char app_store_len[];

#define NETWORK_SIZE (NEVERMORE_SETTINGS_WIFI_SSID_SIZE + NEVERMORE_SETTINGS_WIFI_PASSWORD_SIZE)

_Static_assert(sizeof(((struct picowota_wifi_network*)0)->ssid) == NEVERMORE_SETTINGS_WIFI_SSID_SIZE, "");
_Static_assert(sizeof(((struct picowota_wifi_network*)0)->password) == NEVERMORE_SETTINGS_WIFI_PASSWORD_SIZE, "");

static uint32_t u32_at(uint8_t const* p) {
    uint32_t x;
    memcpy(&x, p, sizeof(x));
    return x;
}

// Same as `nevermore::crc32` (reflected 0xEDB88320, no final XOR).
static uint32_t crc32(uint8_t const* p, uint8_t const* end, uint32_t crc) {
    for (; p < end; ++p) {
        crc ^= *p;
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return crc;
}

// Size of the settings in `slot`, or 0 if invalid.
static uint32_t slot_validate(uint8_t const* slot) {
    uint32_t const size = u32_at(slot + 8);
    if (size < NEVERMORE_SETTINGS_HEADER_SIZE || NEVERMORE_SETTINGS_SLOT_SIZE < size) return 0;
    if (crc32(slot + 4, slot + size, 0xFF) != u32_at(slot)) return 0;  // CRC skips its own field
    if (u32_at(slot + 4) != NEVERMORE_SETTINGS_VERSION_V0) return 0;
    return size;
}

// Latest valid slot (& its size), or NULL.
static uint8_t const* slot_latest(uint32_t* size_out) {
    uint8_t const* const store = (uint8_t const*)(XIP_BASE + (uintptr_t)app_store_offset);
    size_t const slots = (uintptr_t)app_store_len / NEVERMORE_SETTINGS_SLOT_SIZE;

    uint8_t const* latest = NULL;
    uint8_t latest_counter = 0;
    for (size_t i = 0; i < slots; ++i) {
        uint8_t const* slot = store + i * NEVERMORE_SETTINGS_SLOT_SIZE;
        uint32_t const size = slot_validate(slot);
        if (!size) continue;

        // older (smaller) settings have a zero counter
        uint8_t const counter = NEVERMORE_SETTINGS_OFFSET_SAVE_COUNTER < size ? slot[NEVERMORE_SETTINGS_OFFSET_SAVE_COUNTER] : 0;
        // next slot not next in sequence -> counter or slot wrap, i.e. it's older
        if (latest && (uint8_t)(latest_counter + 1) != counter) break;

        latest = slot;
        latest_counter = counter;
        *size_out = size;
    }

    return latest;
}

bool picowota_wifi_network(unsigned index, struct picowota_wifi_network* out) {
    if (NEVERMORE_SETTINGS_WIFI_NETWORKS_MAX <= index) return false;

    uint32_t size = 0;
    uint8_t const* slot = slot_latest(&size);
    if (!slot) return false;

    size_t const offset = NEVERMORE_SETTINGS_OFFSET_WIFI + index * NETWORK_SIZE;
    if (size < offset + NETWORK_SIZE) return false;  // settings predate Wi-Fi

    char const* ssid = (char const*)slot + offset;
    char const* password = ssid + NEVERMORE_SETTINGS_WIFI_SSID_SIZE;
    // used networks come first: an empty SSID ends the list
    if (ssid[0] == '\0') return false;
    if (!memchr(ssid, '\0', NEVERMORE_SETTINGS_WIFI_SSID_SIZE)) return false;
    if (!memchr(password, '\0', NEVERMORE_SETTINGS_WIFI_PASSWORD_SIZE)) return false;

    memcpy(out->ssid, ssid, NEVERMORE_SETTINGS_WIFI_SSID_SIZE);
    memcpy(out->password, password, NEVERMORE_SETTINGS_WIFI_PASSWORD_SIZE);
    return true;
}
