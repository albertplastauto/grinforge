// GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
// Copyright (c) 2026 albertplastauto
// SPDX-License-Identifier: MIT
//
#pragma once
//
// gpu_control.hpp - power limit / clock / fan / undervolt control for the GRIN
// Cuckatoo32 miner (Windows x64, single NVIDIA GPU).
//
// Access to NVAPI needs no SDK and no import library: nvapi64.dll is loaded with
// LoadLibraryW() and every function is obtained through its exported
// `nvapi_QueryInterface(uint32_t id)` entry point. The ids below are hard-coded and
// were verified (see gpu_control.cpp for the provenance of each one) against
//   * NVIDIA's public open-source header set (github.com/NVIDIA/nvapi:
//     nvapi.h / nvapi_interface.h / nvapi_lite_common.h), and
//   * two independent open-source NVAPI bindings (NVFC and NvAPIWrapper), and
//   * the live driver on the target machine, by resolving every id through
//     nvapi_QueryInterface on driver 617.14 (nvapi64.dll, RTX 4060 Ti).
// Ids that did not resolve (or were not corroborated) are NOT used.
//
// Because the miner must work without elevation, every mutation reports whether the
// failure was a privilege problem (`ControlResult::needs_elevation`) so the caller can
// offer a UAC prompt instead of failing silently.
//
// Power limits are always applied with `nvidia-smi -i <idx> -pl <watts>`:
//   * NVAPI's power-policy interface is percentage based, not watt based - the
//     verified live values for this GPU are min=62500, default=100000, max=100000
//     per-cent-mille (PCM), and NvAPI_GPU_ClientPowerTopologyGetStatus returns the
//     instantaneous power *usage* in PCM, not a limit;
//   * the NVAPI setter returns NVAPI_INVALID_USER_PRIVILEGE (-137) without elevation,
//     exactly like `nvidia-smi -pl` ("Insufficient Permissions", exit code 4).
// NVAPI is therefore used for what it does expose on this driver (clock offsets via
// Pstates20, fan/cooler settings, PCM cross-checks) and nvidia-smi for watt-based
// limits and clock locks.
//
#include <cstdint>
#include <string>

namespace grin {

struct PowerLimitRange { uint32_t min_mw = 0, max_mw = 0, default_mw = 0; bool valid = false; };

struct UndervoltPlan {
    int core_clock_offset_mhz = 0;   // e.g. +150
    int voltage_offset_uv = 0;       // e.g. -50000 (microvolts)
    bool lock_core_clock = false;
    uint32_t core_clock_min_mhz = 0, core_clock_max_mhz = 0;
};

struct ControlResult { bool ok = false; std::string detail; bool needs_elevation = false; };

class GpuControl {
public:
    // Resolves the control backend for one device. NVAPI is preferred; if nvapi64.dll
    // is unusable the nvidia-smi command line backend is selected instead. Returns
    // false (with `error`) only when neither backend can drive the device.
    static bool init(unsigned device_index, std::string& error);
    static void shutdown();

    // "nvapi" - NVAPI resolved: clock/voltage offsets, fan/cooler, PCM cross-checks
    //           (power limits and clock locks still go through nvidia-smi).
    // "nvidia-smi" - command line only (power limit, -lgc/-lmc clock locks).
    // "none"  - not initialised.
    static const char* backend_name();

    // Reads the allowed power-limit window in milliwatts (min/max/default).
    static bool query_power_limit_range(PowerLimitRange& out, std::string& error);

    // Applies a power limit in whole watts. The request is clamped into the reported
    // [min, max] window - it is never applied below the minimum or above the maximum -
    // and the clamp is reported in `detail`. Idempotent: a no-op request does not call
    // nvidia-smi. Requires elevation on this driver.
    static ControlResult set_power_limit_watts(uint32_t watts);

    // Applies a core clock offset and a voltage offset through
    // NvAPI_GPU_SetPstates20 and, when requested, locks the core clock through
    // `nvidia-smi -lgc`. Idempotent. The result is verified by re-reading the GPU and
    // by a mandatory stability guard (see the implementation); on any inconsistency the
    // previous state is restored and the call fails.
    static ControlResult set_undervolt(const UndervoltPlan& plan);

    // Reverts offsets and clock locks to the state captured before the first successful
    // set_undervolt() (or to the driver defaults if nothing was applied).
    static ControlResult reset_undervolt();

    // Fan speed in percent. 0 means "automatic" (the driver takes control back) and is
    // NEVER treated as "off". NVAPI only; reported as unsupported elsewhere.
    static ControlResult set_fan_percent(uint32_t percent);

    // Best effort restore of the driver defaults: default power limit, no clock locks,
    // zeroed offsets, automatic fan.
    static ControlResult restore_defaults();
};

} // namespace grin
