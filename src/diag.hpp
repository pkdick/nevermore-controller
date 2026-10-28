#pragma once

#include <cstddef>
#include <cstdint>

// Crash reports: records why the firmware died (HardFault, panic, stack overflow, failed alloc)
// in the watchdog scratch registers, which survive the reboot, then reboots. The next boot
// reports it.
// Diagnostic build (`NEVERMORE_DIAG`): also prints a heartbeat (heap, every task's state & free stack).
namespace nevermore::diag {

enum class Cause : uint8_t {
    none = 0,
    hardfault,
    panic,
    stack_overflow,
    malloc_failed,
};

// Call first thing in `main`: takes (and clears) the previous boot's report.
void init();
// Call once stdio is up: prints the previous report (& starts the heartbeat).
void start();

// Records `cause` (first one wins) & reboots. `a` & `b` are cause specific.
[[noreturn]] void crashed(Cause, uint32_t a, uint32_t b, char const* task_name);

}  // namespace nevermore::diag
