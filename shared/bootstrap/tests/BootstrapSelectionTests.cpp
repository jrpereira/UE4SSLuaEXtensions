#include <CandidateDiscovery.hpp>
#include <LoaderConfig.hpp>

#include <cassert>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

int main()
{
    assert(parse_semantic_version("1.0.7") == SemanticVersion({1, 0, 7}));
    assert(!parse_semantic_version("1.0"));
    assert(!parse_semantic_version("01.0.7"));
    assert(!parse_semantic_version("1.0.7-rc"));

    const auto missing = parse_loader_config(std::nullopt);
    assert(missing.valid && !missing.exact_version);

    constexpr std::string_view automatic_source = R"({"schema":1,"version":"auto"})";
    const auto automatic = parse_loader_config(automatic_source);
    assert(automatic.valid && !automatic.exact_version);
    const auto bom = parse_loader_config(std::string_view("\xEF\xBB\xBF{\"schema\":1,\"version\":\"auto\"}"));
    assert(bom.valid && !bom.exact_version);

    constexpr std::string_view exact_source = R"({ "version": "1.0.7", "schema": 1 })";
    const auto exact = parse_loader_config(exact_source);
    assert(exact.valid && exact.exact_version == SemanticVersion({1, 0, 7}));

    for (const auto invalid : {
             R"({"schema":2,"version":"1.0.7"})",
             R"({"schema":1,"version":"latest"})",
             R"({"schema":1,"version":"1.0.7",})",
             R"({"schema":1,"version":"1.0.7","path":"other.dll"})",
             R"({"schema":1,"schema":1,"version":"1.0.7"})"})
    {
        assert(!parse_loader_config(std::string_view(invalid)).valid);
    }

    const std::vector<BootstrapCandidate> candidates{
        {{1, 0, 6}, "one.dll"},
        {{1, 0, 7}, "two.dll"},
        {{1, 1, 0}, "three.dll"},
    };
    const auto* newest = select_candidate(candidates, std::nullopt);
    assert(newest && newest->version == SemanticVersion({1, 1, 0}));
    const auto* pinned = select_candidate(candidates, SemanticVersion{1, 0, 7});
    assert(pinned && pinned->path == "two.dll");
    assert(!select_candidate(candidates, SemanticVersion{2, 0, 0}));

    // Automatic selection falls back through every version, newest first.
    const auto fallback = candidate_order(candidates, std::nullopt);
    assert(fallback.size() == 3 && fallback[0]->path == "three.dll" &&
           fallback[1]->path == "two.dll" && fallback[2]->path == "one.dll");
    // An exact pin never falls back.
    const auto exact_order = candidate_order(candidates, SemanticVersion{1, 0, 7});
    assert(exact_order.size() == 1 && exact_order[0]->path == "two.dll");
    assert(candidate_order(candidates, SemanticVersion{2, 0, 0}).empty());
    assert(candidate_order({}, std::nullopt).empty());
    assert(version_text({1, 0, 8}) == "1.0.8");

    // The event bridge's identity, as bridge-events/CMakeLists.txt passes it.
    constexpr BridgeProduct events{L"UE4SSLuaEventBridge", L"UE4SSLEB", UINT32_C(0x4C454231)};
    // A second product with the default claim (UE4SSXB-<product>).
    constexpr BridgeProduct files{L"UE4SSLuaFileBridge", L"UE4SSXB-UE4SSLuaFileBridge", UINT32_C(0x4C464231)};

    CandidateMetadata metadata{
        {1, 0, 7}, L"UE4SSLuaEventBridge-1.0.7.dll", L"UE4SSLuaEventBridge-1.0.7.dll",
        L"UE4SSLuaEventBridge", L"Implementation", L"1", L"97b7e501", L"5.5"};
    assert(compatible_candidate(events, metadata));
    metadata.filename = L"ue4ssluaeventbridge-1.0.7.DLL";
    assert(compatible_candidate(events, metadata));
    metadata.filename = L"UE4SSLuaEventBridge-1.0.7.dll";
    metadata.ue4ss_commit = L"other";
    assert(!compatible_candidate(events, metadata));
    metadata.ue4ss_commit = L"97b7e501";
    metadata.original_filename = L"other.dll";
    assert(!compatible_candidate(events, metadata));

    // Today's event bridge names stay exactly as they were.
    assert(candidate_filename(events, {1, 0, 12}) == L"UE4SSLuaEventBridge-1.0.12.dll");
    assert(bootstrap_claim_name(events, 4242) == L"Local\\UE4SSLEB-4242-97b7e501");
    assert(bootstrap_log_prefix(events) == "[UE4SSLuaEventBridge bootstrap] ");

    // Two products side by side: each discovers only its own implementations.
    const CandidateMetadata event_dll{
        {1, 0, 12}, L"UE4SSLuaEventBridge-1.0.12.dll", L"UE4SSLuaEventBridge-1.0.12.dll",
        L"UE4SSLuaEventBridge", L"Implementation", L"1", L"97b7e501", L"5.5"};
    const CandidateMetadata file_dll{
        {0, 1, 0}, L"UE4SSLuaFileBridge-0.1.0.dll", L"UE4SSLuaFileBridge-0.1.0.dll",
        L"UE4SSLuaFileBridge", L"Implementation", L"1", L"97b7e501", L"5.5"};
    assert(compatible_candidate(events, event_dll) && !compatible_candidate(events, file_dll));
    assert(compatible_candidate(files, file_dll) && !compatible_candidate(files, event_dll));

    // A DLL renamed into the other product's prefix, or one whose resource names the
    // other product, is still rejected: file name and ProductName must both match.
    auto renamed = file_dll;
    renamed.filename = L"UE4SSLuaEventBridge-0.1.0.dll";
    renamed.original_filename = L"UE4SSLuaEventBridge-0.1.0.dll";
    assert(!compatible_candidate(events, renamed) && !compatible_candidate(files, renamed));
    auto relabelled = file_dll;
    relabelled.product_name = L"UE4SSLuaEventBridge";
    assert(!compatible_candidate(events, relabelled) && !compatible_candidate(files, relabelled));
    // One product's name is not a prefix match for another's.
    constexpr BridgeProduct shorter{L"UE4SSLuaFile", L"UE4SSXB-UE4SSLuaFile", UINT32_C(0x4C463031)};
    assert(!compatible_candidate(shorter, file_dll));
    assert(!compatible_candidate(BridgeProduct{}, event_dll));

    // Names, claims, log prefixes and magics differ, so the two bootstraps hold
    // independent per-process locks and accept only their own descriptors.
    assert(candidate_filename(events, {0, 1, 0}) != candidate_filename(files, {0, 1, 0}));
    assert(candidate_filename(files, {0, 1, 0}) == L"UE4SSLuaFileBridge-0.1.0.dll");
    assert(bootstrap_claim_name(events, 4242) != bootstrap_claim_name(files, 4242));
    assert(bootstrap_claim_name(files, 4242) == L"Local\\UE4SSXB-UE4SSLuaFileBridge-4242-97b7e501");
    assert(bootstrap_claim_name(files, 4242) != bootstrap_claim_name(files, 4243));
    assert(bootstrap_log_prefix(files) == "[UE4SSLuaFileBridge bootstrap] ");
    assert(bootstrap_log_prefix(events) != bootstrap_log_prefix(files));
    assert(events.magic != files.magic);
}
