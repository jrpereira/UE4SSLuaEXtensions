#pragma once
// Product version of UE4SSLuaLogBridge. API compatibility is versioned separately
// (LogBridgeNativeContract.hpp api_version).
#define UE4SSLLB_VERSION "1.0.0"
#define UE4SSLLB_VERSION_MAJOR 1
#define UE4SSLLB_VERSION_MINOR 0
#define UE4SSLLB_VERSION_PATCH 0
#define UE4SSLLB_WIDEN_IMPL(value) L##value
#define UE4SSLLB_WIDEN(value) UE4SSLLB_WIDEN_IMPL(value)
