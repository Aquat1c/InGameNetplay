#pragma once

#include <stdint.h>
#include <windows.h>

typedef struct EfzOverlayResetApiV1 {
    uint32_t size;
    uint32_t version;
    HRESULT (__cdecl *beforeReset)(void* device, uint32_t epoch);
    HRESULT (__cdecl *afterReset)(void* device, uint32_t epoch, uint32_t ready);
} EfzOverlayResetApiV1;

// Wrong device/epoch/thread and concurrent/reentrant work fail without waiting.
#ifdef __cplusplus
extern "C" {
#endif
__declspec(dllexport) const EfzOverlayResetApiV1* __cdecl EFZ_QueryOverlayResetApiV1(void);
#ifdef __cplusplus
}
#if defined(_WIN32) && !defined(_WIN64)
static_assert(sizeof(EfzOverlayResetApiV1) == 16, "overlay reset ABI must be 16 bytes on x86");
#endif
#endif

#if defined(_MSC_VER) && defined(_M_IX86)
#pragma comment(linker, "/EXPORT:EFZ_QueryOverlayResetApiV1=_EFZ_QueryOverlayResetApiV1")
#endif
