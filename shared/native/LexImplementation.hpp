#pragma once

// Exports the descriptor the bootstrap reads (shared/contract/ImplementationABI.h).
// Use once per implementation DLL, at namespace scope, after the start and
// uninstall functions:
//
//   LEX_EXPORT_IMPLEMENTATION(MAJOR, MINOR, PATCH, &start, &uninstall)
//
// The magic comes from UE4SSLEB_IMPLEMENTATION_MAGIC, which the product's
// build defines; the layout, ABI version and UE4SS commit are shared.

#include <ImplementationABI.h>

#define LEX_EXPORT_IMPLEMENTATION(major, minor, patch, start, uninstall)                       \
    namespace                                                                                  \
    {                                                                                          \
    const UE4SSLEB_ImplementationV1 lex_implementation_descriptor{                             \
        sizeof(UE4SSLEB_ImplementationV1), UE4SSLEB_IMPLEMENTATION_MAGIC,                      \
        UE4SSLEB_IMPLEMENTATION_ABI, UE4SSLEB_TARGET_UE4SS_COMMIT, (major), (minor), (patch),  \
        (start), (uninstall)};                                                                 \
    }                                                                                          \
    extern "C" __declspec(dllexport) const UE4SSLEB_ImplementationV1* UE4SSLEB_GetImplementationV1() \
    {                                                                                          \
        return &lex_implementation_descriptor;                                                 \
    }
