// Ported from Magpie experimental DLSSNRFilter.cpp (see iat_hook.h).
#include "iat_hook.h"

#include <atomic>
#include <cstring>

namespace vsdlssnr {

namespace {

std::atomic<void *> g_hookOwner{ nullptr };
std::atomic<HMODULE> g_snippetCallerModule{ nullptr };

using GetModuleFileNameWFn = DWORD(WINAPI *)(HMODULE, LPWSTR, DWORD);
std::atomic<GetModuleFileNameWFn> g_originalGetModuleFileNameW{ nullptr };

constexpr wchar_t AUTHORIZED_CALLER[] = L"nvngx.dll";
constexpr DWORD AUTHORIZED_CALLER_LENGTH = ARRAYSIZE(AUTHORIZED_CALLER) - 1;

DWORD WINAPI HookedGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size) noexcept {
    if (module == g_snippetCallerModule.load(std::memory_order_acquire)) {
        if (!filename || !size) {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        if (size <= AUTHORIZED_CALLER_LENGTH) {
            if (size > 1) {
                std::memcpy(filename, AUTHORIZED_CALLER, (size - 1) * sizeof(wchar_t));
            }
            filename[size - 1] = L'\0';
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return size;
        }
        std::memcpy(filename, AUTHORIZED_CALLER, sizeof(AUTHORIZED_CALLER));
        return AUTHORIZED_CALLER_LENGTH;
    }

    const auto original = g_originalGetModuleFileNameW.load(std::memory_order_acquire);
    if (original) return original(module, filename, size);
    SetLastError(ERROR_INVALID_FUNCTION);
    return 0;
}

template <typename T>
void *FunctionAddress(T function) noexcept {
    void *result = nullptr;
    static_assert(sizeof(function) == sizeof(result));
    std::memcpy(&result, &function, sizeof(result));
    return result;
}

void **FindImportedFunctionSlot(HMODULE module, const char *functionName) noexcept {
    if (!module || !functionName) return nullptr;
    auto *base = reinterpret_cast<std::byte *>(module);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return nullptr;
    }

    const auto &directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !directory.Size ||
        directory.VirtualAddress >= nt->OptionalHeader.SizeOfImage ||
        directory.Size > nt->OptionalHeader.SizeOfImage ||
        directory.VirtualAddress > nt->OptionalHeader.SizeOfImage - directory.Size) {
        return nullptr;
    }

    auto *descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + directory.VirtualAddress);
    const auto *descriptorEnd = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(
        base + directory.VirtualAddress + directory.Size);
    for (; descriptor < descriptorEnd && descriptor->Name; ++descriptor) {
        if (descriptor->Name >= nt->OptionalHeader.SizeOfImage) continue;
        const char *libraryName = reinterpret_cast<const char *>(base + descriptor->Name);
        if (_stricmp(libraryName, "KERNEL32.dll") != 0 &&
            _stricmp(libraryName, "api-ms-win-core-libraryloader-l1-2-0.dll") != 0 &&
            _stricmp(libraryName, "api-ms-win-core-libraryloader-l1-1-0.dll") != 0) {
            continue;
        }
        if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) continue;
        auto *nameThunk = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + descriptor->OriginalFirstThunk);
        auto *addressThunk = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + descriptor->FirstThunk);
        for (; nameThunk->u1.AddressOfData; ++nameThunk, ++addressThunk) {
            if (IMAGE_SNAP_BY_ORDINAL64(nameThunk->u1.Ordinal)) continue;
            const uint32_t nameRva = static_cast<uint32_t>(nameThunk->u1.AddressOfData);
            if (nameRva >= nt->OptionalHeader.SizeOfImage) return nullptr;
            const auto *import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base + nameRva);
            if (std::strcmp(reinterpret_cast<const char *>(import->Name), functionName) == 0) {
                return reinterpret_cast<void **>(&addressThunk->u1.Function);
            }
        }
    }
    return nullptr;
}

} // namespace

bool InstallSnippetCallerHook(HMODULE snippetModule, SnippetCallerHook &hook) noexcept {
    hook = {};
    hook.iatSlot = FindImportedFunctionSlot(snippetModule, "GetModuleFileNameW");
    if (!hook.iatSlot) return false;

    void *expectedOwner = nullptr;
    if (!g_hookOwner.compare_exchange_strong(
            expectedOwner, &hook, std::memory_order_acq_rel)) {
        return false; // another live context already owns the hook
    }

    void *hookAddress = FunctionAddress(&HookedGetModuleFileNameW);
    HMODULE callerModule = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(hookAddress), &callerModule)) {
        g_hookOwner.store(nullptr, std::memory_order_release);
        return false;
    }

    DWORD oldProtection = 0;
    if (!VirtualProtect(hook.iatSlot, sizeof(void *), PAGE_READWRITE, &oldProtection)) {
        g_hookOwner.store(nullptr, std::memory_order_release);
        return false;
    }

    g_snippetCallerModule.store(callerModule, std::memory_order_release);
    void *original = InterlockedExchangePointer(
        reinterpret_cast<void *volatile *>(hook.iatSlot), hookAddress);
    GetModuleFileNameWFn originalFunction = nullptr;
    std::memcpy(&originalFunction, &original, sizeof(original));
    g_originalGetModuleFileNameW.store(originalFunction, std::memory_order_release);
    hook.installed = true;

    DWORD ignoredProtection = 0;
    VirtualProtect(hook.iatSlot, sizeof(void *), oldProtection, &ignoredProtection);
    FlushInstructionCache(GetCurrentProcess(), hook.iatSlot, sizeof(void *));

    if (!originalFunction) {
        // Null import: the hook cannot work — restore the slot and release
        // the owner right here. The Initialize caller fails without calling
        // RestoreSnippetCallerHook, so leaving the hook armed would serve
        // ERROR_INVALID_FUNCTION from this IAT slot for the whole session
        // and block every future context from installing.
        InterlockedExchangePointer(
            reinterpret_cast<void *volatile *>(hook.iatSlot), original);
        g_originalGetModuleFileNameW.store(nullptr, std::memory_order_release);
        g_snippetCallerModule.store(nullptr, std::memory_order_release);
        hook.installed = false;
        hook.iatSlot = nullptr;
        void *owner = &hook;
        g_hookOwner.compare_exchange_strong(owner, nullptr, std::memory_order_acq_rel);
        return false;
    }
    return true;
}

bool RestoreSnippetCallerHook(SnippetCallerHook &hook) noexcept {
    if (!hook.installed) return true;
    const auto original = g_originalGetModuleFileNameW.load(std::memory_order_acquire);
    DWORD oldProtection = 0;
    if (!hook.iatSlot || !VirtualProtect(hook.iatSlot, sizeof(void *), PAGE_READWRITE, &oldProtection)) {
        return false;
    }

    InterlockedExchangePointer(
        reinterpret_cast<void *volatile *>(hook.iatSlot), FunctionAddress(original));
    DWORD ignoredProtection = 0;
    VirtualProtect(hook.iatSlot, sizeof(void *), oldProtection, &ignoredProtection);
    FlushInstructionCache(GetCurrentProcess(), hook.iatSlot, sizeof(void *));

    hook.installed = false;
    hook.iatSlot = nullptr;
    g_originalGetModuleFileNameW.store(nullptr, std::memory_order_release);
    g_snippetCallerModule.store(nullptr, std::memory_order_release);
    void *expectedOwner = &hook;
    g_hookOwner.compare_exchange_strong(expectedOwner, nullptr, std::memory_order_acq_rel);
    return true;
}

} // namespace vsdlssnr
