#pragma once
// Ported from Magpie experimental src\Magpie.Core\DLSSNRFilter.cpp
// (SnippetGetModuleFileNameW / FindImportedFunctionSlot / Install- and
// RestoreSnippetCallerCompatibility, cpp:565-750).
//
// The signed nvngx_dlssnr.dll snippet only accepts being loaded by the official
// NGX runtime (nvngx.dll). It verifies its caller by asking GetModuleFileNameW
// for its own host module and comparing the result. We satisfy that by rewriting
// the snippet's IAT entry for GetModuleFileNameW so that queries for the snippet
// module itself return L"nvngx.dll".
//
// The hook MUST be installed immediately after LoadLibraryExW of the snippet and
// before any snippet initialization runs (Magpie does the same).

#include <windows.h>
#include <cstring>

namespace vsdlssnr {

struct SnippetCallerHook {
    void **iatSlot = nullptr;
    bool installed = false;
};

// function-pointer → void* via memcpy: the only defined way to inspect a
// function pointer's object representation. Shared by the hook installer and
// the NGX parameter callbacks in dlssnr_context.cpp.
template <typename T>
inline void *FunctionAddress(T function) noexcept {
    void *result = nullptr;
    static_assert(sizeof(function) == sizeof(result));
    std::memcpy(&result, &function, sizeof(result));
    return result;
}

bool InstallSnippetCallerHook(HMODULE snippetModule, SnippetCallerHook &hook) noexcept;
bool RestoreSnippetCallerHook(SnippetCallerHook &hook) noexcept;

} // namespace vsdlssnr
