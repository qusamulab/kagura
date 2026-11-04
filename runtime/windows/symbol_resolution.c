/* Windows loader counterpart of CallIndirection's POSIX dlsym path. */
#include "../internal.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>

void *kagura_windows_resolve_symbol(const char *name, void *import_address) {
    /* Resolve only from loaded images: loading arbitrary DLL names here would
     * introduce DLL-search-path side effects and change application lifetime. */
    FARPROC address;
    if (import_address) {
        HMODULE module = NULL;
        /* The import operand keeps the linker/loader dependency alive and
         * selects the exact exporting image when DLLs share an export name.
         * Ordinal-only exports and import-library aliases have no name lookup;
         * retain the loader's address for those instead of changing behavior. */
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              (LPCSTR)import_address, &module)) {
            kagura_on_tamper_detected();
            return import_address;
        }
        address = GetProcAddress(module, name);
        return address ? (void *)(uintptr_t)address : import_address;
    }
    address = GetProcAddress(GetModuleHandleW(NULL), name);
    HANDLE snapshot;
    MODULEENTRY32W entry;
    if (address)
        return (void *)(uintptr_t)address;
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                       GetCurrentProcessId());
    if (snapshot != INVALID_HANDLE_VALUE) {
        entry.dwSize = sizeof(entry);
        if (Module32FirstW(snapshot, &entry)) {
            do {
                address = GetProcAddress(entry.hModule, name);
                if (address)
                    break;
            } while (Module32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    if (!address)
        kagura_on_tamper_detected();
    return (void *)(uintptr_t)address;
}
#endif
