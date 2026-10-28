#pragma once

#include "btstack_config.h"
#include <cstdint>
#include <optional>
#include <span>

namespace nevermore::gatt {

constexpr size_t ATTR_SIZE_MAX = HCI_ACL_PAYLOAD_SIZE;

// Setup bluetooth and GATT services.
// Caller is responsible for subsequently calling `btstack_run_loop_execute`.
bool init();

// Who writes an attribute: a BLE connection handle, or a stream transport.
// Lets settings held on behalf of a host (fan override, paused VOC calibration) be released
// when that host goes away (network down, Klipper stopped, ...).
using Writer = uint32_t;
constexpr Writer WRITER_NONE = UINT32_MAX;  // e.g. the display
constexpr Writer WRITER_USB = 0x10000;       // above any BLE connection handle
constexpr Writer WRITER_TCP = 0x10001;

// The writer of the attribute being written. Only meaningful inside `attr_write` handlers.
Writer writer(uint16_t conn);
// `writer` is gone (BLE disconnect, stream transport closed).
void writer_gone(Writer);

// used by the stream transports (USB CDC, TCP)
std::optional<uint16_t> read(uint16_t attr, std::span<uint8_t>);
std::optional<uint16_t> write(uint16_t attr, std::span<uint8_t const>, Writer);

}  // namespace nevermore::gatt
