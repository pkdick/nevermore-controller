#include "gatt_stream.hpp"
#include "FreeRTOS.h"  // IWYU pragma: keep
#include "nevermore.h"
#include "semphr.h"  // IWYU pragma: keep [doesn't notice `SemaphoreHandle_t`]
#include "utility/scope_guard.hpp"
#include <cassert>

using namespace std;

namespace nevermore::gatt_stream {

namespace {

static_assert(profile_data[0] == 1, "DB format has changed; update consumers & assert");
static_assert(sizeof(profile_data) <= numeric_limits<PayloadSize>::max());

SemaphoreHandle_t g_lock;

[[nodiscard]] auto lock_guard() {
    assert(g_lock && "`gatt_stream::init` must be called first");
    xSemaphoreTake(g_lock, portMAX_DELAY);
    return ScopeGuard{[&] { xSemaphoreGive(g_lock); }};
}

}  // namespace

void init() {
    g_lock = xSemaphoreCreateMutex();
}

span<uint8_t const> database() {
    return {profile_data, sizeof(profile_data)};
}

optional<uint16_t> read(uint16_t attr, span<uint8_t> buffer) {
    auto _ = lock_guard();
    return gatt::read(attr, buffer);
}

optional<uint16_t> write(uint16_t attr, span<uint8_t const> buffer, gatt::Writer writer) {
    auto _ = lock_guard();
    return gatt::write(attr, buffer, writer);
}

}  // namespace nevermore::gatt_stream
