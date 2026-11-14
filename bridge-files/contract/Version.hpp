#pragma once
// Product version of UE4SSLuaFileBridge. API compatibility is versioned separately
// (FileBridgeNativeContract.hpp api_version).
#define UE4SSLFB_VERSION "1.1.0"
#define UE4SSLFB_VERSION_MAJOR 1
#define UE4SSLFB_VERSION_MINOR 1
#define UE4SSLFB_VERSION_PATCH 0
#define UE4SSLFB_WIDEN_IMPL(value) L##value
#define UE4SSLFB_WIDEN(value) UE4SSLFB_WIDEN_IMPL(value)
