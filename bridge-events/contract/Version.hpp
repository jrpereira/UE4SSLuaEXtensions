#pragma once
// Shared product version. API compatibility is versioned separately.
#define UE4SSLEB_VERSION "1.2.0"
#define UE4SSLEB_VERSION_MAJOR 1
#define UE4SSLEB_VERSION_MINOR 2
#define UE4SSLEB_VERSION_PATCH 0
#define UE4SSLEB_WIDEN_IMPL(value) L##value
#define UE4SSLEB_WIDEN(value) UE4SSLEB_WIDEN_IMPL(value)
