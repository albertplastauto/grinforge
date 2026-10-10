// GrinForge - GRIN Cuckatoo32 GPU miner, open source, no developer fee.
// Copyright (c) 2026 albertplastauto
// SPDX-License-Identifier: MIT
//
//
// telemetry.cpp - NVML (dynamically loaded) / nvidia-smi telemetry backend.
//
// Design notes
// ------------
// * nvml.dll is loaded with LoadLibraryW() and every entry point is resolved with
//   GetProcAddress(). Nothing is linked against nvml.lib and the CUDA Toolkit is not
//   needed at build or run time - only the NVIDIA display driver.
// * The NVML ABI is declared locally instead of including nvml.h so that this module
//   has no dependency on the CUDA Toolkit include directory. Every struct/enum used
//   here was verified against the public NVML header (the go-nvml generated mirror of
//   the shipped header) and exercised against the real nvml.dll on the target machine
//   (RTX 4060 Ti, driver 617.14) with a ctypes harness before being written:
//     - nvmlMemory_v2_t is 40 bytes and its version field is 40 | (2 << 24);
//     - nvmlFieldValue_t is 40 bytes; NVML_FI_DEV_MEMORY_TEMP == 82;
//     - nvmlDeviceGetMemoryInfo_v2 reported 8585740288 B total (8188 MiB);
//     - limit constraints reported 100000/160000 mW; the enforced limit 160000 mW;
//     - NVML_TEMPERATURE_THRESHOLD_MEM_MAX and NVML_FI_DEV_MEMORY_TEMP returned
//       NVML_ERROR_NOT_SUPPORTED (3) on this SKU -> memory temperature = -1.
// * No exception ever escapes: every public entry point catches (...) and converts a
//   failure into (false, error).
// * read() caches handles/names at init() time, so it is safe to poll from a tight
//   loop. The nvidia-smi fallback costs one process spawn per sample and is therefore
//   additionally rate-limited to one spawn per kNvidiaSmiCacheTtlMs (same device).
//
#include "telemetry.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

namespace grin {
namespace {

// ---------------------------------------------------------------------------
// Minimal NVML ABI (see the file header for verification provenance).
// ---------------------------------------------------------------------------
using NvmlDevice = void*;
using NvmlReturn = int;

constexpr int kNvmlSuccess = 0;

constexpr int kNvmlTemperatureGpu = 0;      // nvmlTemperatureSensors_t: NVML_TEMPERATURE_GPU
constexpr int kNvmlThresholdShutdown = 0;   // nvmlTemperatureThresholds_t
constexpr int kNvmlThresholdSlowdown = 1;
constexpr int kNvmlThresholdMemMax = 2;
constexpr int kNvmlThresholdGpuMax = 3;
constexpr int kNvmlClockGraphics = 0;       // nvmlClockType_t
constexpr int kNvmlClockSm = 1;
constexpr int kNvmlClockMem = 2;
// nvmlPcieUtilCounter_t: the sample is in KB/s with 1 KB granularity.
constexpr int kNvmlPcieUtilTxBytes = 0;
constexpr int kNvmlPcieUtilRxBytes = 1;
// nvmlFieldValue_t::fieldId for the memory-junction temperature (may be unsupported).
constexpr int kNvmlFiDevMemoryTemp = 82;

constexpr int kNvmlValueTypeDouble = 0;       // nvmlValueType_t
constexpr int kNvmlValueTypeUnsignedInt = 1;

struct NvmlMemoryV2 {
    unsigned           version;   // must be kNvmlMemoryV2Version
    unsigned long long total;     // bytes
    unsigned long long reserved;  // bytes
    unsigned long long free;      // bytes
    unsigned long long used;      // bytes
};

struct NvmlUtilization {
    unsigned gpu;
    unsigned memory;
};

union NvmlValue {
    double             dVal;
    int                siVal;
    unsigned           uiVal;
    unsigned long      ulVal;
    unsigned long long ullVal;
    long long          sllVal;
    unsigned short     usVal;
};

struct NvmlFieldValue {
    unsigned  fieldId;
    unsigned  scopeId;
    long long timestamp;
    long long latencyUsec;
    int       valueType;
    int       nvmlReturn;
    NvmlValue value;
};

static_assert(sizeof(NvmlMemoryV2) == 40, "nvmlMemory_v2_t must be 40 bytes");
static_assert(sizeof(NvmlFieldValue) == 40, "nvmlFieldValue_t must be 40 bytes");

// NVML_STRUCT_VERSION(Memory, 2) == sizeof(nvmlMemory_v2_t) | (2 << 24)
constexpr unsigned kNvmlMemoryV2Version = static_cast<unsigned>(sizeof(NvmlMemoryV2)) | (2u << 24);

struct NvmlApi {
    HMODULE module = nullptr;

    NvmlReturn(__cdecl* init)(void) = nullptr;
    NvmlReturn(__cdecl* shutdown)(void) = nullptr;
    NvmlReturn(__cdecl* device_get_count)(unsigned* count) = nullptr;
    NvmlReturn(__cdecl* device_get_handle_by_index)(unsigned index, NvmlDevice* device) = nullptr;
    NvmlReturn(__cdecl* device_get_name)(NvmlDevice device, char* name, unsigned length) = nullptr;
    NvmlReturn(__cdecl* device_get_memory_info_v2)(NvmlDevice device, NvmlMemoryV2* memory) = nullptr;
    NvmlReturn(__cdecl* device_get_temperature)(NvmlDevice device, int sensor, unsigned* temp) = nullptr;
    NvmlReturn(__cdecl* device_get_temperature_threshold)(NvmlDevice device, int threshold, unsigned* temp) = nullptr;
    NvmlReturn(__cdecl* device_get_power_usage)(NvmlDevice device, unsigned* milliwatts) = nullptr;
    NvmlReturn(__cdecl* device_get_enforced_power_limit)(NvmlDevice device, unsigned* milliwatts) = nullptr;
    NvmlReturn(__cdecl* device_get_power_limit_constraints)(NvmlDevice device, unsigned* min_mw, unsigned* max_mw) = nullptr;
    NvmlReturn(__cdecl* device_get_fan_speed)(NvmlDevice device, unsigned* percent) = nullptr;
    NvmlReturn(__cdecl* device_get_clock_info)(NvmlDevice device, int clock_type, unsigned* mhz) = nullptr;
    NvmlReturn(__cdecl* device_get_utilization_rates)(NvmlDevice device, NvmlUtilization* utilization) = nullptr;
    NvmlReturn(__cdecl* device_get_current_clocks_throttle_reasons)(NvmlDevice device, unsigned long long* reasons) = nullptr;
    NvmlReturn(__cdecl* device_get_pcie_throughput)(NvmlDevice device, int counter, unsigned* kb_per_s) = nullptr;
    NvmlReturn(__cdecl* device_get_field_values)(NvmlDevice device, int count, NvmlFieldValue* values) = nullptr;
    NvmlReturn(__cdecl* system_get_driver_version)(char* version, unsigned length) = nullptr;
    const char*(__cdecl* error_string)(NvmlReturn result) = nullptr;

    // True when the indispensable entry points were resolved.
    bool usable() const {
        return module != nullptr && init != nullptr && shutdown != nullptr &&
               device_get_count != nullptr && device_get_handle_by_index != nullptr;
    }
};

enum class Backend { None, Nvml, NvidiaSmi };

// Bounded number of devices (a mining rig target far beyond anything this module
// needs) so the cached vectors stay small and predictable.
constexpr unsigned kMaxDevices = 64;

// Timeout for a single nvidia-smi invocation.
constexpr DWORD kCommandTimeoutMs = 5000;

// Hard cap on captured process output (protects against a runaway child).
constexpr size_t kMaxCapturedBytes = 256 * 1024;

// nvidia-smi fallback: minimum interval between two process spawns for the same
// device index. A tight polling loop must not fork hundreds of processes per second.
constexpr uint64_t kNvidiaSmiCacheTtlMs = 200;

// ---------------------------------------------------------------------------
// Global (process-wide) state: the resolved NVML function table plus the cached
// device handles/names that the specification requires after init().
// ---------------------------------------------------------------------------
struct TelemetryState {
    std::shared_mutex mutex;

    bool         initialised = false;
    bool         ready = false;
    Backend      backend = Backend::None;
    NvmlApi      nvml;
    std::wstring smi_path;             // resolved nvidia-smi.exe path

    std::vector<NvmlDevice>  handles;  // NVML backend
    std::vector<std::string> names;
    std::string driver_version;              // filled at init; empty when unknown
    std::vector<double>      cached_min_w;  // power-management limit constraints
    std::vector<double>      cached_max_w;

    // nvidia-smi backend cache (see kNvidiaSmiCacheTtlMs).
    uint64_t     smi_cache_stamp_ms = 0;
    unsigned     smi_cache_index = 0;
    GpuTelemetry smi_cache_value;
};

TelemetryState g_state;

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

// nvidia-smi renders unsupported fields as "N/A" or "[N/A]".
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

// ASCII-only widening used for command lines and error text (nvidia-smi arguments
// are pure ASCII, and the system message is not part of the command line).
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

uint64_t now_unix_ms() {
    FILETIME file_time{};
    GetSystemTimePreciseAsFileTime(&file_time);
    ULARGE_INTEGER value{};
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    // FILETIME counts 100 ns ticks since 1601-01-01; this many of them separate that
    // epoch from the Unix epoch.
    constexpr unsigned long long kFileTimeUnixEpochDelta = 116444736000000000ULL;
    if (value.QuadPart < kFileTimeUnixEpochDelta) {
        return 0;
    }
    return static_cast<uint64_t>((value.QuadPart - kFileTimeUnixEpochDelta) / 10000ULL);
}

// Resolves a system executable: try the explicit System32 path first, then let
// CreateProcessW resolve the bare name through PATH (C:\Windows\System32\ is on it).
std::wstring locate_system_executable(const wchar_t* file_name) {
    wchar_t system_directory[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        std::wstring candidate(system_directory);
        candidate += L'\\';
        candidate += file_name;
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }
    return std::wstring(file_name);
}

// Runs a command line, capturing merged stdout+stderr, with a hard timeout.
bool run_process_capture(const std::wstring& command_line, DWORD timeout_ms,
                         std::string& captured, DWORD& exit_code, std::string& error) {
    captured.clear();
    exit_code = 0;

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
    attributes.bInheritHandle = TRUE;
    attributes.lpSecurityDescriptor = nullptr;

    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (CreatePipe(&read_end, &write_end, &attributes, 0) == FALSE) {
        error = "CreatePipe failed: " + system_error_text(GetLastError());
        return false;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul_device = CreateFileW(L"NUL", GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                                    OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(STARTUPINFOW);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nul_device;
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;

    PROCESS_INFORMATION process{};
    std::wstring mutable_command = command_line;  // CreateProcessW may rewrite the buffer
    const BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    const DWORD create_error = GetLastError();

    CloseHandle(write_end);
    if (nul_device != nullptr && nul_device != INVALID_HANDLE_VALUE) {
        CloseHandle(nul_device);
    }

    if (created == FALSE) {
        CloseHandle(read_end);
        error = "CreateProcessW(\"" + narrow_ascii(command_line) + "\") failed: " + system_error_text(create_error);
        return false;
    }

    const auto drain = [&read_end, &captured]() {
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
            if (captured.size() < kMaxCapturedBytes) {
                captured.append(buffer, received);
            }
        }
    };

    bool timed_out = false;
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
    for (;;) {
        drain();
        const DWORD wait = WaitForSingleObject(process.hProcess, 20);
        if (wait == WAIT_OBJECT_0) {
            drain();  // collect whatever is left in the pipe
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
            exit_code = code;
        }
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(read_end);

    if (captured.size() > kMaxCapturedBytes) {
        captured.resize(kMaxCapturedBytes);
    }
    if (timed_out) {
        error = "command timed out after " + std::to_string(timeout_ms) + " ms";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// NVML backend.
// ---------------------------------------------------------------------------

NvmlApi load_nvml() {
    // Candidate locations, in order: the bare name (resolved through the DLL search
    // order, which finds C:\Windows\System32\nvml.dll), the explicit System32 path
    // (present and verified on the target machine), then the legacy NVSMI directory
    // used by older drivers.
    std::vector<std::wstring> candidates;
    candidates.push_back(L"nvml.dll");
    wchar_t system_directory[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        std::wstring in_system32(system_directory);
        in_system32 += L"\\nvml.dll";
        candidates.push_back(in_system32);
    }
    candidates.push_back(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");

    HMODULE module = nullptr;
    for (const std::wstring& candidate : candidates) {
        module = LoadLibraryW(candidate.c_str());
        if (module != nullptr) {
            break;
        }
    }
    if (module == nullptr) {
        return NvmlApi{};
    }

    const auto resolve = [module](const char* symbol) -> FARPROC {
        return GetProcAddress(module, symbol);
    };

    // Entry points are resolved individually: a driver may not export a deprecated
    // one, in which case the corresponding field simply stays unknown (-1).
    NvmlApi api;
    api.module = module;
    api.init = reinterpret_cast<NvmlReturn(__cdecl*)(void)>(resolve("nvmlInit_v2"));
    api.shutdown = reinterpret_cast<NvmlReturn(__cdecl*)(void)>(resolve("nvmlShutdown"));
    api.device_get_count = reinterpret_cast<NvmlReturn(__cdecl*)(unsigned*)>(resolve("nvmlDeviceGetCount_v2"));
    api.device_get_handle_by_index =
        reinterpret_cast<NvmlReturn(__cdecl*)(unsigned, NvmlDevice*)>(resolve("nvmlDeviceGetHandleByIndex_v2"));
    api.device_get_name =
        reinterpret_cast<NvmlReturn(__cdecl*)(NvmlDevice, char*, unsigned)>(resolve("nvmlDeviceGetName"));
    api.device_get_memory_info_v2 =
        reinterpret_cast<NvmlReturn(__cdecl*)(NvmlDevice, NvmlMemoryV2*)>(resolve("nvmlDeviceGetMemoryInfo_v2"));
    api.device_get_temperature =
        reinterpret_cast<NvmlReturn(__cdecl*)(NvmlDevice, int, unsigned*)>(resolve("nvmlDeviceGetTemperature"));
    api.device_get_temperature_threshold = reinterpret_cast<NvmlReturn(__cdecl*)(NvmlDevice, int, unsigned*)>(
        resolve("nvmlDeviceGetTemperatureThreshold"));
    api.device_get_power_usage =
        reinterpret_cast<NvmlReturn(__cdecl*)(NvmlDevice, unsigned*)>(resolve("nvmlDeviceGetPowerUsage"));
    api.device_get_enforced_power_limit =
        reinterpret_cast<NvmlReturn(__cdecl*)(NvmlDevice, unsigned*)>(resolve("nvmlDeviceGetEnforcedPowerLimit"));
    api.device_get_power_limit_constraints = reinterpret_cast<NvmlReturn(__cdecl*)(NvmlDevice, unsigned*, unsigned*)>(
        resolve("nvmlDeviceGetPowerManagementLimitConstraints"));
    api.device_get_fan_speed =
        reinterpret_cast<NvmlReturn(__cdecl*)(NvmlDevice, unsigned*)>(resolve("nvmlDeviceGetFanSpeed"));
    api.device_get_clock_info =
        reinterpret_cast<NvmlReturn(__cdecl*)(NvmlDevice, int, unsigned*)>(resolve("nvmlDeviceGetClockInfo"));
    api.device_get_utilization_rates =
        reinterpret_cast<NvmlReturn(__cdecl*)(NvmlDevice, NvmlUtilization*)>(resolve("nvmlDeviceGetUtilizationRates"));
    api.device_get_current_clocks_throttle_reasons =
        reinterpret_cast<NvmlReturn(__cdecl*)(NvmlDevice, unsigned long long*)>(
            resolve("nvmlDeviceGetCurrentClocksThrottleReasons"));
    api.device_get_pcie_throughput =
        reinterpret_cast<NvmlReturn(__cdecl*)(NvmlDevice, int, unsigned*)>(resolve("nvmlDeviceGetPcieThroughput"));
    api.device_get_field_values =
        reinterpret_cast<NvmlReturn(__cdecl*)(NvmlDevice, int, NvmlFieldValue*)>(resolve("nvmlDeviceGetFieldValues"));
    api.system_get_driver_version =
        reinterpret_cast<NvmlReturn(__cdecl*)(char*, unsigned)>(resolve("nvmlSystemGetDriverVersion"));
    api.error_string = reinterpret_cast<const char*(__cdecl*)(NvmlReturn)>(resolve("nvmlErrorString"));
    return api;
}

std::string nvml_error_text(const NvmlApi& api, NvmlReturn result) {
    if (api.error_string != nullptr) {
        const char* text = api.error_string(result);
        if (text != nullptr && text[0] != '\0') {
            return std::string(text) + " (NVML code " + std::to_string(result) + ")";
        }
    }
    return "NVML code " + std::to_string(result);
}

// Memory-junction temperature; unsupported on many consumer SKUs (verified:
// NVML_ERROR_NOT_SUPPORTED on the target RTX 4060 Ti) -> returns false.
bool read_memory_temperature(const NvmlApi& api, NvmlDevice device, double& out_c) {
    if (api.device_get_field_values == nullptr) {
        return false;
    }
    NvmlFieldValue field{};
    field.fieldId = kNvmlFiDevMemoryTemp;
    field.scopeId = 0;
    if (api.device_get_field_values(device, 1, &field) != kNvmlSuccess) {
        return false;
    }
    if (field.nvmlReturn != kNvmlSuccess) {
        return false;
    }
    if (field.valueType == kNvmlValueTypeUnsignedInt) {
        out_c = static_cast<double>(field.value.uiVal);
        return true;
    }
    if (field.valueType == kNvmlValueTypeDouble) {
        out_c = field.value.dVal;
        return true;
    }
    return false;
}

// Power-management limit constraints in watts. Cached at init() and refreshed here
// when the cached values are missing.
bool read_power_constraints(const NvmlApi& api, NvmlDevice device, double& min_w, double& max_w) {
    if (api.device_get_power_limit_constraints == nullptr) {
        return false;
    }
    unsigned min_mw = 0;
    unsigned max_mw = 0;
    if (api.device_get_power_limit_constraints(device, &min_mw, &max_mw) != kNvmlSuccess) {
        return false;
    }
    if (max_mw == 0) {
        return false;
    }
    min_w = static_cast<double>(min_mw) / 1000.0;
    max_w = static_cast<double>(max_mw) / 1000.0;
    return true;
}

void read_nvml(const NvmlApi& api, NvmlDevice device, unsigned index, GpuTelemetry& out) {
    out = GpuTelemetry{};
    out.index = index;

    bool temperature_ok = false;
    bool power_ok = false;

    char name_buffer[96] = {};
    if (api.device_get_name != nullptr && api.device_get_name(device, name_buffer, sizeof(name_buffer)) == kNvmlSuccess) {
        out.name.assign(name_buffer);
    }

    if (api.device_get_temperature != nullptr) {
        unsigned value = 0;
        if (api.device_get_temperature(device, kNvmlTemperatureGpu, &value) == kNvmlSuccess) {
            out.temperature_c = static_cast<double>(value);
            temperature_ok = true;
        }
    }

    double memory_temperature = -1.0;
    if (read_memory_temperature(api, device, memory_temperature)) {
        out.memory_temperature_c = memory_temperature;
    }

    if (api.device_get_power_usage != nullptr) {
        unsigned milliwatts = 0;
        if (api.device_get_power_usage(device, &milliwatts) == kNvmlSuccess) {
            out.power_w = static_cast<double>(milliwatts) / 1000.0;
            power_ok = true;
        }
    }
    if (api.device_get_enforced_power_limit != nullptr) {
        unsigned milliwatts = 0;
        if (api.device_get_enforced_power_limit(device, &milliwatts) == kNvmlSuccess) {
            out.power_limit_w = static_cast<double>(milliwatts) / 1000.0;
        }
    }
    {
        double min_w = -1.0;
        double max_w = -1.0;
        if (read_power_constraints(api, device, min_w, max_w)) {
            out.power_min_w = min_w;
            out.power_max_w = max_w;
        }
    }

    if (api.device_get_fan_speed != nullptr) {
        unsigned percent = 0;
        if (api.device_get_fan_speed(device, &percent) == kNvmlSuccess) {
            // A reported 0 is a real reading (zero-RPM fan-stop mode), not "unknown".
            out.fan_percent = static_cast<double>(percent);
        }
    }

    if (api.device_get_clock_info != nullptr) {
        unsigned value = 0;
        if (api.device_get_clock_info(device, kNvmlClockGraphics, &value) == kNvmlSuccess) {
            out.core_clock_mhz = static_cast<double>(value);
        }
        if (api.device_get_clock_info(device, kNvmlClockMem, &value) == kNvmlSuccess) {
            out.memory_clock_mhz = static_cast<double>(value);
        }
        (void)api.device_get_clock_info(device, kNvmlClockSm, &value);
    }

    if (api.device_get_memory_info_v2 != nullptr) {
        NvmlMemoryV2 memory{};
        memory.version = kNvmlMemoryV2Version;
        if (api.device_get_memory_info_v2(device, &memory) == kNvmlSuccess) {
            out.memory_total_bytes = memory.total;
            out.memory_used_bytes = memory.used;
        }
    }

    if (api.device_get_utilization_rates != nullptr) {
        NvmlUtilization utilization{};
        if (api.device_get_utilization_rates(device, &utilization) == kNvmlSuccess) {
            out.utilization_gpu_percent = static_cast<double>(utilization.gpu);
            out.utilization_memory_percent = static_cast<double>(utilization.memory);
        }
    }

    if (api.device_get_current_clocks_throttle_reasons != nullptr) {
        unsigned long long reasons = 0;
        if (api.device_get_current_clocks_throttle_reasons(device, &reasons) == kNvmlSuccess) {
            out.throttle_reasons = static_cast<uint64_t>(reasons);
        }
    }

    if (api.device_get_pcie_throughput != nullptr) {
        unsigned value = 0;
        if (api.device_get_pcie_throughput(device, kNvmlPcieUtilTxBytes, &value) == kNvmlSuccess) {
            out.pcie_tx_kib_per_s = static_cast<double>(value);
        }
        if (api.device_get_pcie_throughput(device, kNvmlPcieUtilRxBytes, &value) == kNvmlSuccess) {
            out.pcie_rx_kib_per_s = static_cast<double>(value);
        }
    }

    out.timestamp_unix_ms = now_unix_ms();
    out.valid = temperature_ok && power_ok;
}

bool read_nvml_thermal_limits(const NvmlApi& api, NvmlDevice device, ThermalLimits& out) {
    if (api.device_get_temperature_threshold == nullptr) {
        return false;
    }
    bool any = false;
    unsigned value = 0;
    if (api.device_get_temperature_threshold(device, kNvmlThresholdSlowdown, &value) == kNvmlSuccess) {
        out.slowdown_c = static_cast<double>(value);
        any = true;
    }
    if (api.device_get_temperature_threshold(device, kNvmlThresholdShutdown, &value) == kNvmlSuccess) {
        out.shutdown_c = static_cast<double>(value);
        any = true;
    }
    if (api.device_get_temperature_threshold(device, kNvmlThresholdGpuMax, &value) == kNvmlSuccess) {
        out.gpu_max_c = static_cast<double>(value);
        any = true;
    }
    // Not supported on this SKU (verified) -> stays -1.
    if (api.device_get_temperature_threshold(device, kNvmlThresholdMemMax, &value) == kNvmlSuccess) {
        out.memory_max_c = static_cast<double>(value);
        any = true;
    }
    out.valid = any;
    return any;
}

// ---------------------------------------------------------------------------
// nvidia-smi fallback backend.
// ---------------------------------------------------------------------------
//
// The field order must stay in sync with kNvidiaSmiQuery below. `nounits` keeps the
// numbers bare: memory is reported in MiB, clocks in MHz, power in W, temperatures in
// degrees Celsius. nvidia-smi does not expose PCIe throughput at all, so those fields
// stay -1 in this backend.
constexpr char kNvidiaSmiQuery[] =
    "index,name,temperature.gpu,temperature.memory,power.draw,power.limit,power.min_limit,"
    "power.max_limit,fan.speed,clocks.current.graphics,clocks.current.memory,memory.used,"
    "memory.total,utilization.gpu,utilization.memory,clocks_throttle_reasons.active";

enum SmiField {
    kSmiIndex = 0,
    kSmiName,
    kSmiTemperature,
    kSmiMemoryTemperature,
    kSmiPowerDraw,
    kSmiPowerLimit,
    kSmiPowerMin,
    kSmiPowerMax,
    kSmiFan,
    kSmiClockCore,
    kSmiClockMemory,
    kSmiMemoryUsed,
    kSmiMemoryTotal,
    kSmiUtilizationGpu,
    kSmiUtilizationMemory,
    kSmiThrottle,
    kSmiFieldCount
};

bool run_nvidia_smi(const std::string& arguments, std::string& output, DWORD& exit_code, std::string& error) {
    std::wstring command = L"\"" + g_state.smi_path + L"\" ";
    command += widen_ascii(arguments);
    return run_process_capture(command, kCommandTimeoutMs, output, exit_code, error);
}

bool nvidia_smi_probe(std::string& error) {
    std::string output;
    DWORD exit_code = 0;
    if (run_nvidia_smi("--query-gpu=index,name --format=csv,noheader,nounits", output, exit_code, error) == false) {
        return false;
    }
    if (exit_code != 0) {
        error = "nvidia-smi exited with code " + std::to_string(exit_code) + ": " + trim(output);
        return false;
    }
    std::vector<std::string> lines;
    for (const std::string& line : split(output, '\n')) {
        if (line.empty() == false) {
            lines.push_back(line);
        }
    }
    if (lines.empty()) {
        error = "nvidia-smi reported no GPUs";
        return false;
    }

    g_state.handles.clear();
    g_state.names.clear();
    g_state.cached_min_w.clear();
    g_state.cached_max_w.clear();

    for (size_t i = 0; i < lines.size() && i < kMaxDevices; ++i) {
        const std::vector<std::string> fields = split(lines[i], ',');
        g_state.handles.push_back(nullptr);
        g_state.names.push_back(fields.size() > 1 ? fields[1] : std::string());
        g_state.cached_min_w.push_back(-1.0);
        g_state.cached_max_w.push_back(-1.0);
    }

    // Cache the static power-limit range once; it does not change while running.
    std::string range_output;
    DWORD range_code = 0;
    std::string range_error;
    if (run_nvidia_smi("--query-gpu=index,power.min_limit,power.max_limit --format=csv,noheader,nounits",
                       range_output, range_code, range_error) &&
        range_code == 0) {
        for (const std::string& line : split(range_output, '\n')) {
            if (line.empty()) {
                continue;
            }
            const std::vector<std::string> fields = split(line, ',');
            if (fields.size() < 3) {
                continue;
            }
            double index_value = 0.0;
            double min_w = 0.0;
            double max_w = 0.0;
            if (parse_number(fields[0], index_value) == false) {
                continue;
            }
            const size_t index = static_cast<size_t>(index_value);
            if (index >= g_state.cached_min_w.size()) {
                continue;
            }
            if (parse_number(fields[1], min_w)) {
                g_state.cached_min_w[index] = min_w;
            }
            if (parse_number(fields[2], max_w)) {
                g_state.cached_max_w[index] = max_w;
            }
        }
    }
    return true;
}

void read_nvidia_smi(unsigned index, GpuTelemetry& out, std::string& error, bool& ok) {
    ok = false;
    out = GpuTelemetry{};
    out.index = index;

    const std::string arguments = "-i " + std::to_string(index) + " --query-gpu=" + kNvidiaSmiQuery +
                                  " --format=csv,noheader,nounits";
    std::string output;
    DWORD exit_code = 0;
    if (run_nvidia_smi(arguments, output, exit_code, error) == false) {
        return;
    }
    if (exit_code != 0) {
        error = "nvidia-smi exited with code " + std::to_string(exit_code) + ": " + trim(output);
        return;
    }

    std::vector<std::string> lines;
    for (const std::string& line : split(output, '\n')) {
        if (line.empty() == false) {
            lines.push_back(line);
        }
    }
    if (lines.empty()) {
        error = "nvidia-smi returned no data for GPU " + std::to_string(index);
        return;
    }

    const std::vector<std::string> fields = split(lines.front(), ',');
    if (fields.size() < static_cast<size_t>(kSmiFieldCount)) {
        error = "nvidia-smi returned " + std::to_string(fields.size()) + " fields, expected " +
                std::to_string(static_cast<int>(kSmiFieldCount));
        return;
    }

    out.name = fields[kSmiName];
    if (index < g_state.cached_min_w.size()) {
        out.power_min_w = g_state.cached_min_w[index];
        out.power_max_w = g_state.cached_max_w[index];
    }

    double value = 0.0;
    if (parse_number(fields[kSmiTemperature], value)) {
        out.temperature_c = value;
    }
    if (parse_number(fields[kSmiMemoryTemperature], value)) {
        out.memory_temperature_c = value;
    }
    if (parse_number(fields[kSmiPowerDraw], value)) {
        out.power_w = value;
    }
    if (parse_number(fields[kSmiPowerLimit], value)) {
        out.power_limit_w = value;
    }
    if (parse_number(fields[kSmiPowerMin], value)) {
        out.power_min_w = value;
    }
    if (parse_number(fields[kSmiPowerMax], value)) {
        out.power_max_w = value;
    }
    if (parse_number(fields[kSmiFan], value)) {
        out.fan_percent = value;
    }
    if (parse_number(fields[kSmiClockCore], value)) {
        out.core_clock_mhz = value;
    }
    if (parse_number(fields[kSmiClockMemory], value)) {
        out.memory_clock_mhz = value;
    }
    if (parse_number(fields[kSmiMemoryUsed], value)) {
        // nvidia-smi reports memory in MiB when `nounits` is used.
        out.memory_used_bytes = static_cast<uint64_t>(value * 1024.0 * 1024.0);
    }
    if (parse_number(fields[kSmiMemoryTotal], value)) {
        out.memory_total_bytes = static_cast<uint64_t>(value * 1024.0 * 1024.0);
    }
    if (parse_number(fields[kSmiUtilizationGpu], value)) {
        out.utilization_gpu_percent = value;
    }
    if (parse_number(fields[kSmiUtilizationMemory], value)) {
        out.utilization_memory_percent = value;
    }

    const std::string throttle = trim(fields[kSmiThrottle]);
    if (throttle.empty() == false && throttle != "N/A" && throttle.front() != '[') {
        char* end = nullptr;
        const unsigned long long reasons = std::strtoull(throttle.c_str(), &end, 16);
        if (end != throttle.c_str()) {
            out.throttle_reasons = static_cast<uint64_t>(reasons);
        }
    }

    out.timestamp_unix_ms = now_unix_ms();
    out.valid = (out.temperature_c > 0.0) || (out.power_w > 0.0) || (out.memory_total_bytes > 0);
    ok = true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Throttle-reason decoding (public API).
// ---------------------------------------------------------------------------
std::vector<std::string> throttle_reason_words(uint64_t throttle_reasons) {
    // Bit values: see the table in telemetry.hpp. Order is chosen so that the
    // mining-relevant limiters come first.
    static const struct {
        uint64_t    bit;
        const char* word;
    } kReasons[] = {
        {0x004ULL, "sw_power_cap"},
        {0x008ULL, "hw_slowdown"},
        {0x040ULL, "hw_thermal_slowdown"},
        {0x080ULL, "hw_power_brake_slowdown"},
        {0x020ULL, "sw_thermal_slowdown"},
        {0x010ULL, "sync_boost"},
        {0x001ULL, "gpu_idle"},
        {0x002ULL, "applications_clocks_setting"},
        {0x100ULL, "display_clock_setting"},
        {0x200ULL, "board_limit"},
        {0x400ULL, "reliability"},
    };

    std::vector<std::string> words;
    if (throttle_reasons == 0) {
        words.emplace_back("none");
        return words;
    }
    uint64_t remaining = throttle_reasons;
    for (const auto& reason : kReasons) {
        if ((throttle_reasons & reason.bit) != 0) {
            words.emplace_back(reason.word);
            remaining &= ~reason.bit;
        }
    }
    if (remaining != 0) {
        // Forward compatibility: report unknown bits verbatim instead of hiding them.
        words.push_back("unknown_0x" + std::to_string(remaining));
    }
    return words;
}

std::string decode_throttle_reasons(uint64_t throttle_reasons) {
    const std::vector<std::string> words = throttle_reason_words(throttle_reasons);
    std::string text;
    for (const std::string& word : words) {
        if (text.empty() == false) {
            text += '|';
        }
        text += word;
    }
    return text;
}

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------
bool Telemetry::init(std::string& error) {
    try {
        std::unique_lock<std::shared_mutex> lock(g_state.mutex);
        if (g_state.initialised) {
            if (g_state.ready) {
                return true;
            }
            error = "telemetry backend unavailable (initialisation was already attempted)";
            return false;
        }
        g_state.initialised = true;
        error.clear();

        std::string nvml_failure;
        NvmlApi api = load_nvml();
        if (api.usable()) {
            const NvmlReturn result = api.init();
            if (result == kNvmlSuccess) {
                unsigned count = 0;
                if (api.device_get_count(&count) == kNvmlSuccess && count > 0) {
                    if (count > kMaxDevices) {
                        count = kMaxDevices;
                    }
                    bool enumeration_ok = true;
                    for (unsigned index = 0; index < count; ++index) {
                        NvmlDevice device = nullptr;
                        if (api.device_get_handle_by_index(index, &device) != kNvmlSuccess || device == nullptr) {
                            enumeration_ok = false;
                            nvml_failure = "nvmlDeviceGetHandleByIndex_v2 failed for GPU " + std::to_string(index);
                            break;
                        }
                        g_state.handles.push_back(device);

                        std::string name;
                        if (api.device_get_name != nullptr) {
                            char buffer[96] = {};
                            if (api.device_get_name(device, buffer, sizeof(buffer)) == kNvmlSuccess) {
                                name.assign(buffer);
                            }
                        }
                        g_state.names.push_back(name);

                        double min_w = -1.0;
                        double max_w = -1.0;
                        if (read_power_constraints(api, device, min_w, max_w) == false) {
                            min_w = -1.0;
                            max_w = -1.0;
                        }
                        g_state.cached_min_w.push_back(min_w);
                        g_state.cached_max_w.push_back(max_w);
                    }
                    if (enumeration_ok) {
                        g_state.nvml = api;
                        // Record the driver this run is bound to: every result in the
                        // project is tied to a specific driver version, so the log
                        // should name it instead of relying on documentation.
                        if (api.system_get_driver_version != nullptr) {
                            char driver[80] = {0};
                            if (api.system_get_driver_version(driver, sizeof(driver)) == 0) {
                                g_state.driver_version.assign(driver);
                            }
                        }
                        g_state.backend = Backend::Nvml;
                        g_state.ready = true;
                        return true;
                    }
                    g_state.handles.clear();
                    g_state.names.clear();
                    g_state.cached_min_w.clear();
                    g_state.cached_max_w.clear();
                    (void)api.shutdown();
                } else {
                    nvml_failure = "nvmlDeviceGetCount_v2 reported no devices";
                    (void)api.shutdown();
                }
            } else {
                nvml_failure = "nvmlInit_v2 failed: " + nvml_error_text(api, result);
            }
            if (api.module != nullptr) {
                FreeLibrary(api.module);
            }
        } else {
            nvml_failure = "nvml.dll could not be loaded or does not export the required entry points";
            if (api.module != nullptr) {
                FreeLibrary(api.module);
            }
        }

        // Fall back to parsing nvidia-smi output.
        g_state.smi_path = locate_system_executable(L"nvidia-smi.exe");
        std::string smi_failure;
        if (nvidia_smi_probe(smi_failure)) {
            g_state.backend = Backend::NvidiaSmi;
            g_state.ready = true;
            return true;
        }

        g_state.backend = Backend::None;
        error = "no usable GPU telemetry backend - NVML: " + nvml_failure + "; nvidia-smi: " + smi_failure;
        return false;
    } catch (...) {
        error = "unexpected exception while initialising telemetry";
        return false;
    }
}

void Telemetry::shutdown() {
    try {
        std::unique_lock<std::shared_mutex> lock(g_state.mutex);
        if (g_state.backend == Backend::Nvml && g_state.nvml.usable()) {
            (void)g_state.nvml.shutdown();
            if (g_state.nvml.module != nullptr) {
                FreeLibrary(g_state.nvml.module);
            }
        }
        g_state.nvml = NvmlApi{};
        g_state.handles.clear();
        g_state.names.clear();
        g_state.cached_min_w.clear();
        g_state.cached_max_w.clear();
        g_state.smi_cache_stamp_ms = 0;
        g_state.smi_cache_index = 0;
        g_state.smi_cache_value = GpuTelemetry{};
        g_state.smi_path.clear();
        g_state.driver_version.clear();
        g_state.backend = Backend::None;
        g_state.ready = false;
        g_state.initialised = false;
    } catch (...) {
        // shutdown() never propagates; on failure the previous state is kept.
    }
}

size_t Telemetry::device_count() {
    try {
        std::shared_lock<std::shared_mutex> lock(g_state.mutex);
        return g_state.ready ? g_state.handles.size() : 0U;
    } catch (...) {
        return 0U;
    }
}

bool Telemetry::read(unsigned index, GpuTelemetry& out, std::string& error) {
    try {
        std::shared_lock<std::shared_mutex> lock(g_state.mutex);
        error.clear();
        out = GpuTelemetry{};
        if (g_state.ready == false) {
            error = "telemetry is not initialised";
            return false;
        }
        if (index >= g_state.handles.size()) {
            error = "GPU index " + std::to_string(index) + " is out of range (device count " +
                    std::to_string(g_state.handles.size()) + ")";
            return false;
        }
        if (g_state.backend == Backend::Nvml) {
            read_nvml(g_state.nvml, g_state.handles[index], index, out);
            if (out.name.empty() && index < g_state.names.size()) {
                out.name = g_state.names[index];
            }
            return true;
        }
        if (g_state.backend == Backend::NvidiaSmi) {
            // Rate-limit the fallback: one process spawn per kNvidiaSmiCacheTtlMs per index.
            const uint64_t now = GetTickCount64();
            if (g_state.smi_cache_stamp_ms != 0 && g_state.smi_cache_index == index &&
                now - g_state.smi_cache_stamp_ms < kNvidiaSmiCacheTtlMs) {
                out = g_state.smi_cache_value;
                return true;
            }
            bool ok = false;
            read_nvidia_smi(index, out, error, ok);
            if (ok) {
                g_state.smi_cache_stamp_ms = now;
                g_state.smi_cache_index = index;
                g_state.smi_cache_value = out;
            }
            return ok;
        }
        error = "telemetry is not initialised";
        return false;
    } catch (...) {
        error = "unexpected exception while reading telemetry";
        return false;
    }
}

bool Telemetry::read_thermal_limits(unsigned index, ThermalLimits& out, std::string& error) {
    try {
        std::shared_lock<std::shared_mutex> lock(g_state.mutex);
        error.clear();
        out = ThermalLimits{};
        if (g_state.ready == false) {
            error = "telemetry is not initialised";
            return false;
        }
        if (index >= g_state.handles.size()) {
            error = "GPU index " + std::to_string(index) + " is out of range (device count " +
                    std::to_string(g_state.handles.size()) + ")";
            return false;
        }
        if (g_state.backend != Backend::Nvml) {
            // Note: do not call backend_name() here - it would take the shared lock
            // again while this thread already holds it (deadlock risk with a waiting
            // writer), so the name is derived from the state directly.
            const char* name = (g_state.backend == Backend::NvidiaSmi) ? "nvidia-smi" : "none";
            error = std::string("temperature thresholds are only available through the NVML backend (active backend: ") +
                    name + ")";
            return false;
        }
        if (read_nvml_thermal_limits(g_state.nvml, g_state.handles[index], out) == false) {
            error = "nvmlDeviceGetTemperatureThreshold is not supported on this device";
            out = ThermalLimits{};
            return false;
        }
        return true;
    } catch (...) {
        error = "unexpected exception while reading thermal limits";
        return false;
    }
}

std::string Telemetry::driver_version() {
    try {
        std::shared_lock<std::shared_mutex> lock(g_state.mutex);
        return g_state.driver_version;
    } catch (...) {
        return std::string();
    }
}

const char* Telemetry::backend_name() {    try {
        std::shared_lock<std::shared_mutex> lock(g_state.mutex);
        switch (g_state.backend) {
            case Backend::Nvml:
                return "nvml";
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

}  // namespace grin
