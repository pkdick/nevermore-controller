#pragma once

#include "bluetooth.h"
#include "gatt.hpp"
#include <cstdint>
#include <optional>
#include <span>

namespace nevermore::gatt::configuration {

std::optional<uint16_t> attr_read(hci_con_handle_t, uint16_t attr, uint16_t offset, std::span<uint8_t>);
std::optional<int> attr_write(hci_con_handle_t, uint16_t attr, std::span<uint8_t const>);

bool init();
void disconnected(hci_con_handle_t);

// If `writer` paused VOC calibration, it's resumed by `calibration_resume_if_abandoned`.
void writer_gone(gatt::Writer);
// Call when not filtering: resuming mid-filtering would teach the sensors that dirty air is normal.
void calibration_resume_if_abandoned();

}  // namespace nevermore::gatt::configuration
