/* Run on Windows: clean probes, symbol lookup and actual code/IAT mutation. */
#include "../../runtime/internal.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "failed at line %d: %s\n", __LINE__, #expr); return 1; \
} } while (0)

__declspec(dllexport) int kagura_test_export(void);
__declspec(dllexport) int kagura_test_export(void) { return 42; }

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
static DWORD WINAPI hardware_probe(LPVOID unused) {
    (void)unused;
    return (DWORD)kagura_check_hw_breakpoints();
}
#endif

static ULONG_PTR *debugger_import(void) {
    unsigned char *base = (unsigned char *)GetModuleHandleW(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    IMAGE_IMPORT_DESCRIPTOR *imports = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva);
    for (; imports->Name; ++imports) {
        IMAGE_THUNK_DATA *names, *slots;
        if (!imports->OriginalFirstThunk)
            continue;
        names = (IMAGE_THUNK_DATA *)(base + imports->OriginalFirstThunk);
        slots = (IMAGE_THUNK_DATA *)(base + imports->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            IMAGE_IMPORT_BY_NAME *name;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.AddressOfData))
                continue;
            name = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)name->Name, "IsDebuggerPresent") == 0)
                return &slots->u1.Function;
        }
    }
    return NULL;
}

int main(void) {
    unsigned char *api = (unsigned char *)(uintptr_t)GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "IsDebuggerPresent");
    unsigned char saved[16];
    DWORD previous, ignored;
    ULONG_PTR *slot, original;
    int detected;
    volatile BOOL debugger = IsDebuggerPresent(); /* Keep a real IAT entry. */
    CHECK(!debugger);
    CHECK(kagura_check_tracer_pid() == 0);
    CHECK(kagura_check_inline_hooks() == 0);
    CHECK(kagura_check_got_hooks() == 0);
    CHECK(kagura_check_sw_breakpoints() == 0);
    CHECK(kagura_check_hw_breakpoints() == 0);
    CHECK(kagura_check_emulator() == 0);
    CHECK(kagura_windows_resolve_symbol("GetCurrentProcessId", NULL) ==
          (void *)(uintptr_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentProcessId"));
    CHECK(kagura_windows_resolve_symbol("kagura_test_export", NULL) ==
          (void *)(uintptr_t)&kagura_test_export);

#if defined(_M_X64) || defined(__x86_64__)
    CHECK(api != NULL);
    memcpy(saved, api, sizeof(saved));
    CHECK(VirtualProtect(api, sizeof(saved), PAGE_EXECUTE_READWRITE, &previous));
    api[0] = 0xcc;
    FlushInstructionCache(GetCurrentProcess(), api, sizeof(saved));
    detected = kagura_check_sw_breakpoints();
    memcpy(api, saved, sizeof(saved));
    VirtualProtect(api, sizeof(saved), previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), api, sizeof(saved));
    CHECK(detected == 1);
    CHECK(kagura_check_sw_breakpoints() == 0);

    CHECK(VirtualProtect(api, sizeof(saved), PAGE_EXECUTE_READWRITE, &previous));
    /* jmp [rip+0] into our application, outside the system DLLs. */
    api[0] = 0xff; api[1] = 0x25;
    memset(api + 2, 0, 4);
    { void *target = (void *)(uintptr_t)&kagura_test_export;
      memcpy(api + 6, &target, sizeof(target)); }
    FlushInstructionCache(GetCurrentProcess(), api, sizeof(saved));
    detected = kagura_check_inline_hooks();
    memcpy(api, saved, sizeof(saved));
    VirtualProtect(api, sizeof(saved), previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), api, sizeof(saved));
    CHECK(detected == 1);
    CHECK(kagura_check_inline_hooks() == 0);
#endif

    /* An export in the executable must not override an import-library alias
     * belonging to another loaded module. */
    { void *imported = (void *)(uintptr_t)GetProcAddress(
          GetModuleHandleW(L"kernel32.dll"), "GetCurrentProcessId");
      CHECK(kagura_windows_resolve_symbol("kagura_test_export", imported) == imported); }

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    /* Set a real debug register on a suspended worker. Its address is never
     * executed, so detection must inspect the register rather than a trap. */
    { CONTEXT context;
      DWORD result;
      HANDLE thread = CreateThread(NULL, 0, hardware_probe, NULL, CREATE_SUSPENDED, NULL);
      CHECK(thread != NULL);
      memset(&context, 0, sizeof(context));
      context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      CHECK(GetThreadContext(thread, &context));
      context.Dr0 = (DWORD_PTR)&kagura_test_export;
      context.Dr7 = (context.Dr7 & ~(DWORD_PTR)0xffU) | 1U;
      CHECK(SetThreadContext(thread, &context));
      CHECK(ResumeThread(thread) != (DWORD)-1);
      CHECK(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
      CHECK(GetExitCodeThread(thread, &result));
      CloseHandle(thread);
      CHECK(result == 1); }
#endif

    slot = debugger_import();
    CHECK(slot != NULL);
    original = *slot;
    CHECK(VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &previous));
    *slot = (ULONG_PTR)&kagura_test_export;
    detected = kagura_check_got_hooks();
    *slot = original;
    VirtualProtect(slot, sizeof(*slot), previous, &ignored);
    CHECK(detected == 1);
    CHECK(kagura_check_got_hooks() == 0);
    return 0;
}
