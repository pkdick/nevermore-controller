#include "usb_cdc_gatt.hpp"
#include "class/cdc/cdc_device.h"
#include "device/usbd.h"
#include "gatt_stream.hpp"
#include "sdk/task.hpp"
#include "tusb.h"
#include <cstdio>
#include <span>

using namespace std;

namespace nevermore {

namespace {

constexpr int GATT_ITF = 1;  // CDC interface #, 0 is stdio
constexpr auto READ_FAILED_YIELD_PERIOD = 1ms;
constexpr auto NOT_CONNECTED_YIELD_PERIOD = 100ms;

struct UsbCdcStream {
    [[nodiscard]] char const* name() const {
        return "USB CDC";
    }

    [[nodiscard]] gatt::Writer writer() const {
        return gatt::WRITER_USB;
    }

    [[nodiscard]] bool recv(span<uint8_t> dst) {
        auto const end = dst.end();
        for (auto curr = dst.begin(); curr != end;) {
            if (!tud_cdc_n_connected(GATT_ITF)) {
                printf("ERR - GATT CDC - lost connection mid-recv\n");
                return false;
            }

            auto n = tud_cdc_n_read(GATT_ITF, &*curr, end - curr);
            if (n == 0) vTaskDelay(0);  // yield to other tasks
            curr += n;
        }

        return true;
    }

    void send(span<char const> xs) {
        auto const end = xs.end();
        for (auto curr = xs.begin(); curr != end;) {
            if (!tud_cdc_n_connected(GATT_ITF)) {
                printf("ERR - GATT CDC - lost connection mid-send\n");
                tud_cdc_n_write_clear(GATT_ITF);  // drop anything that's still pending
                return;
            }

            curr += tud_cdc_n_write(GATT_ITF, &*curr, end - curr);
        }
    }

    void flush() {
        tud_cdc_n_write_flush(GATT_ITF);
    }
};

}  // namespace

void usb_cdc_gatt_task() {
    UsbCdcStream stream;
    bool connected = false;

    for (;;) {
        // HACK: Don't read before there's a connection. It causes panics
        //       in the rp2040 platform code. Guess we just need to race between
        //       the two.
        //       To reproduce:  Disable connected check.
        //                      Power on application w/ USB unplugged.
        //                      Plug in USB -> panic.
        if (!tud_cdc_n_connected(GATT_ITF)) {
            if (connected) {  // host closed the port (Klipper stopped, cable unplugged, ...)
                connected = false;
                gatt::writer_gone(gatt::WRITER_USB);
            }
            task_delay<NOT_CONNECTED_YIELD_PERIOD>();
            continue;
        }
        connected = true;

        auto const c = tud_cdc_n_read_char(GATT_ITF);
        if (c < 0) {  // failed or timeout, try again in a bit
            task_delay<READ_FAILED_YIELD_PERIOD>();
            continue;
        }

        gatt_stream::dispatch(stream, uint8_t(c));
    }
}

}  // namespace nevermore
