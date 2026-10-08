#pragma once
//
// telemetry.hpp - read-only GPU telemetry for the GRIN Cuckatoo32 miner (Windows x64).
//
// Backends (chosen at Telemetry::init()):
//   * "nvml"       - nvml.dll loaded with LoadLibraryW() and resolved with GetProcAddress().
//                    Nothing is linked statically and the CUDA Toolkit is NOT required at
//                    runtime; only the NVIDIA display driver (which ships nvml.dll in
//                    C:\Windows\System32) is needed.
//   * "nvidia-smi" - fallback: parse `nvidia-smi --query-gpu=... --format=csv,noheader,nounits`
//                    output captured through CreateProcessW() with a timeout.
//   * "none"       - neither backend is usable; every read() then fails with a message.
//
// Verified on the target machine (RTX 4060 Ti 8 GB, driver 617.14, CUDA UMD 13.4):
//   * nvml.dll is present in C:\Windows\System32  -> NVML backend is the normal path.
//   * total VRAM 8585740288 B (8188 MiB), power limits 100000/160000 mW.
//   * NVML_TEMPERATURE_THRESHOLD_MEM_MAX and NVML_FI_DEV_MEMORY_TEMP are NOT supported
//     on this SKU, so memory_temperature_c is reported as -1.
//
// All functions are non-throwing: failures return false and fill `error` with a
// human-readable message.
//
#include <cstdint>
#include <string>
#include <vector>

namespace grin {

// A single point-in-time snapshot of one GPU.
// Fields that the current SKU/backend cannot provide are set to -1 (or 0 for the
// mandatory fields) and `valid` reflects whether the core readings were obtained.
struct GpuTelemetry {
    unsigned    index = 0;
    std::string name;
    double      temperature_c = 0.0;
    double      memory_temperature_c = -1.0;   // -1 = unsupported on this SKU
    double      power_w = 0.0;
    double      power_limit_w = 0.0;
    double      power_min_w = 0.0;
    double      power_max_w = 0.0;
    double      fan_percent = -1.0;            // -1 = unsupported
    double      core_clock_mhz = 0.0;
    double      memory_clock_mhz = 0.0;
    uint64_t    memory_used_bytes = 0;
    uint64_t    memory_total_bytes = 0;
    double      utilization_gpu_percent = -1.0;
    double      utilization_memory_percent = -1.0;
    uint64_t    throttle_reasons = 0;          // NVML bitmask
    double      pcie_tx_kib_per_s = -1.0;
    double      pcie_rx_kib_per_s = -1.0;
    uint64_t    timestamp_unix_ms = 0;
    bool        valid = false;
};

// Hardware temperature thresholds reported by the NVML backend
// (nvmlDeviceGetTemperatureThreshold). Unsupported thresholds stay at -1; on the
// target RTX 4060 Ti memory_max_c is -1 because NVML_TEMPERATURE_THRESHOLD_MEM_MAX
// returns NVML_ERROR_NOT_SUPPORTED. `valid` is true when at least one threshold was
// read successfully. These values are useful for a thermal watchdog in the miner.
struct ThermalLimits {
    double slowdown_c = -1.0;    // hardware slowdown temperature
    double shutdown_c = -1.0;    // hardware shutdown temperature
    double gpu_max_c = -1.0;     // GPU temperature at which clocks may drop below base
    double memory_max_c = -1.0;  // memory temperature that triggers SW slowdown
    bool   valid = false;
};

// ---------------------------------------------------------------------------
// Throttle bitmask helpers.
//
// NVML clock-throttle-reason bits (nvmlClocksThrottleReasons_t, values taken from
// the public NVML header / NVML API reference and verified with
// nvmlDeviceGetCurrentClocksThrottleReasons on the target GPU):
//   0x001 gpu_idle                      0x002 applications_clocks_setting
//   0x004 sw_power_cap                  0x008 hw_slowdown
//   0x010 sync_boost                    0x020 sw_thermal_slowdown
//   0x040 hw_thermal_slowdown           0x080 hw_power_brake_slowdown
//   0x100 display_clock_setting         0x200 board_limit
//   0x400 reliability
// ---------------------------------------------------------------------------

// Individual reason words, in a stable order; {"none"} when nothing is throttling.
std::vector<std::string> throttle_reason_words(uint64_t throttle_reasons);

// Convenience wrapper: "sw_power_cap|gpu_idle", or "none".
std::string decode_throttle_reasons(uint64_t throttle_reasons);

// ---------------------------------------------------------------------------
// Telemetry reader (process-wide, stateless towards the caller).
// ---------------------------------------------------------------------------
class Telemetry {
public:
    // Idempotent. Resolves the backend once and caches GPU handles/names so that
    // read() is cheap enough for a tight polling loop. Returns false (and fills
    // `error`) only when no backend is usable at all.
    static bool init(std::string& error);

    // Releases the NVML library handle (if any). Safe to call without init().
    static void shutdown();

    // Number of NVIDIA GPUs visible to the active backend (0 when uninitialised).
    static size_t device_count();

    // Reads one snapshot. Returns false with `error` for an out-of-range index or
    // when the backend is not initialised; individual unsupported fields are
    // reported as documented in GpuTelemetry instead of failing the whole call.
    static bool read(unsigned index, GpuTelemetry& out, std::string& error);

    // Reads the hardware temperature thresholds. NVML backend only: the
    // nvidia-smi fallback returns false with an explanatory error.
    static bool read_thermal_limits(unsigned index, ThermalLimits& out, std::string& error);

    // "nvml", "nvidia-smi" or "none".
    static const char* backend_name();
};

} // namespace grin
