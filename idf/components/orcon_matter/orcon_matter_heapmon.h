#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sdkconfig.h>
#include "orcon_matter_out.h"
// Heap monitor for hardware measurements (diagnostics); see orcon_matter_heapmon.cpp.
// Without CONFIG_ORCON_MATTER_HEAPMON every call below is an empty inline function.
namespace chip { namespace DeviceLayer { struct ChipDeviceEvent; } }
namespace orcon_matter {
enum class MonPhase : uint8_t { MatterStart, Window, Commission };   // phases that other code reports
#if CONFIG_ORCON_MATTER_HEAPMON
constexpr unsigned kHeapMonitorPeriodMs = 20;
void heapMonitorStart();                          // once, before Wi-Fi and Matter: hook, event marks, first sample
void heapMonitorSample();                         // every kHeapMonitorPeriodMs, from the telemetry task only
void heapMonitorReport(Out& o);                   // same task: prints the active phases, adds its [mem] fields
void heapMonitorSet(MonPhase p, bool on);         // any task
void heapMonitorChipEvent(const chip::DeviceLayer::ChipDeviceEvent* e);   // the esp_matter event callback (CHIP task)
bool heapMonitorAllocHookOk();                    // the failed-allocation hook is registered
bool heapMonitorEventsOk();                       // Wi-Fi/IP event marks registered, no registration error
#else
inline void heapMonitorStart() {}
inline void heapMonitorSample() {}
inline void heapMonitorReport(Out&) {}
inline void heapMonitorSet(MonPhase, bool) {}
inline void heapMonitorChipEvent(const chip::DeviceLayer::ChipDeviceEvent*) {}
inline bool heapMonitorAllocHookOk() { return true; }
inline bool heapMonitorEventsOk() { return true; }
#endif
}  // namespace orcon_matter
