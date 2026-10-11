#pragma once

#include <stdint.h>

#if defined(_MSC_VER)
#define UE4SSLEB_CALL __cdecl
#else
#define UE4SSLEB_CALL
#endif

/* The descriptor layout and the exported symbol name are shared by every product;
   the magic is per product. A product's implementation build defines
   UE4SSLEB_IMPLEMENTATION_MAGIC, and its bootstrap gets the same value from
   add_bridge_bootstrap(MAGIC). Without a definition it is the event bridge's 'LEB1'. */
#define UE4SSLEB_EVENT_BRIDGE_MAGIC UINT32_C(0x4C454231)
#ifndef UE4SSLEB_IMPLEMENTATION_MAGIC
#define UE4SSLEB_IMPLEMENTATION_MAGIC UE4SSLEB_EVENT_BRIDGE_MAGIC
#endif
#define UE4SSLEB_IMPLEMENTATION_ABI UINT32_C(1)
#define UE4SSLEB_TARGET_UE4SS_COMMIT UINT32_C(0x97b7e501)

typedef void* UE4SSLEB_ModHandle;

typedef enum UE4SSLEB_UninstallResult
{
    UE4SSLEB_UNINSTALL_CAN_UNLOAD = 0,
    UE4SSLEB_UNINSTALL_RETAIN_MODULE = 1,
} UE4SSLEB_UninstallResult;

typedef struct UE4SSLEB_ImplementationV1
{
    uint32_t struct_size;
    uint32_t magic;
    uint32_t abi_version;
    uint32_t ue4ss_commit;
    uint32_t version_major;
    uint32_t version_minor;
    uint32_t version_patch;
    UE4SSLEB_ModHandle (UE4SSLEB_CALL *start)(void);
    UE4SSLEB_UninstallResult (UE4SSLEB_CALL *uninstall)(UE4SSLEB_ModHandle);
} UE4SSLEB_ImplementationV1;

typedef const UE4SSLEB_ImplementationV1* (UE4SSLEB_CALL *UE4SSLEB_GetImplementationV1Fn)(void);

#undef UE4SSLEB_CALL
