// GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
// Copyright (c) 2026 albertplastauto
// SPDX-License-Identifier: MIT
//
//
// gpu_control.cpp - NVAPI (no SDK, dynamically resolved) and nvidia-smi control path.
//
// ---------------------------------------------------------------------------
// NVAPI interface ids used below. Each id is documented with its provenance:
//   [O] present in NVIDIA's public header set (github.com/NVIDIA/nvapi):
//       nvapi_interface.h (name -> id table) and/or nvapi_lite_common.h, and/or the
//       "MAKE_NVAPI_VERSION(type,ver) == sizeof(type) | (ver << 16)" definition.
//   [B] corroborated by two independent open-source NVAPI bindings (NVFC, NvAPIWrapper).
//   [L] confirmed live on the target machine (driver 617.14, nvapi64.dll) by resolving
//       the id through nvapi_QueryInterface: non-NULL pointer means the driver knows it.
//
//   id            function                                  provenance
//   0x0150E828    NvAPI_Initialize                          [O][L]
//   0xD22BDD7E    NvAPI_Unload                              [O][L]
//   0xE5AC921F    NvAPI_EnumPhysicalGPUs                    [O][L]
//   0xCEEE8E9F    NvAPI_GPU_GetFullName                     [O][L]
//   0x6FF81213    NvAPI_GPU_GetPstates20                    [O][L]
//   0x0F4DAE6B    NvAPI_GPU_SetPstates20                    [B][L]
//   0xDA141340    NvAPI_GPU_GetCoolerSettings               [B][L]
//   0x891FA0AE    NvAPI_GPU_SetCoolerLevels                 [B][L]
//   0x34206D86    NvAPI_GPU_ClientPowerPoliciesGetInfo      [B][L]
//   0x70916171    NvAPI_GPU_ClientPowerPoliciesGetStatus    [B][L]
//   0xAD95F5ED    NvAPI_GPU_ClientPowerPoliciesSetStatus    [B][L]
//   0xEDCF624E    NvAPI_GPU_ClientPowerTopologyGetStatus    [B][L]
//
// Ids that were requested by the module specification but are WRONG (they do not
// resolve on the live driver and appear in no public header), and are therefore not
// used anywhere in this file:
//   0x0FED1D25  claimed "NvAPI_GPU_SetPstates20"   -> resolves to NULL; the real id is
//               0x0F4DAE6B, which does resolve.
//   0x0EF02C24  claimed "NvAPI_GPU_ClientPowerPoliciesGetStatus" -> resolves to NULL;
//               the real id is 0x70916171 (NvAPIWrapper calls it
//               ClientPowerPoliciesGetStatus, NVFC calls it GetPowerPoliciesStatus).
//   0x0EDCF624  claimed "NvAPI_GPU_ClientPowerTopologyGetStatus" -> resolves to NULL;
//               the real id is 0xEDCF624E.
//
// ---------------------------------------------------------------------------
// Verified live behaviour on the target machine (RTX 4060 Ti, driver 617.14):
//   * NvAPI_GPU_GetPstates20: NV_GPU_PERF_PSTATES20_INFO_V1/V2/V3 are all accepted
//     (VER3 -> sizeof 7416 | 3<<16). P0 exposes 2-3 clock entries: domain 0
//     (GRAPHICS, type RANGE, delta range +-1000 MHz) and domain 4 (MEMORY), and
//     numBaseVoltages == 0 with no OV entries -> there is NO voltage entry to write on
//     this SKU. nvidia-smi -pl is used for power limits for the reasons in the header.
//   * NvAPI_GPU_SetPstates20: returns NVAPI_NOT_SUPPORTED (-104) on this driver/SKU even
//     for a write-back of an unmodified snapshot, i.e. NVAPI clock/voltage offsets are
//     blocked on this consumer GeForce. That is reported verbatim instead of pretending
//     success; the clock-lock fallback (nvidia-smi -lgc) remains available.
//   * NvAPI_GPU_GetCoolerSettings / SetCoolerLevels: return NVAPI_NOT_SUPPORTED (-104),
//     as expected for consumer GeForce fan control.
//   * NvAPI_GPU_ClientPowerPoliciesGetInfo/GetStatus: succeed without elevation and
//     report per-cent-mille values (min 62500, default 100000, max 100000 -> the 100 W
//     minimum and 160 W maximum of this GPU). GetStatus needs no elevation, but
//     SetStatus returns NVAPI_INVALID_USER_PRIVILEGE (-137), exactly like `nvidia-smi
//     -pl` ("Insufficient Permissions", exit code 4).
//   * NvAPI_GPU_ClientPowerTopologyGetStatus returns instantaneous power *usage* in PCM
//     per domain, not a limit, so it is not used to derive limits.
//
// Safety rules implemented here:
//   * a power limit is never applied below the reported minimum nor above the maximum;
//     the request is clamped and the clamp is reported;
//   * fan value 0 means "automatic", never "off";
//   * every mutation is logged with before/after values (stderr + OutputDebugStringA)
//     and mirrored into ControlResult::detail;
//   * set_undervolt() verifies the applied state by re-reading the GPU, and always runs
//     a stability guard; if the guard fails (or the verification mismatches) the
//     previous state is restored and the call fails.
//
#include "gpu_control.hpp"

#include "telemetry.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace grin {
namespace {

// ---------------------------------------------------------------------------
// NVAPI ids (see the provenance table at the top of this file).
// ---------------------------------------------------------------------------
constexpr uint32_t kNvapiInitialize = 0x0150E828u;
constexpr uint32_t kNvapiUnload = 0xD22BDD7Eu;
constexpr uint32_t kNvapiEnumPhysicalGPUs = 0xE5AC921Fu;
constexpr uint32_t kNvapiGpuGetFullName = 0xCEEE8E9Fu;
constexpr uint32_t kNvapiGpuGetPstates20 = 0x6FF81213u;
constexpr uint32_t kNvapiGpuSetPstates20 = 0x0F4DAE6Bu;
constexpr uint32_t kNvapiGpuGetCoolerSettings = 0xDA141340u;
constexpr uint32_t kNvapiGpuSetCoolerLevels = 0x891FA0AEu;
constexpr uint32_t kNvapiGpuClientPowerPoliciesGetInfo = 0x34206D86u;
constexpr uint32_t kNvapiGpuClientPowerPoliciesGetStatus = 0x70916171u;
constexpr uint32_t kNvapiGpuClientPowerPoliciesSetStatus = 0xAD95F5EDu;
constexpr uint32_t kNvapiGpuClientPowerTopologyGetStatus = 0xEDCF624Eu;

// NvAPI_Status values, from NVIDIA's public "NvAPI Status Values" reference.
constexpr int kNvapiOk = 0;
constexpr int kNvapiError = -1;
constexpr int kNvapiNoImplementation = -3;
constexpr int kNvapiApiNotInitialized = -4;
constexpr int kNvapiInvalidArgument = -5;
constexpr int kNvapiIncompatibleStructVersion = -9;
constexpr int kNvapiHandleInvalidated = -10;
constexpr int kNvapiNotSupported = -104;
constexpr int kNvapiInvalidUserPrivilege = -137;
constexpr int kNvapiSetNotAllowed = -158;
constexpr int kNvapiAccessDenied = -175;
constexpr int kNvapiGpuInDebugMode = -217;

// Internal sentinel statuses, never returned by the driver: they let the helpers below
// report "the GPU does not expose this field" distinctly from a driver status code.
constexpr int kNvapiLocalNoClockEntry = 1000;
constexpr int kNvapiLocalNoVoltageEntry = 1001;

// ---------------------------------------------------------------------------
// NV_GPU_PERF_PSTATES20_INFO (public nvapi.h layout, verified live: the driver
// accepted sizeof(V1)=7316 with version 1 and sizeof(V2)=7416 with versions 2 and 3).
// All members are 32-bit, so the default MSVC packing matches the header layout; the
// static_asserts below pin the sizes down.
// ---------------------------------------------------------------------------
constexpr uint32_t kMaxPstates20Pstates = 16;
constexpr uint32_t kMaxPstates20Clocks = 8;
constexpr uint32_t kMaxPstates20BaseVoltages = 4;

struct NvapiPstates20Delta {
    int32_t value;      // current delta from nominal, in kHz (clock) or uV (voltage)
    int32_t value_min;  // allowed delta range, read from the GPU and preserved on write
    int32_t value_max;
};

struct NvapiPstates20ClockRange {
    uint32_t min_freq_khz;
    uint32_t max_freq_khz;
    uint32_t voltage_domain_id;
    uint32_t min_voltage_uv;
    uint32_t max_voltage_uv;
};

union NvapiPstates20ClockData {
    uint32_t                single_freq_khz;  // when type_id == SINGLE
    NvapiPstates20ClockRange range;           // when type_id == RANGE
};

struct NvapiPstates20ClockEntry {
    uint32_t               domain_id;  // NV_GPU_PUBLIC_CLOCK_ID
    uint32_t               type_id;    // NV_GPU_PERF_PSTATE20_CLOCK_TYPE_ID
    uint32_t               editable;   // bIsEditable:1 | reserved:31
    NvapiPstates20Delta    freq_delta_khz;
    NvapiPstates20ClockData data;
};

struct NvapiPstates20BaseVoltageEntry {
    uint32_t            domain_id;  // NV_GPU_PERF_VOLTAGE_INFO_DOMAIN_ID
    uint32_t            editable;
    uint32_t            volt_uv;
    NvapiPstates20Delta volt_delta_uv;
};

struct NvapiPstate20 {
    uint32_t                        pstate_id;  // NV_GPU_PERF_PSTATE_ID (0 == P0)
    uint32_t                        editable;
    NvapiPstates20ClockEntry        clocks[kMaxPstates20Clocks];
    NvapiPstates20BaseVoltageEntry  base_voltages[kMaxPstates20BaseVoltages];
};

struct NvapiPstates20OverVoltage {
    uint32_t                       num_voltages;
    NvapiPstates20BaseVoltageEntry voltages[kMaxPstates20BaseVoltages];
};

struct NvapiPstates20InfoV2 {
    uint32_t                   version;
    uint32_t                   flags;  // bit 0 == bIsEditable
    uint32_t                   num_pstates;
    uint32_t                   num_clocks;
    uint32_t                   num_base_voltages;
    NvapiPstate20              pstates[kMaxPstates20Pstates];
    NvapiPstates20OverVoltage  ov;
};

static_assert(sizeof(NvapiPstates20Delta) == 12, "NV_GPU_PERF_PSTATES20_PARAM_DELTA is 12 bytes");
static_assert(sizeof(NvapiPstates20ClockRange) == 20, "clock range is 20 bytes");
static_assert(sizeof(NvapiPstates20ClockEntry) == 44, "NV_GPU_PSTATE20_CLOCK_ENTRY_V1 is 44 bytes");
static_assert(sizeof(NvapiPstates20BaseVoltageEntry) == 24, "base voltage entry is 24 bytes");
static_assert(sizeof(NvapiPstate20) == 456, "pstate entry is 456 bytes");
static_assert(sizeof(NvapiPstates20InfoV2) == 7416, "NV_GPU_PERF_PSTATES20_INFO_V2 is 7416 bytes");

// NV_GPU_PERF_PSTATES20_INFO_VER3 == MAKE_NVAPI_VERSION(...,3) == sizeof | (3 << 16)
constexpr uint32_t kNvapiPstates20Version3 = static_cast<uint32_t>(sizeof(NvapiPstates20InfoV2)) | (3u << 16);
constexpr uint32_t kNvapiPstates20FlagEditable = 0x00000001u;

// NV_GPU_PUBLIC_CLOCK_ID (public nvapi.h)
constexpr uint32_t kNvapiClockDomainGraphics = 0;
constexpr uint32_t kNvapiClockDomainMemory = 4;
// NV_GPU_PERF_VOLTAGE_INFO_DOMAIN_ID (public nvapi.h)
constexpr uint32_t kNvapiVoltageDomainCore = 0;
// NV_GPU_PERF_PSTATE_ID P0
constexpr uint32_t kNvapiPstateP0 = 0;

// ---------------------------------------------------------------------------
// NV_GPU_COOLER_SETTINGS / NV_GPU_COOLER_LEVELS. Field order and the 3-cooler array
// size (NVAPI_MAX_COOLERS_PER_GPU == 3) are corroborated by the two independent
// bindings; because the driver validates the version field (which encodes the struct
// size) before touching memory, a hypothetical layout mismatch can only produce
// NVAPI_INCOMPATIBLE_STRUCT_VERSION and never a partially written structure.
// ---------------------------------------------------------------------------
constexpr uint32_t kMaxCoolersPerGpu = 3;

struct NvapiCoolerSetting {
    int32_t  type;
    int32_t  controller;
    uint32_t default_min;
    uint32_t default_max;
    uint32_t current_min;
    uint32_t current_max;
    uint32_t current_level;
    uint32_t default_policy;
    uint32_t current_policy;
    uint32_t target;
    uint32_t control_type;
    uint32_t active;
};

struct NvapiCoolerSettingsV1 {
    uint32_t           version;
    uint32_t           count;
    NvapiCoolerSetting coolers[kMaxCoolersPerGpu];
};

struct NvapiCoolerLevel {
    uint32_t level;   // percent
    uint32_t policy;  // NVAPI_COOLER_POLICY_*
};

struct NvapiCoolerLevelsV1 {
    uint32_t          version;
    NvapiCoolerLevel  levels[kMaxCoolersPerGpu];
};

static_assert(sizeof(NvapiCoolerSetting) == 48, "cooler setting is 48 bytes");
static_assert(sizeof(NvapiCoolerSettingsV1) == 152, "NV_GPU_COOLER_SETTINGS (3 coolers) is 152 bytes");
static_assert(sizeof(NvapiCoolerLevelsV1) == 28, "NV_GPU_COOLER_LEVELS (3 coolers) is 28 bytes");

constexpr uint32_t kNvapiCoolerSettingsVersion = static_cast<uint32_t>(sizeof(NvapiCoolerSettingsV1)) | (1u << 16);
constexpr uint32_t kNvapiCoolerLevelsVersion = static_cast<uint32_t>(sizeof(NvapiCoolerLevelsV1)) | (1u << 16);
// NVAPI_COOLER_POLICY_MANUAL (user supplied level) and NVAPI_COOLER_POLICY_DEFAULT
// (return control to the driver) as documented by the open-source bindings.
constexpr uint32_t kNvapiCoolerPolicyDefault = 0x00000020u;
constexpr uint32_t kNvapiCoolerPolicyManual = 0x00000001u;

// ---------------------------------------------------------------------------
// NV_GPU_POWER_POLICIES_INFO / STATUS (per-cent-mille ratios). Used read-only, both to
// cross-check the watt-based limit and to derive a limit window when nvidia-smi is not
// available. The 2 reserved bytes keep `entries` 4-byte aligned exactly like the
// driver's ABI (verified live: sizeof 184 with count 1 and 62500/100000/100000).
// ---------------------------------------------------------------------------
constexpr uint32_t kMaxPowerPolicies = 4;

struct NvapiPowerPolicyInfoEntry {
    uint32_t pstate;
    uint32_t reserved0;
    uint32_t reserved1;
    uint32_t min_power_pcm;   // per cent mille
    uint32_t reserved2;
    uint32_t reserved3;
    uint32_t default_power_pcm;
    uint32_t reserved4;
    uint32_t reserved5;
    uint32_t max_power_pcm;
    uint32_t reserved6;
};

struct NvapiPowerPoliciesInfo {
    uint32_t                     version;  // 1 << 16 | sizeof
    uint8_t                      valid;
    uint8_t                      count;
    uint8_t                      reserved[2];
    NvapiPowerPolicyInfoEntry    entries[kMaxPowerPolicies];
};

struct NvapiPowerPolicyStatusEntry {
    uint32_t pstate;
    uint32_t reserved0;
    uint32_t power_pcm;
    uint32_t reserved1;
};

struct NvapiPowerPoliciesStatus {
    uint32_t                       version;
    uint32_t                       count;
    NvapiPowerPolicyStatusEntry    entries[kMaxPowerPolicies];
};

static_assert(sizeof(NvapiPowerPoliciesInfo) == 184, "NV_GPU_POWER_POLICIES_INFO is 184 bytes");
static_assert(sizeof(NvapiPowerPoliciesStatus) == 72, "NV_GPU_POWER_POLICIES_STATUS is 72 bytes");

constexpr uint32_t kNvapiPowerPoliciesInfoVersion = static_cast<uint32_t>(sizeof(NvapiPowerPoliciesInfo)) | (1u << 16);
constexpr uint32_t kNvapiPowerPoliciesStatusVersion = static_cast<uint32_t>(sizeof(NvapiPowerPoliciesStatus)) | (1u << 16);

// ---------------------------------------------------------------------------
// Safety envelope for the undervolt helper (documented magic numbers).
//   * core clock offset: -1000 MHz (the delta range the GPU itself reports for the
//     GRAPHICS domain on this SKU) .. +250 MHz. A positive offset above +250 MHz is
//     outside what an Ada 8 GB part can sustain with the stock power limit.
//   * voltage offset: -125000 uV (-125 mV) .. 0. Only undervolting is allowed; asking
//     for an over-voltage is refused rather than silently clamped.
//   * core clock lock window: 300 MHz .. 3200 MHz, min <= max. The 4060 Ti boosts to
//     ~2.5 GHz and Pstates20 reports a 3105 MHz ceiling, so 3200 MHz is a safe cap.
// ---------------------------------------------------------------------------
constexpr int kMinCoreClockOffsetMhz = -1000;
constexpr int kMaxCoreClockOffsetMhz = 250;
constexpr int kMinVoltageOffsetUv = -125000;
constexpr int kMaxVoltageOffsetUv = 0;
constexpr uint32_t kMinCoreClockLockMhz = 300;
constexpr uint32_t kMaxCoreClockLockMhz = 3200;

// Settle time before the mandatory post-mutation verification / stability guard.
constexpr DWORD kSettleMs = 300;

// Fallback stability guard temperature when the device does not report a slowdown
// threshold. The device threshold (97 C on the target GPU) minus 5 C is preferred.
constexpr double kFallbackThermalGuardC = 85.0;

// Throttle-reason bits used by the stability guard (see telemetry.hpp).
constexpr uint64_t kThrottleHwThermalSlowdown = 0x040ULL;
constexpr uint64_t kThrottleHwPowerBrakeSlowdown = 0x080ULL;

// Timeout for one nvidia-smi invocation.
constexpr DWORD kCommandTimeoutMs = 5000;

// ---------------------------------------------------------------------------
// NVAPI function table. Signatures follow the public headers; __cdecl matches
// NVAPI_INTERFACE on Windows.
// ---------------------------------------------------------------------------
using NvapiQueryInterfaceFn = void*(__cdecl*)(uint32_t);
using NvapiInitializeFn = int(__cdecl*)(void);
using NvapiUnloadFn = int(__cdecl*)(void);
using NvapiEnumPhysicalGpusFn = int(__cdecl*)(void** handles, int* count);
using NvapiGpuGetFullNameFn = int(__cdecl*)(void* gpu, char* name);
using NvapiGpuGetPstates20Fn = int(__cdecl*)(void* gpu, NvapiPstates20InfoV2* info);
using NvapiGpuSetPstates20Fn = int(__cdecl*)(void* gpu, NvapiPstates20InfoV2* info);
using NvapiGpuGetCoolerSettingsFn = int(__cdecl*)(void* gpu, int cooler_index, NvapiCoolerSettingsV1* settings);
using NvapiGpuSetCoolerLevelsFn = int(__cdecl*)(void* gpu, int cooler_index, NvapiCoolerLevelsV1* levels);
using NvapiGpuPowerPoliciesGetInfoFn = int(__cdecl*)(void* gpu, NvapiPowerPoliciesInfo* info);
using NvapiGpuPowerPoliciesGetStatusFn = int(__cdecl*)(void* gpu, NvapiPowerPoliciesStatus* status);
using NvapiGpuPowerPoliciesSetStatusFn = int(__cdecl*)(void* gpu, NvapiPowerPoliciesStatus* status);
using NvapiGpuPowerTopologyGetStatusFn = int(__cdecl*)(void* gpu, void* status);

struct NvapiApi {
    HMODULE module = nullptr;
    NvapiQueryInterfaceFn query_interface = nullptr;
    NvapiInitializeFn initialize = nullptr;
    NvapiUnloadFn unload = nullptr;
    NvapiEnumPhysicalGpusFn enum_physical_gpus = nullptr;
    NvapiGpuGetFullNameFn gpu_get_full_name = nullptr;
    NvapiGpuGetPstates20Fn gpu_get_pstates20 = nullptr;
    NvapiGpuSetPstates20Fn gpu_set_pstates20 = nullptr;
    NvapiGpuGetCoolerSettingsFn gpu_get_cooler_settings = nullptr;
    NvapiGpuSetCoolerLevelsFn gpu_set_cooler_levels = nullptr;
    NvapiGpuPowerPoliciesGetInfoFn power_policies_get_info = nullptr;
    NvapiGpuPowerPoliciesGetStatusFn power_policies_get_status = nullptr;
    NvapiGpuPowerPoliciesSetStatusFn power_policies_set_status = nullptr;
    NvapiGpuPowerTopologyGetStatusFn power_topology_get_status = nullptr;

    bool usable() const {
        return module != nullptr && query_interface != nullptr && initialize != nullptr && unload != nullptr &&
               enum_physical_gpus != nullptr;
    }
};

enum class Backend { None, Nvapi, NvidiaSmi };

// ---------------------------------------------------------------------------
// Process-wide state: the resolved NVAPI function table, the cached device handle and
// the bookkeeping needed for idempotent/reversible mutations.
// ---------------------------------------------------------------------------
struct ControlState {
    std::mutex mutex;

    bool        initialised = false;
    bool        ready = false;
    Backend     backend = Backend::None;
    unsigned    device_index = 0;
    std::wstring smi_path;

    NvapiApi    nvapi;
    void*       gpu_handle = nullptr;
    std::string gpu_name;

    // Undervolt bookkeeping: the values captured before the first mutation we made,
    // so reset_undervolt() can restore exactly that state.
    bool have_undervolt_record = false;
    int  original_core_delta_khz = 0;
    int  original_voltage_delta_uv = 0;
    int  last_applied_core_delta_khz = 0;
    int  last_applied_voltage_delta_uv = 0;

    // Clock-lock bookkeeping (nvidia-smi -lgc).
    bool     have_lock_record = false;
    uint32_t lock_min_mhz = 0;
    uint32_t lock_max_mhz = 0;

    // Cached power-limit window.
    bool            have_power_range = false;
    PowerLimitRange power_range;
};

ControlState g_control;

// ---------------------------------------------------------------------------
// Small helpers.
// ---------------------------------------------------------------------------
std::string trim(std::string text) {
    const auto not_space = [](unsigned char ch) { return std::isspace(ch) == 0; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

std::vector<std::string> split(const std::string& text, char delimiter) {
    std::vector<std::string> parts;
    std::string current;
    for (char ch : text) {
        if (ch == delimiter) {
            parts.push_back(trim(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    parts.push_back(trim(current));
    return parts;
}

// nvidia-smi prints unsupported values as "N/A" or "[N/A]".
bool parse_number(const std::string& token, double& out) {
    std::string value = trim(token);
    if (value.empty()) {
        return false;
    }
    if (value.front() == '[' && value.back() == ']') {
        value = trim(value.substr(1, value.size() - 2));
    }
    if (value.empty() || value == "N/A" || value == "n/a") {
        return false;
    }
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (end == value.c_str() || (end != nullptr && *end != '\0')) {
        return false;
    }
    out = parsed;
    return true;
}

std::wstring widen_ascii(const std::string& text) {
    std::wstring wide;
    wide.reserve(text.size());
    for (char ch : text) {
        wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
    }
    return wide;
}

std::string narrow_ascii(const std::wstring& text) {
    std::string narrow;
    narrow.reserve(text.size());
    for (wchar_t ch : text) {
        const unsigned int code = static_cast<unsigned int>(ch);
        narrow.push_back(code < 128u ? static_cast<char>(code) : '?');
    }
    return narrow;
}

std::string lowercase(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string system_error_text(DWORD code) {
    LPWSTR buffer = nullptr;
    // English on purpose: LANG_NEUTRAL returns the OS display language, which would leak Russian
    // (or any other locale) into output that is meant to be English everywhere. The numeric code
    // is already the first half of the string, so it stays useful if FormatMessage fails.
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string text = "win32 error " + std::to_string(code);
    if (length != 0 && buffer != nullptr) {
        std::wstring wide(buffer, length);
        LocalFree(buffer);
        while (wide.empty() == false && (wide.back() == L'\r' || wide.back() == L'\n' || wide.back() == L' ')) {
            wide.pop_back();
        }
        const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                               nullptr, 0, nullptr, nullptr);
        if (needed > 0) {
            std::string narrow(static_cast<size_t>(needed), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                narrow.data(), needed, nullptr, nullptr);
            text += " (";
            text += narrow;
            text += ")";
        }
    }
    return text;
}

// Recognises the privilege-related failure text/status so the caller can offer a UAC
// prompt. `nvidia-smi -pl` without elevation prints
// "Failed to set power management limit for GPU ...: Insufficient Permissions"
// and exits with code 4 on the verified driver.
bool looks_like_permission_problem(const std::string& output, DWORD exit_code) {
    const std::string text = lowercase(output);
    static const char* kNeedles[] = {
        // `nvidia-smi -pl` without elevation:
        //   "Failed to set power management limit for GPU ...: Insufficient Permissions"
        "insufficient permission",
        // `nvidia-smi -lgc` without elevation:
        //   "The current user does not have permission to change clocks for GPU ..."
        "permission",
        "not permitted",
        "access denied",
        "requires administrator",
        "administrator privileges",
        "run as administrator",
        "elevat",
        "privilege",
    };
    for (const char* needle : kNeedles) {
        if (text.find(needle) != std::string::npos) {
            return true;
        }
    }
    // Exit code 4 is what nvidia-smi returns for these permission failures on the
    // verified driver; treat it as such when the process produced no other explanation.
    return exit_code == 4 && trim(output).empty();
}

std::string nvapi_status_name(int status) {
    switch (status) {
        case kNvapiOk:
            return "NVAPI_OK";
        case kNvapiError:
            return "NVAPI_ERROR";
        case kNvapiNoImplementation:
            return "NVAPI_NO_IMPLEMENTATION";
        case kNvapiApiNotInitialized:
            return "NVAPI_API_NOT_INITIALIZED";
        case kNvapiInvalidArgument:
            return "NVAPI_INVALID_ARGUMENT";
        case kNvapiIncompatibleStructVersion:
            return "NVAPI_INCOMPATIBLE_STRUCT_VERSION";
        case kNvapiHandleInvalidated:
            return "NVAPI_HANDLE_INVALIDATED";
        case kNvapiNotSupported:
            return "NVAPI_NOT_SUPPORTED";
        case kNvapiInvalidUserPrivilege:
            return "NVAPI_INVALID_USER_PRIVILEGE";
        case kNvapiSetNotAllowed:
            return "NVAPI_SET_NOT_ALLOWED";
        case kNvapiAccessDenied:
            return "NVAPI_ACCESS_DENIED";
        case kNvapiGpuInDebugMode:
            return "NVAPI_GPU_IN_DEBUG_MODE";
        case kNvapiLocalNoClockEntry:
            return "P0 GRAPHICS clock entry missing from NV_GPU_PERF_PSTATES20_INFO";
        case kNvapiLocalNoVoltageEntry:
            return "this SKU exposes no P0 CORE base-voltage entry in NV_GPU_PERF_PSTATES20_INFO";
        default:
            return "NVAPI status " + std::to_string(status);
    }
}

bool nvapi_needs_elevation(int status) {
    return status == kNvapiInvalidUserPrivilege || status == kNvapiAccessDenied;
}

struct CommandResult {
    bool        launched = false;
    DWORD       exit_code = 0;
    std::string output;
    std::string error;
};

// Runs a command line with a hard timeout, capturing merged stdout+stderr. This is a
// deliberately local copy of the helper used in telemetry.cpp: the module is defined by
// exactly four files, so there is no shared internal header to put it in.
CommandResult run_command(const std::wstring& command_line, DWORD timeout_ms) {
    CommandResult result;

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
    attributes.bInheritHandle = TRUE;
    attributes.lpSecurityDescriptor = nullptr;

    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (CreatePipe(&read_end, &write_end, &attributes, 0) == FALSE) {
        result.error = "CreatePipe failed: " + system_error_text(GetLastError());
        return result;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul_device = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &attributes, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(STARTUPINFOW);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nul_device;
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;

    PROCESS_INFORMATION process{};
    std::wstring mutable_command = command_line;
    const BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    const DWORD create_error = GetLastError();

    CloseHandle(write_end);
    if (nul_device != nullptr && nul_device != INVALID_HANDLE_VALUE) {
        CloseHandle(nul_device);
    }
    if (created == FALSE) {
        CloseHandle(read_end);
        result.error = "CreateProcessW(\"" + narrow_ascii(command_line) + "\") failed: " +
                       system_error_text(create_error);
        return result;
    }
    result.launched = true;

    // Hard cap on captured output (nvidia-smi output is tiny; this only guards against
    // a pathological child).
    constexpr size_t kMaxCapturedBytes = 64 * 1024;

    const auto drain = [&read_end, &result]() {
        for (;;) {
            DWORD available = 0;
            if (PeekNamedPipe(read_end, nullptr, 0, nullptr, &available, nullptr) == FALSE) {
                return;  // writer closed the pipe
            }
            if (available == 0) {
                return;
            }
            char buffer[4096];
            const DWORD wanted = (available < sizeof(buffer)) ? available : static_cast<DWORD>(sizeof(buffer));
            DWORD received = 0;
            if (ReadFile(read_end, buffer, wanted, &received, nullptr) == FALSE || received == 0) {
                return;
            }
            if (result.output.size() < kMaxCapturedBytes) {
                result.output.append(buffer, received);
            }
        }
    };

    bool timed_out = false;
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
    for (;;) {
        drain();
        const DWORD wait = WaitForSingleObject(process.hProcess, 20);
        if (wait == WAIT_OBJECT_0) {
            // Collect whatever the child wrote just before exiting; without this final
            // drain a fast process (nvidia-smi on error) can lose its whole message.
            drain();
            break;
        }
        if (GetTickCount64() >= deadline) {
            timed_out = true;
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 1000);
            break;
        }
    }

    if (timed_out == false) {
        DWORD code = 0;
        if (GetExitCodeProcess(process.hProcess, &code) != FALSE) {
            result.exit_code = code;
        }
    } else {
        result.error = "command timed out after " + std::to_string(timeout_ms) + " ms";
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(read_end);
    result.output = trim(result.output);
    return result;
}

CommandResult run_nvidia_smi(const std::string& arguments) {
    std::wstring command = L"\"" + g_control.smi_path + L"\" ";
    command += widen_ascii(arguments);
    return run_command(command, kCommandTimeoutMs);
}

// Every mutation is logged with before/after values: to the debugger, to stderr (the
// miner's console) and into ControlResult::detail for the caller.
void log_mutation(const char* operation, const std::string& before, const std::string& after,
                  const ControlResult& result) {
    std::string line = "[gpu_control] ";
    line += operation;
    line += ": before={";
    line += before;
    line += "} after={";
    line += after;
    line += "} ok=";
    line += result.ok ? "true" : "false";
    line += " needs_elevation=";
    line += result.needs_elevation ? "true" : "false";
    line += " detail=";
    line += result.detail;
    line += "\n";
    OutputDebugStringA(line.c_str());
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
}

uint32_t watts_to_mw(double watts) {
    if (watts <= 0.0) {
        return 0;
    }
    return static_cast<uint32_t>(watts * 1000.0 + 0.5);
}

// ---------------------------------------------------------------------------
// NVAPI helpers (all of them assume g_control.mutex is held).
// ---------------------------------------------------------------------------
bool read_pstates20_locked(NvapiPstates20InfoV2& info, int& status) {
    if (g_control.nvapi.gpu_get_pstates20 == nullptr || g_control.gpu_handle == nullptr) {
        status = kNvapiNoImplementation;
        return false;
    }
    info = NvapiPstates20InfoV2{};
    info.version = kNvapiPstates20Version3;
    status = g_control.nvapi.gpu_get_pstates20(g_control.gpu_handle, &info);
    return status == kNvapiOk;
}

// Finds the P0 GRAPHICS clock entry and, when present, the P0 CORE base-voltage entry.
NvapiPstates20ClockEntry* find_p0_core_clock(NvapiPstates20InfoV2& info) {
    for (uint32_t i = 0; i < info.num_pstates && i < kMaxPstates20Pstates; ++i) {
        if (info.pstates[i].pstate_id != kNvapiPstateP0) {
            continue;
        }
        for (uint32_t c = 0; c < info.num_clocks && c < kMaxPstates20Clocks; ++c) {
            if (info.pstates[i].clocks[c].domain_id == kNvapiClockDomainGraphics) {
                return &info.pstates[i].clocks[c];
            }
        }
        return nullptr;
    }
    return nullptr;
}

NvapiPstates20BaseVoltageEntry* find_p0_core_voltage(NvapiPstates20InfoV2& info) {
    for (uint32_t i = 0; i < info.num_pstates && i < kMaxPstates20Pstates; ++i) {
        if (info.pstates[i].pstate_id != kNvapiPstateP0) {
            continue;
        }
        for (uint32_t v = 0; v < info.num_base_voltages && v < kMaxPstates20BaseVoltages; ++v) {
            if (info.pstates[i].base_voltages[v].domain_id == kNvapiVoltageDomainCore) {
                return &info.pstates[i].base_voltages[v];
            }
        }
        return nullptr;
    }
    return nullptr;
}

// Writes a P0 clock/voltage offset pair back through NvAPI_GPU_SetPstates20. The struct
// is always re-read first so that all untouched fields (including the allowed delta
// ranges) are preserved exactly.
int write_p0_offsets_locked(int core_delta_khz, int voltage_delta_uv, bool touch_voltage) {
    NvapiPstates20InfoV2 info{};
    int status = kNvapiOk;
    if (read_pstates20_locked(info, status) == false) {
        return status;
    }
    NvapiPstates20ClockEntry* clock_entry = find_p0_core_clock(info);
    if (clock_entry == nullptr) {
        return kNvapiLocalNoClockEntry;
    }
    info.flags |= kNvapiPstates20FlagEditable;
    clock_entry->editable |= kNvapiPstates20FlagEditable;
    clock_entry->freq_delta_khz.value = core_delta_khz;

    if (touch_voltage) {
        NvapiPstates20BaseVoltageEntry* voltage_entry = find_p0_core_voltage(info);
        if (voltage_entry == nullptr) {
            return kNvapiLocalNoVoltageEntry;
        }
        voltage_entry->editable |= kNvapiPstates20FlagEditable;
        voltage_entry->volt_delta_uv.value = voltage_delta_uv;
    }

    const int set_status = g_control.nvapi.gpu_set_pstates20(g_control.gpu_handle, &info);
    return set_status;
}

// Reads back the currently applied P0 offsets (kHz / uV).
bool read_p0_offsets_locked(int& core_delta_khz, int& voltage_delta_uv, bool& has_voltage_entry, std::string& error) {
    NvapiPstates20InfoV2 info{};
    int status = kNvapiOk;
    if (read_pstates20_locked(info, status) == false) {
        error = "NvAPI_GPU_GetPstates20 failed: " + nvapi_status_name(status);
        return false;
    }
    NvapiPstates20ClockEntry* clock_entry = find_p0_core_clock(info);
    if (clock_entry == nullptr) {
        error = "P0 GRAPHICS clock entry not found in NV_GPU_PERF_PSTATES20_INFO";
        return false;
    }
    core_delta_khz = clock_entry->freq_delta_khz.value;
    NvapiPstates20BaseVoltageEntry* voltage_entry = find_p0_core_voltage(info);
    has_voltage_entry = voltage_entry != nullptr;
    voltage_delta_uv = (voltage_entry != nullptr) ? voltage_entry->volt_delta_uv.value : 0;
    return true;
}

// Power-policy ratios in per cent mille; returns false when unavailable.
bool read_power_policy_ratios_locked(uint32_t& min_pcm, uint32_t& default_pcm, uint32_t& max_pcm,
                                     std::string& error) {
    if (g_control.nvapi.power_policies_get_info == nullptr || g_control.gpu_handle == nullptr) {
        error = "NvAPI_GPU_ClientPowerPoliciesGetInfo is not available";
        return false;
    }
    NvapiPowerPoliciesInfo info{};
    info.version = kNvapiPowerPoliciesInfoVersion;
    const int status = g_control.nvapi.power_policies_get_info(g_control.gpu_handle, &info);
    if (status != kNvapiOk) {
        error = "NvAPI_GPU_ClientPowerPoliciesGetInfo failed: " + nvapi_status_name(status);
        return false;
    }
    if (info.valid == 0 || info.count == 0) {
        error = "NVAPI power policy info is not valid on this GPU";
        return false;
    }
    const NvapiPowerPolicyInfoEntry& entry = info.entries[0];
    min_pcm = entry.min_power_pcm;
    default_pcm = entry.default_power_pcm;
    max_pcm = entry.max_power_pcm;
    return max_pcm > 0;
}

// Current power target as a per-cent-mille ratio, for cross-checking a watt-based limit.
bool read_power_policy_status_pcm_locked(uint32_t& pcm, std::string& error) {
    if (g_control.nvapi.power_policies_get_status == nullptr || g_control.gpu_handle == nullptr) {
        error = "NvAPI_GPU_ClientPowerPoliciesGetStatus is not available";
        return false;
    }
    NvapiPowerPoliciesStatus status{};
    status.version = kNvapiPowerPoliciesStatusVersion;
    const int result = g_control.nvapi.power_policies_get_status(g_control.gpu_handle, &status);
    if (result != kNvapiOk) {
        error = "NvAPI_GPU_ClientPowerPoliciesGetStatus failed: " + nvapi_status_name(result);
        return false;
    }
    if (status.count == 0) {
        error = "NVAPI power policy status is empty";
        return false;
    }
    pcm = status.entries[0].power_pcm;
    return true;
}

// ---------------------------------------------------------------------------
// Telemetry helpers (they use the sibling telemetry module, which owns the NVML
// backend; no extra dependency is introduced).
// ---------------------------------------------------------------------------
double thermal_guard_c() {
    ThermalLimits limits;
    std::string error;
    if (Telemetry::read_thermal_limits(g_control.device_index, limits, error) && limits.slowdown_c > 0.0) {
        return std::max(60.0, limits.slowdown_c - 5.0);
    }
    return kFallbackThermalGuardC;
}

// Mandatory stability guard: after every successful mutation the GPU state is
// re-read and validated. It refuses to leave the GPU with a power limit below the
// reported minimum, with an excessive temperature, or with hardware slowdown asserted.
bool stability_guard_locked(std::string& reason) {
    GpuTelemetry telemetry;
    std::string error;
    if (Telemetry::read(g_control.device_index, telemetry, error) == false) {
        reason = "telemetry re-read failed: " + error;
        return false;
    }
    if (telemetry.power_min_w > 0.0 && telemetry.power_limit_w > 0.0 &&
        telemetry.power_limit_w + 0.5 < telemetry.power_min_w) {
        reason = "power limit " + std::to_string(telemetry.power_limit_w) + " W is below the reported minimum " +
                 std::to_string(telemetry.power_min_w) + " W";
        return false;
    }
    const double guard_c = thermal_guard_c();
    if (telemetry.temperature_c > guard_c) {
        reason = "temperature " + std::to_string(telemetry.temperature_c) + " C exceeds the stability guard " +
                 std::to_string(guard_c) + " C";
        return false;
    }
    if ((telemetry.throttle_reasons & kThrottleHwThermalSlowdown) != 0) {
        reason = "hw_thermal_slowdown is asserted (throttle reasons 0x" +
                 std::to_string(telemetry.throttle_reasons) + ")";
        return false;
    }
    if ((telemetry.throttle_reasons & kThrottleHwPowerBrakeSlowdown) != 0) {
        reason = "hw_power_brake_slowdown is asserted (throttle reasons 0x" +
                 std::to_string(telemetry.throttle_reasons) + ")";
        return false;
    }
    return true;
}

// Reads the enforced power limit in watts, preferring nvidia-smi and falling back to
// NVML through the telemetry module. Returns false when neither source has a value.
bool read_power_limit_watts_locked(double& watts, std::string& source) {
    if (g_control.smi_path.empty() == false) {
        const CommandResult result = run_nvidia_smi("-i " + std::to_string(g_control.device_index) +
                                                    " --query-gpu=power.limit --format=csv,noheader,nounits");
        if (result.launched && result.exit_code == 0) {
            double value = 0.0;
            if (parse_number(result.output, value)) {
                watts = value;
                source = "nvidia-smi";
                return true;
            }
        }
    }
    GpuTelemetry telemetry;
    std::string error;
    if (Telemetry::read(g_control.device_index, telemetry, error) && telemetry.power_limit_w > 0.0) {
        watts = telemetry.power_limit_w;
        source = "nvml";
        return true;
    }
    source.clear();
    return false;
}

// ---------------------------------------------------------------------------
// Power limit.
// ---------------------------------------------------------------------------
bool query_power_limit_range_locked(PowerLimitRange& out, std::string& error) {
    out = PowerLimitRange{};
    if (g_control.have_power_range) {
        out = g_control.power_range;
        return true;
    }

    std::string smi_failure;
    if (g_control.smi_path.empty() == false) {
        const CommandResult result =
            run_nvidia_smi("-i " + std::to_string(g_control.device_index) +
                           " --query-gpu=power.min_limit,power.max_limit,power.default_limit "
                           "--format=csv,noheader,nounits");
        if (result.launched == false) {
            smi_failure = result.error;
        } else if (result.exit_code != 0) {
            smi_failure = "nvidia-smi exited with code " + std::to_string(result.exit_code) + ": " + result.output;
        } else {
            const std::vector<std::string> fields = split(result.output, ',');
            double min_w = 0.0;
            double max_w = 0.0;
            double default_w = 0.0;
            if (fields.size() >= 2 && parse_number(fields[1], max_w) && max_w > 0.0) {
                out.max_mw = watts_to_mw(max_w);
                out.min_mw = parse_number(fields[0], min_w) ? watts_to_mw(min_w) : 0u;
                out.default_mw = (fields.size() >= 3 && parse_number(fields[2], default_w) && default_w > 0.0)
                                     ? watts_to_mw(default_w)
                                     : out.max_mw;
                if (out.min_mw > out.max_mw) {
                    std::swap(out.min_mw, out.max_mw);
                }
                out.valid = true;
                g_control.power_range = out;
                g_control.have_power_range = true;
                return true;
            }
            smi_failure = "could not parse the power limits from \"" + result.output + "\"";
        }
    } else {
        smi_failure = "nvidia-smi is not available";
    }

    // Fallback: NVML gives the watt window; the NVAPI power-policy ratios then provide
    // the default limit (verified live: 62500/100000/100000 PCM for 100 W/160 W/160 W).
    GpuTelemetry telemetry;
    std::string telemetry_error;
    if (Telemetry::read(g_control.device_index, telemetry, telemetry_error) == false) {
        error = "power limit range unavailable - nvidia-smi: " + smi_failure +
                "; telemetry: " + telemetry_error;
        return false;
    }
    if (telemetry.power_max_w <= 0.0) {
        error = "power limit range unavailable - nvidia-smi: " + smi_failure +
                "; NVML reported no maximum power limit";
        return false;
    }
    const double reference_w = (telemetry.power_limit_w > 0.0) ? telemetry.power_limit_w : telemetry.power_max_w;
    out.max_mw = watts_to_mw(telemetry.power_max_w);
    out.min_mw = telemetry.power_min_w > 0.0 ? watts_to_mw(telemetry.power_min_w) : 0u;
    out.default_mw = out.max_mw;

    uint32_t min_pcm = 0;
    uint32_t default_pcm = 0;
    uint32_t max_pcm = 0;
    std::string pcm_error;
    if (read_power_policy_ratios_locked(min_pcm, default_pcm, max_pcm, pcm_error)) {
        if (default_pcm > 0) {
            out.default_mw = watts_to_mw(reference_w * static_cast<double>(default_pcm) / 100000.0);
        }
        if (max_pcm > 0 && max_pcm != 100000u) {
            out.max_mw = watts_to_mw(reference_w * static_cast<double>(max_pcm) / 100000.0);
        }
        if (min_pcm > 0) {
            out.min_mw = watts_to_mw(reference_w * static_cast<double>(min_pcm) / 100000.0);
        }
    }
    if (out.min_mw > out.max_mw) {
        std::swap(out.min_mw, out.max_mw);
    }
    out.valid = out.max_mw > 0;
    if (out.valid == false) {
        error = "power limit range unavailable - nvidia-smi: " + smi_failure;
        return false;
    }
    g_control.power_range = out;
    g_control.have_power_range = true;
    return true;
}

ControlResult set_power_limit_watts_locked(uint32_t watts) {
    ControlResult result;

    if (g_control.backend == Backend::None) {
        result.detail = "gpu control is not initialised";
        return result;
    }
    if (g_control.smi_path.empty()) {
        result.detail = "setting a power limit requires nvidia-smi, which was not found (NVAPI exposes only "
                        "percentage-based power policies and its setter needs elevation as well)";
        return result;
    }

    PowerLimitRange range;
    std::string error;
    if (query_power_limit_range_locked(range, error) == false || range.valid == false) {
        result.detail = "refusing to apply a power limit without a valid range: " + error;
        return result;
    }

    // Safety: clamp into the reported window. Never below min, never above max.
    uint32_t target_w = watts;
    std::string clamp_note;
    if (range.min_mw > 0 && watts_to_mw(static_cast<double>(watts)) < range.min_mw) {
        target_w = (range.min_mw + 999u) / 1000u;  // round up to stay >= min
        clamp_note = "requested " + std::to_string(watts) + " W is below the minimum " +
                     std::to_string(range.min_mw / 1000u) + " W -> clamped";
    } else if (range.max_mw > 0 && watts_to_mw(static_cast<double>(watts)) > range.max_mw) {
        target_w = range.max_mw / 1000u;  // round down to stay <= max
        clamp_note = "requested " + std::to_string(watts) + " W is above the maximum " +
                     std::to_string(range.max_mw / 1000u) + " W -> clamped";
    }

    double before_w = 0.0;
    std::string source;
    const bool have_before = read_power_limit_watts_locked(before_w, source);

    // Idempotent: do not spawn nvidia-smi when nothing would change.
    if (have_before && std::fabs(before_w - static_cast<double>(target_w)) < 0.6) {
        result.ok = true;
        result.detail = "power limit is already " + std::to_string(target_w) + " W (no change)";
        if (clamp_note.empty() == false) {
            result.detail += "; " + clamp_note;
        }
        log_mutation("set_power_limit_watts",
                     have_before ? (std::to_string(before_w) + " W") : std::string("unknown"),
                     std::to_string(target_w) + " W", result);
        return result;
    }

    const CommandResult command = run_nvidia_smi("-i " + std::to_string(g_control.device_index) + " -pl " +
                                                 std::to_string(target_w));
    result.needs_elevation = looks_like_permission_problem(command.output, command.exit_code);

    if (command.launched == false) {
        result.detail = "could not run nvidia-smi: " + command.error;
        log_mutation("set_power_limit_watts", have_before ? std::to_string(before_w) + " W" : "unknown",
                     std::to_string(target_w) + " W", result);
        return result;
    }
    if (command.exit_code != 0) {
        result.detail = "nvidia-smi -pl " + std::to_string(target_w) + " failed with exit code " +
                        std::to_string(command.exit_code) + ": " + command.output;
        if (result.needs_elevation) {
            result.detail += " (this operation needs elevation: relaunch elevated / accept the UAC prompt)";
        }
        if (clamp_note.empty() == false) {
            result.detail += "; " + clamp_note;
        }
        log_mutation("set_power_limit_watts", have_before ? std::to_string(before_w) + " W" : "unknown",
                     std::to_string(target_w) + " W", result);
        return result;
    }

    // Verify by re-reading the enforced limit.
    double after_w = 0.0;
    std::string after_source;
    const bool have_after = read_power_limit_watts_locked(after_w, after_source);
    if (have_after == false) {
        result.detail = "nvidia-smi -pl " + std::to_string(target_w) +
                        " reported success but the new limit could not be read back";
        log_mutation("set_power_limit_watts", have_before ? std::to_string(before_w) + " W" : "unknown",
                     std::to_string(target_w) + " W", result);
        return result;
    }
    if (std::fabs(after_w - static_cast<double>(target_w)) > 1.0) {
        result.detail = "nvidia-smi -pl " + std::to_string(target_w) + " reported success but the enforced limit is " +
                        std::to_string(after_w) + " W (from " + after_source + ")";
        log_mutation("set_power_limit_watts", have_before ? std::to_string(before_w) + " W" : "unknown",
                     std::to_string(after_w) + " W", result);
        return result;
    }
    if (range.min_mw > 0 && watts_to_mw(after_w) < range.min_mw) {
        result.detail = "refusing to leave the power limit below the minimum: enforced " + std::to_string(after_w) +
                        " W < " + std::to_string(range.min_mw / 1000u) + " W";
        log_mutation("set_power_limit_watts", have_before ? std::to_string(before_w) + " W" : "unknown",
                     std::to_string(after_w) + " W", result);
        return result;
    }

    // Read-only cross-check through the NVAPI power-policy ratio, when available.
    std::string cross_check;
    uint32_t pcm = 0;
    std::string pcm_error;
    if (read_power_policy_status_pcm_locked(pcm, pcm_error) && range.default_mw > 0) {
        const double expected_w = static_cast<double>(range.default_mw) / 1000.0 * static_cast<double>(pcm) / 100000.0;
        cross_check = " NVAPI power policy reports " + std::to_string(pcm) + " PCM (~" +
                      std::to_string(expected_w) + " W)";
    }

    result.ok = true;
    result.detail = "power limit applied: " + std::to_string(target_w) + " W (verified via " + after_source + ")" +
                    cross_check;
    if (clamp_note.empty() == false) {
        result.detail += "; " + clamp_note;
    }
    log_mutation("set_power_limit_watts", have_before ? std::to_string(before_w) + " W" : "unknown",
                 std::to_string(after_w) + " W", result);
    return result;
}

// ---------------------------------------------------------------------------
// Clock lock (nvidia-smi -lgc / -rgc; -lmc / -rmc).
// ---------------------------------------------------------------------------
bool validate_lock_window(uint32_t min_mhz, uint32_t max_mhz, std::string& error) {
    if (min_mhz < kMinCoreClockLockMhz || max_mhz > kMaxCoreClockLockMhz || min_mhz > max_mhz || min_mhz == 0) {
        error = "core clock lock " + std::to_string(min_mhz) + ".." + std::to_string(max_mhz) +
                " MHz is outside the supported envelope " + std::to_string(kMinCoreClockLockMhz) + ".." +
                std::to_string(kMaxCoreClockLockMhz) + " MHz with min <= max";
        return false;
    }
    return true;
}

ControlResult apply_core_clock_lock_locked(uint32_t min_mhz, uint32_t max_mhz) {
    ControlResult result;
    if (g_control.smi_path.empty()) {
        result.detail = "locking the core clock requires nvidia-smi, which was not found";
        return result;
    }
    if (g_control.have_lock_record && g_control.lock_min_mhz == min_mhz && g_control.lock_max_mhz == max_mhz) {
        result.ok = true;
        result.detail = "core clock is already locked to " + std::to_string(min_mhz) + ".." +
                        std::to_string(max_mhz) + " MHz (no change)";
        return result;
    }

    std::string before = g_control.have_lock_record
                             ? (std::to_string(g_control.lock_min_mhz) + ".." + std::to_string(g_control.lock_max_mhz) +
                                " MHz")
                             : std::string("unlocked");
    const std::string after = std::to_string(min_mhz) + ".." + std::to_string(max_mhz) + " MHz";

    const CommandResult command = run_nvidia_smi("-i " + std::to_string(g_control.device_index) + " -lgc " +
                                                 std::to_string(min_mhz) + "," + std::to_string(max_mhz));
    result.needs_elevation = looks_like_permission_problem(command.output, command.exit_code);
    if (command.launched == false) {
        result.detail = "could not run nvidia-smi: " + command.error;
        log_mutation("lock_core_clock", before, after, result);
        return result;
    }
    if (command.exit_code != 0) {
        result.detail = "nvidia-smi -lgc " + std::to_string(min_mhz) + "," + std::to_string(max_mhz) +
                        " failed with exit code " + std::to_string(command.exit_code) + ": " + command.output;
        if (result.needs_elevation) {
            result.detail += " (this operation needs elevation: relaunch elevated / accept the UAC prompt)";
        }
        log_mutation("lock_core_clock", before, after, result);
        return result;
    }

    g_control.have_lock_record = true;
    g_control.lock_min_mhz = min_mhz;
    g_control.lock_max_mhz = max_mhz;
    result.ok = true;
    result.detail = "core clock locked to " + after + " (nvidia-smi -lgc)";
    log_mutation("lock_core_clock", before, after, result);
    return result;
}

ControlResult clear_core_clock_lock_locked() {
    ControlResult result;
    if (g_control.smi_path.empty()) {
        result.ok = true;
        result.detail = "no nvidia-smi available; nothing to reset for the clock lock";
        return result;
    }
    const CommandResult command = run_nvidia_smi("-i " + std::to_string(g_control.device_index) + " -rgc");
    result.needs_elevation = looks_like_permission_problem(command.output, command.exit_code);
    if (command.launched == false) {
        result.detail = "could not run nvidia-smi: " + command.error;
        return result;
    }
    if (command.exit_code != 0) {
        result.detail = "nvidia-smi -rgc failed with exit code " + std::to_string(command.exit_code) + ": " +
                        command.output;
        if (result.needs_elevation) {
            result.detail += " (this operation needs elevation: relaunch elevated / accept the UAC prompt)";
        }
        return result;
    }
    g_control.have_lock_record = false;
    g_control.lock_min_mhz = 0;
    g_control.lock_max_mhz = 0;
    result.ok = true;
    result.detail = "core clock lock cleared (nvidia-smi -rgc)";
    return result;
}

// ---------------------------------------------------------------------------
// Undervolt / offsets.
// ---------------------------------------------------------------------------
ControlResult reset_undervolt_locked() {
    ControlResult result;
    bool did_something = false;
    std::string notes;

    const bool has_offsets = g_control.have_undervolt_record &&
                             (g_control.last_applied_core_delta_khz != 0 ||
                              g_control.last_applied_voltage_delta_uv != 0);
    if (has_offsets) {
        if (g_control.backend != Backend::Nvapi) {
            notes += "offsets were not applied through NVAPI, so there is nothing to restore there; ";
        } else {
            const bool touch_voltage = g_control.last_applied_voltage_delta_uv != 0;
            const int status = write_p0_offsets_locked(g_control.original_core_delta_khz,
                                                       g_control.original_voltage_delta_uv, touch_voltage);
            did_something = true;
            if (status == kNvapiOk) {
                notes += "P0 offsets restored to " + std::to_string(g_control.original_core_delta_khz / 1000) +
                         " MHz / " + std::to_string(g_control.original_voltage_delta_uv) + " uV; ";
            } else {
                result.needs_elevation = result.needs_elevation || nvapi_needs_elevation(status);
                notes += "restoring the P0 offsets failed: " + nvapi_status_name(status) + "; ";
            }
        }
    }

    ControlResult lock_result;
    if (g_control.have_lock_record) {
        lock_result = clear_core_clock_lock_locked();
        did_something = true;
        result.needs_elevation = result.needs_elevation || lock_result.needs_elevation;
        result.ok = lock_result.ok;
        notes += lock_result.detail;
        notes += "; ";
    }

    if (did_something == false) {
        result.ok = true;
        result.detail = "nothing to reset (no offsets or clock lock were applied by this process)";
        return result;
    }

    // Verify the restored state.
    if (g_control.have_undervolt_record) {
        int core_khz = 0;
        int volt_uv = 0;
        bool has_voltage = false;
        std::string error;
        if (read_p0_offsets_locked(core_khz, volt_uv, has_voltage, error)) {
            if (core_khz != g_control.original_core_delta_khz) {
                result.ok = false;
                notes += "verified core offset is " + std::to_string(core_khz) + " kHz, expected " +
                         std::to_string(g_control.original_core_delta_khz) + " kHz; ";
            }
        } else {
            result.ok = false;
            notes += "verification failed: " + error + "; ";
        }
    }

    g_control.last_applied_core_delta_khz = 0;
    g_control.last_applied_voltage_delta_uv = 0;

    result.detail = notes.empty() ? std::string("reset completed") : notes;
    if (result.detail.size() >= 2 && result.detail.compare(result.detail.size() - 2, 2, "; ") == 0) {
        result.detail.erase(result.detail.size() - 2);
    }
    return result;
}

ControlResult set_undervolt_locked(const UndervoltPlan& plan) {
    ControlResult result;

    if (g_control.ready == false) {
        result.detail = "gpu control is not initialised";
        return result;
    }

    // ---- Validation (safety envelope) ------------------------------------
    if (plan.core_clock_offset_mhz < kMinCoreClockOffsetMhz || plan.core_clock_offset_mhz > kMaxCoreClockOffsetMhz) {
        result.detail = "core clock offset " + std::to_string(plan.core_clock_offset_mhz) +
                        " MHz is outside the supported envelope " + std::to_string(kMinCoreClockOffsetMhz) + ".." +
                        std::to_string(kMaxCoreClockOffsetMhz) + " MHz";
        return result;
    }
    if (plan.voltage_offset_uv < kMinVoltageOffsetUv || plan.voltage_offset_uv > kMaxVoltageOffsetUv) {
        result.detail = "voltage offset " + std::to_string(plan.voltage_offset_uv) +
                        " uV is outside the supported envelope " + std::to_string(kMinVoltageOffsetUv) + ".." +
                        std::to_string(kMaxVoltageOffsetUv) + " uV (only undervolting is allowed)";
        return result;
    }
    if (plan.lock_core_clock) {
        std::string lock_error;
        if (validate_lock_window(plan.core_clock_min_mhz, plan.core_clock_max_mhz, lock_error) == false) {
            result.detail = lock_error;
            return result;
        }
    }

    const bool wants_offsets = plan.core_clock_offset_mhz != 0 || plan.voltage_offset_uv != 0;
    if (wants_offsets == false && plan.lock_core_clock == false) {
        result.ok = true;
        result.detail = "nothing to apply (offsets are zero and no clock lock was requested)";
        return result;
    }

    std::string before = "core_offset=" + std::to_string(plan.core_clock_offset_mhz) + " MHz,voltage_offset=" +
                         std::to_string(plan.voltage_offset_uv) + " uV";
    std::string notes;
    bool offsets_ok = (wants_offsets == false);
    bool guard_needed = false;

    // ---- NVAPI Pstates20 offsets -----------------------------------------
    if (wants_offsets) {
        if (g_control.backend != Backend::Nvapi || g_control.gpu_handle == nullptr) {
            notes += "clock/voltage offsets require NVAPI, which is not active on this device; ";
        } else {
            NvapiPstates20InfoV2 probe{};
            int status = kNvapiOk;
            if (read_pstates20_locked(probe, status) == false) {
                notes += "NvAPI_GPU_GetPstates20 failed: " + nvapi_status_name(status) + "; ";
            } else {
                const bool needs_voltage_entry = plan.voltage_offset_uv != 0;
                const bool has_voltage_entry = find_p0_core_voltage(probe) != nullptr;
                if (needs_voltage_entry && has_voltage_entry == false) {
                    // Verified on the target RTX 4060 Ti: numBaseVoltages == 0 and there
                    // are no over-voltage entries, so a uV offset cannot be expressed.
                    notes += "voltage offset is not applicable on this SKU: Pstates20 exposes no base-voltage "
                             "entry (numBaseVoltages=" + std::to_string(probe.num_base_voltages) +
                             ", ov.numVoltages=" + std::to_string(probe.ov.num_voltages) +
                             ") - apply a core clock lock instead; ";
                } else {
                    int current_core_khz = 0;
                    int current_volt_uv = 0;
                    bool current_has_voltage = false;
                    std::string read_error;
                    if (read_p0_offsets_locked(current_core_khz, current_volt_uv, current_has_voltage, read_error) ==
                        false) {
                        notes += read_error + "; ";
                    } else {
                        const int requested_core_khz = plan.core_clock_offset_mhz * 1000;
                        const bool already_applied =
                            current_core_khz == requested_core_khz &&
                            (needs_voltage_entry == false || current_volt_uv == plan.voltage_offset_uv);
                        if (already_applied) {
                            offsets_ok = true;
                            notes += "P0 offsets already applied (no change); ";
                        } else {
                            if (g_control.have_undervolt_record == false) {
                                g_control.have_undervolt_record = true;
                                g_control.original_core_delta_khz = current_core_khz;
                                g_control.original_voltage_delta_uv = current_volt_uv;
                            }
                            const int set_status = write_p0_offsets_locked(requested_core_khz,
                                                                           plan.voltage_offset_uv,
                                                                           needs_voltage_entry);
                            if (set_status != kNvapiOk) {
                                result.needs_elevation = result.needs_elevation || nvapi_needs_elevation(set_status);
                                notes += "NvAPI_GPU_SetPstates20 failed: " + nvapi_status_name(set_status);
                                if (set_status == kNvapiNotSupported) {
                                    notes += " (verified on this driver/SKU: NVAPI clock/voltage offsets are blocked "
                                             "on this consumer GeForce even for a no-op write; use the core clock lock "
                                             "fallback instead)";
                                }
                                notes += "; ";
                            } else {
                                guard_needed = true;
                                // Verify by re-reading the GPU before accepting the change.
                                int verified_core_khz = 0;
                                int verified_volt_uv = 0;
                                bool verified_has_voltage = false;
                                std::string verify_error;
                                if (read_p0_offsets_locked(verified_core_khz, verified_volt_uv, verified_has_voltage,
                                                           verify_error) == false) {
                                    notes += "verification failed: " + verify_error + "; ";
                                    const int restore = write_p0_offsets_locked(g_control.original_core_delta_khz,
                                                                                g_control.original_voltage_delta_uv,
                                                                                needs_voltage_entry);
                                    notes += (restore == kNvapiOk) ? "previous offsets restored; "
                                                                   : "restore also failed: " +
                                                                         nvapi_status_name(restore) + "; ";
                                } else if (verified_core_khz != requested_core_khz ||
                                           (needs_voltage_entry && verified_volt_uv != plan.voltage_offset_uv)) {
                                    notes += "applied offsets do not match the request (core " +
                                             std::to_string(verified_core_khz) + " kHz, voltage " +
                                             std::to_string(verified_volt_uv) + " uV) -> rolling back; ";
                                    const int restore = write_p0_offsets_locked(g_control.original_core_delta_khz,
                                                                                g_control.original_voltage_delta_uv,
                                                                                needs_voltage_entry);
                                    notes += (restore == kNvapiOk) ? "previous offsets restored; "
                                                                   : "restore also failed: " +
                                                                         nvapi_status_name(restore) + "; ";
                                } else {
                                    offsets_ok = true;
                                    g_control.last_applied_core_delta_khz = requested_core_khz;
                                    g_control.last_applied_voltage_delta_uv =
                                        needs_voltage_entry ? plan.voltage_offset_uv : 0;
                                    notes += "P0 offsets applied and verified (core " +
                                             std::to_string(plan.core_clock_offset_mhz) + " MHz, voltage " +
                                             std::to_string(plan.voltage_offset_uv) + " uV); ";
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- Core clock lock through nvidia-smi ------------------------------
    bool lock_ok = (plan.lock_core_clock == false);
    if (plan.lock_core_clock) {
        const ControlResult lock_result =
            apply_core_clock_lock_locked(plan.core_clock_min_mhz, plan.core_clock_max_mhz);
        lock_ok = lock_result.ok;
        result.needs_elevation = result.needs_elevation || lock_result.needs_elevation;
        notes += lock_result.detail + "; ";
        if (lock_result.ok) {
            guard_needed = true;
        }
    }

    // ---- Mandatory stability guard ---------------------------------------
    bool guard_ok = true;
    if (guard_needed) {
        Sleep(kSettleMs);
        std::string guard_reason;
        if (stability_guard_locked(guard_reason) == false) {
            guard_ok = false;
            notes += "stability guard failed: " + guard_reason + " -> rolling back; ";
            const ControlResult rollback = reset_undervolt_locked();
            result.needs_elevation = result.needs_elevation || rollback.needs_elevation;
            notes += rollback.detail + "; ";
            offsets_ok = false;
            lock_ok = false;
        } else {
            notes += "stability guard passed; ";
        }
    }

    result.ok = offsets_ok && lock_ok && guard_ok;
    result.detail = notes;
    if (result.detail.size() >= 2 && result.detail.compare(result.detail.size() - 2, 2, "; ") == 0) {
        result.detail.erase(result.detail.size() - 2);
    }

    std::string after = "core_offset=" + std::to_string(offsets_ok ? plan.core_clock_offset_mhz : 0) + " MHz";
    if (plan.lock_core_clock) {
        after += ",lock=" + std::to_string(plan.core_clock_min_mhz) + ".." +
                 std::to_string(plan.core_clock_max_mhz) + " MHz";
    }
    log_mutation("set_undervolt", before, after, result);
    return result;
}

// ---------------------------------------------------------------------------
// Fan control.
// ---------------------------------------------------------------------------
ControlResult set_fan_percent_locked(uint32_t percent) {
    ControlResult result;

    if (g_control.backend != Backend::Nvapi || g_control.gpu_handle == nullptr) {
        result.detail = "fan control requires NVAPI; the nvidia-smi backend cannot set fan speed (nvidia-smi has no "
                        "fan-set option on GeForce) - install/prepare nvapi64.dll or use an elevated vendor tool";
        return result;
    }
    if (g_control.nvapi.gpu_get_cooler_settings == nullptr || g_control.nvapi.gpu_set_cooler_levels == nullptr) {
        result.detail = "nvapi64.dll does not export the cooler-settings entry points "
                        "(NvAPI_GPU_GetCoolerSettings / NvAPI_GPU_SetCoolerLevels)";
        return result;
    }

    // 0 means automatic, never "off".
    const uint32_t requested = percent;
    uint32_t level = 0;
    uint32_t policy = kNvapiCoolerPolicyDefault;
    std::string note = "0 = automatic (driver controlled)";
    if (percent > 0) {
        level = (percent > 100u) ? 100u : percent;   // clamp; never above 100 %
        policy = kNvapiCoolerPolicyManual;
        note = "fan level " + std::to_string(level) + "%";
        if (percent > 100u) {
            note += " (requested " + std::to_string(percent) + "% was clamped to 100%)";
        }
    }

    // Read first: it validates the struct/version handshake and lets us report
    // idempotent no-ops without touching the GPU.
    NvapiCoolerSettingsV1 settings{};
    settings.version = kNvapiCoolerSettingsVersion;
    const int read_status = g_control.nvapi.gpu_get_cooler_settings(g_control.gpu_handle, 0, &settings);
    if (read_status != kNvapiOk) {
        result.needs_elevation = nvapi_needs_elevation(read_status);
        result.detail = "NvAPI_GPU_GetCoolerSettings failed: " + nvapi_status_name(read_status);
        if (read_status == kNvapiNotSupported) {
            result.detail += " - fan/cooler control is blocked on this consumer GeForce GPU; the fan stays under "
                             "driver control";
        }
        log_mutation("set_fan_percent", std::to_string(requested) + "%", "unchanged", result);
        return result;
    }
    const uint32_t current_level = (settings.count > 0) ? settings.coolers[0].current_level : 0;
    const uint32_t current_policy = (settings.count > 0) ? settings.coolers[0].current_policy : 0;

    if (settings.count > 0 && current_level == level && current_policy == policy) {
        result.ok = true;
        result.detail = "fan is already at " + note + " (no change)";
        log_mutation("set_fan_percent", std::to_string(current_level) + "%/" + std::to_string(current_policy),
                     std::to_string(level) + "%/" + std::to_string(policy), result);
        return result;
    }

    NvapiCoolerLevelsV1 levels{};
    levels.version = kNvapiCoolerLevelsVersion;
    levels.levels[0].level = level;
    levels.levels[0].policy = policy;
    const int set_status = g_control.nvapi.gpu_set_cooler_levels(g_control.gpu_handle, 0, &levels);
    const std::string before = std::to_string(current_level) + "% (policy " + std::to_string(current_policy) + ")";
    const std::string after = std::to_string(level) + "% (policy " + std::to_string(policy) + ")";

    if (set_status != kNvapiOk) {
        result.needs_elevation = nvapi_needs_elevation(set_status);
        result.detail = "NvAPI_GPU_SetCoolerLevels failed: " + nvapi_status_name(set_status);
        if (set_status == kNvapiNotSupported) {
            result.detail += " - fan/cooler control is blocked on this consumer GeForce GPU";
        }
        log_mutation("set_fan_percent", before, after, result);
        return result;
    }

    // Verify by re-reading the cooler settings.
    NvapiCoolerSettingsV1 verified{};
    verified.version = kNvapiCoolerSettingsVersion;
    const int verify_status = g_control.nvapi.gpu_get_cooler_settings(g_control.gpu_handle, 0, &verified);
    if (verify_status != kNvapiOk) {
        result.detail = "fan command was accepted but the settings could not be re-read: " +
                        nvapi_status_name(verify_status);
        log_mutation("set_fan_percent", before, after, result);
        return result;
    }
    const uint32_t verified_level = (verified.count > 0) ? verified.coolers[0].current_level : 0;
    const uint32_t verified_policy = (verified.count > 0) ? verified.coolers[0].current_policy : 0;
    if (percent > 0 && (verified_level != level || verified_policy != policy)) {
        result.detail = "fan command was accepted but the reported level/policy is " + std::to_string(verified_level) +
                        "%/" + std::to_string(verified_policy) + " instead of " + std::to_string(level) + "%/" +
                        std::to_string(policy);
        log_mutation("set_fan_percent", before, after, result);
        return result;
    }

    result.ok = true;
    result.detail = "fan set to " + note + " (verified through NvAPI_GPU_GetCoolerSettings)";
    log_mutation("set_fan_percent", before, after, result);
    return result;
}

// ---------------------------------------------------------------------------
// Restore defaults.
// ---------------------------------------------------------------------------
ControlResult restore_defaults_locked() {
    ControlResult result;
    std::string notes;
    bool all_ok = true;

    // 1. Power limit back to the default reported by nvidia-smi.
    PowerLimitRange range;
    std::string range_error;
    if (query_power_limit_range_locked(range, range_error) && range.valid && range.default_mw > 0) {
        const ControlResult power = set_power_limit_watts_locked(range.default_mw / 1000u);
        all_ok = all_ok && power.ok;
        result.needs_elevation = result.needs_elevation || power.needs_elevation;
        notes += "power: " + power.detail + "; ";
    } else {
        all_ok = false;
        notes += "power: default limit unknown - " + range_error + "; ";
    }

    // 2. Clock locks (also clears locks left by an earlier process).
    if (g_control.smi_path.empty() == false) {
        const CommandResult rgc = run_nvidia_smi("-i " + std::to_string(g_control.device_index) + " -rgc");
        const CommandResult rmc = run_nvidia_smi("-i " + std::to_string(g_control.device_index) + " -rmc");
        const bool rgc_ok = rgc.launched && rgc.exit_code == 0;
        const bool rmc_ok = rmc.launched && rmc.exit_code == 0;
        result.needs_elevation = result.needs_elevation ||
                                 looks_like_permission_problem(rgc.output, rgc.exit_code) ||
                                 looks_like_permission_problem(rmc.output, rmc.exit_code);
        if (rgc_ok && rmc_ok) {
            g_control.have_lock_record = false;
            notes += "clocks: core and memory clock locks cleared (nvidia-smi -rgc/-rmc); ";
        } else {
            all_ok = false;
            notes += "clocks: ";
            if (rgc_ok == false) {
                notes += "core reset failed (exit " + std::to_string(rgc.exit_code) + ": " +
                         (rgc.launched ? rgc.output : rgc.error) + "); ";
            }
            if (rmc_ok == false) {
                notes += "memory reset failed (exit " + std::to_string(rmc.exit_code) + ": " +
                         (rmc.launched ? rmc.output : rmc.error) + "); ";
            }
        }
    } else {
        notes += "clocks: nvidia-smi unavailable, clock locks not reset; ";
    }

    // 3. NVAPI offsets.
    if (g_control.have_undervolt_record && g_control.backend == Backend::Nvapi) {
        const ControlResult offsets = reset_undervolt_locked();
        all_ok = all_ok && offsets.ok;
        result.needs_elevation = result.needs_elevation || offsets.needs_elevation;
        notes += "offsets: " + offsets.detail + "; ";
    } else {
        notes += "offsets: nothing recorded by this process; ";
    }

    // 4. Fan back to automatic.
    const ControlResult fan = set_fan_percent_locked(0);
    if (fan.ok) {
        notes += "fan: automatic; ";
    } else {
        // Not being able to control the fan is not a hard failure on consumer GPUs,
        // but it must be reported.
        notes += "fan: " + fan.detail + "; ";
    }
    result.needs_elevation = result.needs_elevation || fan.needs_elevation;

    result.ok = all_ok;
    result.detail = notes;
    if (result.detail.size() >= 2 && result.detail.compare(result.detail.size() - 2, 2, "; ") == 0) {
        result.detail.erase(result.detail.size() - 2);
    }
    log_mutation("restore_defaults", "unknown", "driver defaults", result);
    return result;
}

// ---------------------------------------------------------------------------
// Init.
// ---------------------------------------------------------------------------
NvapiApi load_nvapi(std::string& error) {
    NvapiApi api;
    std::vector<std::wstring> candidates;
    candidates.push_back(L"nvapi64.dll");
    wchar_t system_directory[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        std::wstring in_system32(system_directory);
        in_system32 += L"\\nvapi64.dll";
        candidates.push_back(in_system32);
    }
    candidates.push_back(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvapi64.dll");

    for (const std::wstring& candidate : candidates) {
        api.module = LoadLibraryW(candidate.c_str());
        if (api.module != nullptr) {
            break;
        }
    }
    if (api.module == nullptr) {
        error = "nvapi64.dll could not be loaded";
        return NvapiApi{};
    }

    const FARPROC query = GetProcAddress(api.module, "nvapi_QueryInterface");
    if (query == nullptr) {
        error = "nvapi64.dll does not export nvapi_QueryInterface";
        FreeLibrary(api.module);
        return NvapiApi{};
    }
    api.query_interface = reinterpret_cast<NvapiQueryInterfaceFn>(query);

    const auto resolve = [&api](uint32_t id) -> void* {
        return api.query_interface(id);
    };
    api.initialize = reinterpret_cast<NvapiInitializeFn>(resolve(kNvapiInitialize));
    api.unload = reinterpret_cast<NvapiUnloadFn>(resolve(kNvapiUnload));
    api.enum_physical_gpus = reinterpret_cast<NvapiEnumPhysicalGpusFn>(resolve(kNvapiEnumPhysicalGPUs));
    api.gpu_get_full_name = reinterpret_cast<NvapiGpuGetFullNameFn>(resolve(kNvapiGpuGetFullName));
    api.gpu_get_pstates20 = reinterpret_cast<NvapiGpuGetPstates20Fn>(resolve(kNvapiGpuGetPstates20));
    api.gpu_set_pstates20 = reinterpret_cast<NvapiGpuSetPstates20Fn>(resolve(kNvapiGpuSetPstates20));
    api.gpu_get_cooler_settings =
        reinterpret_cast<NvapiGpuGetCoolerSettingsFn>(resolve(kNvapiGpuGetCoolerSettings));
    api.gpu_set_cooler_levels = reinterpret_cast<NvapiGpuSetCoolerLevelsFn>(resolve(kNvapiGpuSetCoolerLevels));
    api.power_policies_get_info =
        reinterpret_cast<NvapiGpuPowerPoliciesGetInfoFn>(resolve(kNvapiGpuClientPowerPoliciesGetInfo));
    api.power_policies_get_status =
        reinterpret_cast<NvapiGpuPowerPoliciesGetStatusFn>(resolve(kNvapiGpuClientPowerPoliciesGetStatus));
    api.power_policies_set_status =
        reinterpret_cast<NvapiGpuPowerPoliciesSetStatusFn>(resolve(kNvapiGpuClientPowerPoliciesSetStatus));
    api.power_topology_get_status =
        reinterpret_cast<NvapiGpuPowerTopologyGetStatusFn>(resolve(kNvapiGpuClientPowerTopologyGetStatus));

    if (api.usable() == false) {
        error = "nvapi64.dll is missing required entry points";
        FreeLibrary(api.module);
        return NvapiApi{};
    }
    return api;
}

bool nvidia_smi_probe(unsigned device_index, std::string& error) {
    const CommandResult result = run_nvidia_smi("-i " + std::to_string(device_index) +
                                               " --query-gpu=name --format=csv,noheader,nounits");
    if (result.launched == false) {
        error = result.error;
        return false;
    }
    if (result.exit_code != 0) {
        error = "nvidia-smi exited with code " + std::to_string(result.exit_code) + ": " + result.output;
        return false;
    }
    if (result.output.empty()) {
        error = "nvidia-smi returned no data for GPU " + std::to_string(device_index);
        return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------
bool GpuControl::init(unsigned device_index, std::string& error) {
    try {
        std::lock_guard<std::mutex> lock(g_control.mutex);
        if (g_control.initialised) {
            if (g_control.ready) {
                return true;
            }
            error = "gpu control backend unavailable (initialisation was already attempted)";
            return false;
        }
        g_control.initialised = true;
        error.clear();
        g_control.device_index = device_index;
        g_control.smi_path = std::wstring();

        // Resolve nvidia-smi beforehand: several operations use it even when NVAPI is
        // active (watt-based power limits and clock locks).
        {
            wchar_t system_directory[MAX_PATH] = {};
            const UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
            if (length > 0 && length < MAX_PATH) {
                std::wstring candidate(system_directory);
                candidate += L"\\nvidia-smi.exe";
                if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    g_control.smi_path = candidate;
                }
            }
            if (g_control.smi_path.empty()) {
                std::string probe_error;
                g_control.smi_path = L"nvidia-smi.exe";
                if (nvidia_smi_probe(device_index, probe_error) == false) {
                    g_control.smi_path.clear();
                }
            }
        }

        // Prefer NVAPI.
        std::string nvapi_error;
        NvapiApi api = load_nvapi(nvapi_error);
        if (api.usable()) {
            const int init_status = api.initialize();
            if (init_status == kNvapiOk) {
                void* handles[64] = {};
                int count = 0;
                const int enum_status = api.enum_physical_gpus(handles, &count);
                if (enum_status == kNvapiOk && count > 0 && device_index < static_cast<unsigned>(count) &&
                    handles[device_index] != nullptr) {
                    g_control.nvapi = api;
                    g_control.gpu_handle = handles[device_index];
                    if (api.gpu_get_full_name != nullptr) {
                        char name[64] = {};
                        if (api.gpu_get_full_name(g_control.gpu_handle, name) == kNvapiOk) {
                            g_control.gpu_name.assign(name);
                        }
                    }
                    g_control.backend = Backend::Nvapi;
                    g_control.ready = true;
                    return true;
                }
                nvapi_error = "NvAPI_EnumPhysicalGPUs failed or GPU index " + std::to_string(device_index) +
                              " is not available (reported " + std::to_string(count) + " devices, status " +
                              nvapi_status_name(enum_status) + ")";
                (void)api.unload();
            } else {
                nvapi_error = "NvAPI_Initialize failed: " + nvapi_status_name(init_status);
            }
            if (api.module != nullptr) {
                FreeLibrary(api.module);
            }
        }

        // Fall back to the nvidia-smi command line backend.
        std::string smi_error;
        if (g_control.smi_path.empty() == false && nvidia_smi_probe(device_index, smi_error)) {
            g_control.backend = Backend::NvidiaSmi;
            g_control.ready = true;
            return true;
        }
        if (g_control.smi_path.empty()) {
            smi_error = "nvidia-smi was not found";
        }

        g_control.backend = Backend::None;
        error = "no usable GPU control backend - NVAPI: " + nvapi_error + "; nvidia-smi: " + smi_error;
        return false;
    } catch (...) {
        error = "unexpected exception while initialising gpu control";
        return false;
    }
}

void GpuControl::shutdown() {
    try {
        std::lock_guard<std::mutex> lock(g_control.mutex);
        if (g_control.backend == Backend::Nvapi && g_control.nvapi.usable()) {
            (void)g_control.nvapi.unload();
        }
        if (g_control.nvapi.module != nullptr) {
            FreeLibrary(g_control.nvapi.module);
        }
        // ControlState contains a std::mutex, so it is neither copyable nor movable;
        // reset the fields individually instead of assigning a fresh instance.
        g_control.nvapi = NvapiApi{};
        g_control.gpu_handle = nullptr;
        g_control.gpu_name.clear();
        g_control.device_index = 0;
        g_control.smi_path.clear();
        g_control.backend = Backend::None;
        g_control.ready = false;
        g_control.initialised = false;
        g_control.have_undervolt_record = false;
        g_control.original_core_delta_khz = 0;
        g_control.original_voltage_delta_uv = 0;
        g_control.last_applied_core_delta_khz = 0;
        g_control.last_applied_voltage_delta_uv = 0;
        g_control.have_lock_record = false;
        g_control.lock_min_mhz = 0;
        g_control.lock_max_mhz = 0;
        g_control.have_power_range = false;
        g_control.power_range = PowerLimitRange{};
    } catch (...) {
        // shutdown() never propagates.
    }
}

const char* GpuControl::backend_name() {
    try {
        std::lock_guard<std::mutex> lock(g_control.mutex);
        switch (g_control.backend) {
            case Backend::Nvapi:
                return "nvapi";
            case Backend::NvidiaSmi:
                return "nvidia-smi";
            case Backend::None:
            default:
                return "none";
        }
    } catch (...) {
        return "none";
    }
}

bool GpuControl::query_power_limit_range(PowerLimitRange& out, std::string& error) {
    try {
        std::lock_guard<std::mutex> lock(g_control.mutex);
        if (g_control.ready == false) {
            error = "gpu control is not initialised";
            return false;
        }
        return query_power_limit_range_locked(out, error);
    } catch (...) {
        error = "unexpected exception while querying the power limit range";
        return false;
    }
}

ControlResult GpuControl::set_power_limit_watts(uint32_t watts) {
    try {
        std::lock_guard<std::mutex> lock(g_control.mutex);
        return set_power_limit_watts_locked(watts);
    } catch (...) {
        ControlResult result;
        result.detail = "unexpected exception while setting the power limit";
        return result;
    }
}

ControlResult GpuControl::set_undervolt(const UndervoltPlan& plan) {
    try {
        std::lock_guard<std::mutex> lock(g_control.mutex);
        return set_undervolt_locked(plan);
    } catch (...) {
        ControlResult result;
        result.detail = "unexpected exception while applying the undervolt plan";
        return result;
    }
}

ControlResult GpuControl::reset_undervolt() {
    try {
        std::lock_guard<std::mutex> lock(g_control.mutex);
        if (g_control.ready == false) {
            ControlResult result;
            result.detail = "gpu control is not initialised";
            return result;
        }
        const ControlResult result = reset_undervolt_locked();
        log_mutation("reset_undervolt", "recorded state", "captured/original state", result);
        return result;
    } catch (...) {
        ControlResult result;
        result.detail = "unexpected exception while resetting the undervolt";
        return result;
    }
}

ControlResult GpuControl::set_fan_percent(uint32_t percent) {
    try {
        std::lock_guard<std::mutex> lock(g_control.mutex);
        if (g_control.ready == false) {
            ControlResult result;
            result.detail = "gpu control is not initialised";
            return result;
        }
        return set_fan_percent_locked(percent);
    } catch (...) {
        ControlResult result;
        result.detail = "unexpected exception while setting the fan speed";
        return result;
    }
}

ControlResult GpuControl::restore_defaults() {
    try {
        std::lock_guard<std::mutex> lock(g_control.mutex);
        if (g_control.ready == false) {
            ControlResult result;
            result.detail = "gpu control is not initialised";
            return result;
        }
        return restore_defaults_locked();
    } catch (...) {
        ControlResult result;
        result.detail = "unexpected exception while restoring the defaults";
        return result;
    }
}

}  // namespace grin
