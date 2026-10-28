#include "diag.hpp"
#include "FreeRTOS.h"  // IWYU pragma: keep
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "pico/platform.h"
#include "picowota/reboot.h"
#include "task.h"
#include "utility/task.hpp"
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>

using namespace std;
using namespace std::literals::chrono_literals;

namespace nevermore::diag {

namespace {

// Watchdog scratch 0..3 are free: the SDK uses 4..7, picowota 5 & 6.
//  [0] = MAGIC | core << 8 | cause
//  [1], [2] = cause specific (PC & LR, caller & size, ...)
//  [3] = task name: first 2 & last 2 chars (e.g. `wifi` vs `wial` for `wifi-portal`)
constexpr uint32_t MAGIC = 0x4e560000;  // "NV"
constexpr uint32_t MAGIC_MASK = 0xffff0000;

struct Report {
    Cause cause = Cause::none;
    uint8_t core = 0;
    uint32_t a = 0, b = 0;
    array<char, 5> task{};
};

Report g_previous;
bool g_watchdog_reboot = false;

uint32_t task_tag(char const* name) {
    if (!name) return 0;
    auto const n = strlen(name);
    char tag[4]{};
    for (size_t i = 0; i < 2 && i < n; ++i)
        tag[i] = name[i];
    for (size_t i = 0; i < 2 && 2 + i < n; ++i)
        tag[2 + i] = name[n - 2 + i];
    uint32_t x;
    memcpy(&x, tag, sizeof(x));
    return x;
}

char const* cause_name(Cause c) {
    switch (c) {
    case Cause::none: return "none";
    case Cause::hardfault: return "HardFault (a=PC, b=LR)";
    case Cause::panic: return "panic (a=caller, b=0)";
    case Cause::stack_overflow: return "stack overflow (a=0, b=0)";
    case Cause::malloc_failed: return "malloc failed (a=caller, b=size)";
    }
    return "?";
}

void print_previous() {
    if (g_previous.cause == Cause::none) {
        printf("DIAG - previous boot: no crash recorded%s\n", g_watchdog_reboot ? " (watchdog reboot)" : "");
        return;
    }

    printf("DIAG - previous boot CRASHED: %s; a=0x%08lx b=0x%08lx core=%u task=`%s`\n",
            cause_name(g_previous.cause), (unsigned long)g_previous.a, (unsigned long)g_previous.b,
            g_previous.core, g_previous.task.data());
}

#if NEVERMORE_DIAG

constexpr auto HEARTBEAT_PERIOD = 5s;
constexpr size_t TASKS_MAX = 32;

char state_char(eTaskState s) {
    switch (s) {
    case eRunning: return 'X';
    case eReady: return 'R';
    case eBlocked: return 'B';
    case eSuspended: return 'S';
    case eDeleted: return 'D';
    default: return '?';
    }
}

void heartbeat() {
    static array<TaskStatus_t, TASKS_MAX> tasks;
    for (uint32_t n = 0;; ++n) {
        auto const count = uxTaskGetSystemState(tasks.data(), tasks.size(), nullptr);
        printf("DIAG #%lu t=%lus heap free=%u min=%u tasks=%u\n", (unsigned long)n,
                (unsigned long)(xTaskGetTickCount() / configTICK_RATE_HZ), unsigned(xPortGetFreeHeapSize()),
                unsigned(xPortGetMinimumEverFreeHeapSize()), unsigned(count));
        for (size_t i = 0; i < count; ++i) {
            auto const& t = tasks[i];
            printf("DIAG   %-20s %c prio=%2u stack_free=%4lu words\n", t.pcTaskName, state_char(t.eCurrentState),
                    unsigned(t.uxCurrentPriority), (unsigned long)t.usStackHighWaterMark);
        }
        if (n % 6 == 0) print_previous();  // in case the host connected late
        task_delay<HEARTBEAT_PERIOD>();
    }
}

#endif

}  // namespace

void init() {
    g_watchdog_reboot = watchdog_caused_reboot();
    auto const head = watchdog_hw->scratch[0];
    if ((head & MAGIC_MASK) == MAGIC) {
        g_previous.cause = Cause(head & 0xff);
        g_previous.core = (head >> 8) & 0xff;
        g_previous.a = watchdog_hw->scratch[1];
        g_previous.b = watchdog_hw->scratch[2];
        uint32_t const tag = watchdog_hw->scratch[3];
        memcpy(g_previous.task.data(), &tag, 4);
    }
    for (int i = 0; i < 4; ++i)
        watchdog_hw->scratch[i] = 0;
}

void start() {
    print_previous();
#if NEVERMORE_DIAG
    mk_task("diag", Priority::Startup, 1024)(heartbeat).release();
#endif
}

void crashed(Cause cause, uint32_t a, uint32_t b, char const* task_name) {
    if ((watchdog_hw->scratch[0] & MAGIC_MASK) != MAGIC) {  // first one wins
        watchdog_hw->scratch[1] = a;
        watchdog_hw->scratch[2] = b;
        watchdog_hw->scratch[3] = task_tag(task_name);
        watchdog_hw->scratch[0] = MAGIC | (get_core_num() << 8) | uint32_t(cause);
    }
    picowota_reboot(false);
}

}  // namespace nevermore::diag

using namespace nevermore;

namespace {

char const* current_task_name() {
    // the scheduler may not be running yet (or we're in a fault handler); best effort
    auto const t = xTaskGetCurrentTaskHandle();
    return t ? pcTaskGetName(t) : "-";
}

}  // namespace

extern "C" {

// Stacked frame: r0 r1 r2 r3 r12 lr pc xpsr
[[gnu::used]] void nevermore_diag_hardfault(uint32_t const* frame) {
    diag::crashed(diag::Cause::hardfault, frame[6], frame[5], current_task_name());
}

// ARMv6-M compatible (RP2040): no IT blocks, and a plain `b` may not reach.
[[gnu::naked]] void isr_hardfault() {
    asm volatile(
            "movs r0, #4\n"
            "mov r1, lr\n"
            "tst r0, r1\n"
            "beq 1f\n"
            "mrs r0, psp\n"
            "b 2f\n"
            "1: mrs r0, msp\n"
            "2: ldr r1, 3f\n"
            "bx r1\n"
            ".align 2\n"
            "3: .word nevermore_diag_hardfault\n");
}

// Linked w/ `--wrap=panic`
[[noreturn]] void __wrap_panic(char const* fmt, ...) {
    auto const caller = uint32_t(__builtin_return_address(0));
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);  // might not make it out, the report will
    va_end(args);
    diag::crashed(diag::Cause::panic, caller, 0, current_task_name());
}

// Linked w/ `--wrap=pvPortMalloc`
void* __real_pvPortMalloc(size_t);
void* __wrap_pvPortMalloc(size_t size) {
    auto const caller = uint32_t(__builtin_return_address(0));
    auto* p = __real_pvPortMalloc(size);
    if (!p && size) diag::crashed(diag::Cause::malloc_failed, caller, size, current_task_name());
    return p;
}
}

