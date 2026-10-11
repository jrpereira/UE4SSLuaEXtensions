#define NOMINMAX
#include <windows.h>
#include <winver.h>

#include <BootstrapIdentity.hpp>
#include <CandidateDiscovery.hpp>
#include <ImplementationABI.h>
#include <LoaderConfig.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
HMODULE implementation_module{};
const UE4SSLEB_ImplementationV1* implementation_api{};
std::mutex implementation_mutex;
HANDLE bootstrap_claim{};
int bootstrap_address_marker{};

using UE4SSOutputSend = void (*)(std::wstring_view);

// UE4SS's output (UE4SS.log), resolved at runtime so the bootstrap keeps no
// link-time dependency on a particular UE4SS build.
UE4SSOutputSend ue4ss_output()
{
    static const auto send = [] {
        const auto module = GetModuleHandleW(L"UE4SS.dll");
        const auto address = module
            ? GetProcAddress(module, "?send@Output@RC@@YAXV?$basic_string_view@_WU?$char_traits@_W@std@@@std@@@Z")
            : nullptr;
        return reinterpret_cast<UE4SSOutputSend>(reinterpret_cast<void*>(address));
    }();
    return send;
}

void log(std::string_view message)
{
    std::string line{bootstrap_log_prefix(bootstrap_product)};
    line.append(message);
    line.push_back('\n');
    OutputDebugStringA(line.c_str());
    const auto send = ue4ss_output();
    if (!send) return;
    try
    {
        std::wstring wide;
        wide.reserve(line.size());
        for (const char c : line) wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
        send(wide);
    }
    catch (...)
    {
    }
}

std::filesystem::path bootstrap_file()
{
    HMODULE module{};
    constexpr DWORD from_address = 0x00000004UL;
    constexpr DWORD unchanged_reference_count = 0x00000002UL;
    if (!GetModuleHandleExW(
            from_address | unchanged_reference_count,
            reinterpret_cast<LPCWSTR>(&bootstrap_address_marker),
            &module)) return {};
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    return length && length < path.size() ? std::filesystem::path(std::wstring(path.data(), length))
                                          : std::filesystem::path{};
}

std::optional<std::wstring> resource_string(std::vector<std::byte>& data, const wchar_t* name)
{
    const std::wstring query = std::wstring{L"\\StringFileInfo\\040904B0\\"} + name;
    void* value{};
    UINT size{};
    if (!VerQueryValueW(data.data(), query.c_str(), &value, &size) || !value || size == 0) return {};
    const auto* text = static_cast<const wchar_t*>(value);
    return std::wstring(text, size && text[size - 1] == L'\0' ? size - 1 : size);
}

std::optional<CandidateMetadata> read_metadata(const std::filesystem::path& path)
{
    DWORD ignored{};
    const auto size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) return {};
    std::vector<std::byte> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
    const auto product_version = resource_string(data, L"ProductVersion");
    const auto product_name = resource_string(data, L"ProductName");
    const auto original_filename = resource_string(data, L"OriginalFilename");
    const auto role = resource_string(data, L"BridgeRole");
    const auto implementation_abi = resource_string(data, L"ImplementationABI");
    const auto ue4ss_commit = resource_string(data, L"UE4SSCommit");
    const auto unreal_version = resource_string(data, L"UnrealVersion");
    if (!product_version || !product_name || !original_filename || !role ||
        !implementation_abi || !ue4ss_commit || !unreal_version) return {};
    std::string narrow_version;
    narrow_version.reserve(product_version->size());
    for (const auto character : *product_version)
    {
        if (character > static_cast<wchar_t>(0x7f)) return {};
        narrow_version.push_back(static_cast<char>(character));
    }
    const auto version = parse_semantic_version(narrow_version);
    if (!version) return {};
    return CandidateMetadata{
        *version, path.filename().wstring(), *original_filename, *product_name, *role,
        *implementation_abi, *ue4ss_commit, *unreal_version};
}

bool same_name(const std::wstring& left, const std::wstring& right)
{
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

std::vector<BootstrapCandidate> discover(const std::filesystem::path& directory)
{
    std::vector<BootstrapCandidate> result;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(directory, error), end; !error && iterator != end;
         iterator.increment(error))
    {
        if (iterator->is_symlink(error) || error || !iterator->is_regular_file(error) || error ||
            !same_name(iterator->path().extension().wstring(), L".dll")) continue;
        const auto metadata = read_metadata(iterator->path());
        if (!metadata || !compatible_candidate(bootstrap_product, *metadata)) continue;
        result.push_back({metadata->version, iterator->path()});
    }
    return result;
}

LoaderConfig load_config(const std::filesystem::path& path)
{
    std::error_code error;
    if (!std::filesystem::exists(path, error))
        return error ? LoaderConfig{false, {}, "cannot inspect main.json"}
                     : parse_loader_config(std::nullopt);
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {false, {}, "cannot open main.json"};
    std::string source((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return parse_loader_config(std::string_view(source));
}

bool descriptor_matches(const UE4SSLEB_ImplementationV1* api, const SemanticVersion& version)
{
    return api && api->struct_size >= sizeof(UE4SSLEB_ImplementationV1) &&
           api->magic == bootstrap_product.magic &&
           api->abi_version == UE4SSLEB_IMPLEMENTATION_ABI &&
           api->ue4ss_commit == UE4SSLEB_TARGET_UE4SS_COMMIT && api->start && api->uninstall &&
           api->version_major == version.major && api->version_minor == version.minor &&
           api->version_patch == version.patch;
}
}

#define UE4SSLEB_BOOTSTRAP_API extern "C" __declspec(dllexport)

UE4SSLEB_BOOTSTRAP_API void* start_mod()
{
    std::scoped_lock lock(implementation_mutex);
    if (implementation_module)
    {
        log("start_mod called more than once");
        return nullptr;
    }
    const auto claim_name = bootstrap_claim_name(bootstrap_product, GetCurrentProcessId());
    bootstrap_claim = CreateMutexW(nullptr, FALSE, claim_name.c_str());
    if (!bootstrap_claim || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (bootstrap_claim) CloseHandle(bootstrap_claim);
        bootstrap_claim = nullptr;
        log("another bootstrap instance already owns this process");
        return nullptr;
    }
    const auto file = bootstrap_file();
    if (file.empty())
    {
        log("cannot locate main.dll");
        CloseHandle(bootstrap_claim);
        bootstrap_claim = nullptr;
        return nullptr;
    }
    const auto config = load_config(file.parent_path() / L"main.json");
    if (!config.valid)
    {
        log(config.error);
        CloseHandle(bootstrap_claim);
        bootstrap_claim = nullptr;
        return nullptr;
    }
    const auto candidates = discover(file.parent_path() / L"versions");
    const auto order = candidate_order(candidates, config.exact_version);
    if (order.empty())
    {
        log(config.exact_version ? "configured implementation is unavailable or incompatible"
                                 : "no compatible implementation was discovered");
        CloseHandle(bootstrap_claim);
        bootstrap_claim = nullptr;
        return nullptr;
    }
    for (const auto* candidate : order)
    {
        const auto version = version_text(candidate->version);
        auto* module = LoadLibraryExW(
            candidate->path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module)
        {
            log("implementation " + version + " could not be loaded");
            continue;
        }
        const auto get_api = reinterpret_cast<UE4SSLEB_GetImplementationV1Fn>(
            GetProcAddress(module, "UE4SSLEB_GetImplementationV1"));
        const auto* api = get_api ? get_api() : nullptr;
        if (!descriptor_matches(api, candidate->version))
        {
            log("implementation " + version + " failed its private ABI check");
            FreeLibrary(module);
            continue;
        }
        void* mod{};
        try
        {
            mod = api->start();
            if (!mod) log("implementation " + version + " did not start");
        }
        catch (...)
        {
            log("implementation " + version + " threw during startup");
        }
        if (!mod)
        {
            FreeLibrary(module);
            continue;
        }
        implementation_module = module;
        implementation_api = api;
        log("started implementation " + version +
            (candidate == order.front() ? std::string{} : std::string{" after newer versions failed"}));
        return mod;
    }
    CloseHandle(bootstrap_claim);
    bootstrap_claim = nullptr;
    return nullptr;
}

UE4SSLEB_BOOTSTRAP_API void uninstall_mod(void* mod)
{
    std::scoped_lock lock(implementation_mutex);
    auto* module = implementation_module;
    const auto* api = implementation_api;
    if (!module || !api)
    {
        return;
    }
    UE4SSLEB_UninstallResult result{};
    try
    {
        result = api->uninstall(mod);
    }
    catch (...)
    {
        log("implementation threw during uninstall; retaining its module");
        implementation_api = nullptr;
        return;
    }
    implementation_api = nullptr;
    if (result != UE4SSLEB_UNINSTALL_CAN_UNLOAD)
    {
        log("implementation retained native code; restart the process before selecting another version");
        return;
    }
    implementation_module = nullptr;
    FreeLibrary(module);
    if (bootstrap_claim) CloseHandle(bootstrap_claim);
    bootstrap_claim = nullptr;
}
