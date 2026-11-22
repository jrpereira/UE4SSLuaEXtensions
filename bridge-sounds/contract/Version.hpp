#pragma once
// Product version of UE4SSLuaSoundBridge. API compatibility is versioned separately
// (SoundBridgeNativeContract.hpp api_version).
#define UE4SSLSB_VERSION "1.2.0"
#define UE4SSLSB_VERSION_MAJOR 1
#define UE4SSLSB_VERSION_MINOR 2
#define UE4SSLSB_VERSION_PATCH 0
#define UE4SSLSB_WIDEN_IMPL(value) L##value
#define UE4SSLSB_WIDEN(value) UE4SSLSB_WIDEN_IMPL(value)
