/* Entry points emitted by AntiDebug.cpp. Windows probes stay separate from
 * the POSIX implementation; a clean result means no supported probe fired. */
#include "../internal.h"

#ifdef _WIN32
#include <windows.h>
#include <string.h>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif

static int readable(const void *ptr, size_t len) {
    MEMORY_BASIC_INFORMATION info;
    uintptr_t at = (uintptr_t)ptr;
    DWORD access;
    if (!VirtualQuery(ptr, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return 0;
    access = info.Protect & 0xffU;
    if (access != PAGE_READONLY && access != PAGE_READWRITE &&
        access != PAGE_WRITECOPY && access != PAGE_EXECUTE_READ &&
        access != PAGE_EXECUTE_READWRITE && access != PAGE_EXECUTE_WRITECOPY)
        return 0;
    return at >= (uintptr_t)info.BaseAddress &&
           at - (uintptr_t)info.BaseAddress <= info.RegionSize &&
           len <= info.RegionSize - (at - (uintptr_t)info.BaseAddress);
}

static int terminated(const unsigned char *ptr, size_t remaining) {
    while (remaining) {
        MEMORY_BASIC_INFORMATION info;
        size_t bytes;
        if (!readable(ptr, 1) || !VirtualQuery(ptr, &info, sizeof(info)))
            return 0;
        bytes = info.RegionSize - ((uintptr_t)ptr - (uintptr_t)info.BaseAddress);
        if (bytes > remaining)
            bytes = remaining;
        if (memchr(ptr, 0, bytes))
            return 1;
        ptr += bytes;
        remaining -= bytes;
    }
    return 0;
}

static HMODULE owner(const void *ptr) {
    HMODULE module = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      (LPCSTR)ptr, &module);
    return module;
}

/* Kernel32 exports are commonly forwarded to KernelBase/ntdll. Such normal
 * loader forwarding must not be mistaken for an injected trampoline. */
static int system_owner(const void *ptr) {
    HMODULE module = owner(ptr);
    return module && (module == GetModuleHandleW(L"kernel32.dll") ||
                      module == GetModuleHandleW(L"kernelbase.dll") ||
                      module == GetModuleHandleW(L"ntdll.dll"));
}

int kagura_check_tracer_pid(void) {
    return kagura_check_debugger_present() || kagura_check_remote_debugger() ||
           kagura_check_nt_debug_port();
}

static const unsigned char *probe_address(unsigned index) {
    static const char *names[] = {
        "IsDebuggerPresent", "CheckRemoteDebuggerPresent", "VirtualProtect"
    };
    return (const unsigned char *)(uintptr_t)GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), names[index]);
}

int kagura_check_inline_hooks(void) {
    unsigned i;
    for (i = 0; i < 3; ++i) {
        const unsigned char *p = probe_address(i);
        const void *target = NULL;
        if (!p)
            continue;
        if (!system_owner(p))
            return 1;
#if defined(_M_X64) || defined(__x86_64__)
        if (!readable(p, 6))
            continue;
        if (p[0] == 0xe9) {
            int32_t displacement;
            memcpy(&displacement, p + 1, sizeof(displacement));
            target = (const void *)((uintptr_t)p + 5 + displacement);
        } else if (p[0] == 0xff && p[1] == 0x25) {
            int32_t displacement;
            const void *slot;
            memcpy(&displacement, p + 2, sizeof(displacement));
            slot = (const void *)((uintptr_t)p + 6 + displacement);
            if (readable(slot, sizeof(target)))
                memcpy(&target, slot, sizeof(target));
        }
#endif
        if (target && !system_owner(target))
            return 1;
    }
    return 0;
}

/* Compare the main image's normal IAT with loader resolution. Delay imports
 * and bound imports without an original name table are deliberately skipped. */
int kagura_check_got_hooks(void) {
    const unsigned char *base = (const unsigned char *)GetModuleHandleW(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt;
    const IMAGE_IMPORT_DESCRIPTOR *imports;
    size_t image_size, count, i;
    DWORD rva, directory_size;
    if (!readable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew < 0 || dos->e_lfanew > 0x100000)
        return 0;
    nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (!readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    image_size = nt->OptionalHeader.SizeOfImage;
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT)
        return 0;
    rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    directory_size = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;
    if (!rva || rva >= image_size || directory_size > image_size - rva)
        return 0;
    imports = (const IMAGE_IMPORT_DESCRIPTOR *)(base + rva);
    count = directory_size / sizeof(*imports);
    for (i = 0; i < count; ++i) {
        const IMAGE_IMPORT_DESCRIPTOR *imp = &imports[i];
        HMODULE library;
        size_t index, names_count, slots_count;
        const IMAGE_THUNK_DATA *names, *slots;
        if (!readable(imp, sizeof(*imp)) || !imp->Name)
            break;
        if (imp->Name >= image_size || !imp->OriginalFirstThunk ||
            imp->OriginalFirstThunk >= image_size || !imp->FirstThunk ||
            imp->FirstThunk >= image_size)
            continue;
        if (!terminated(base + imp->Name, image_size - imp->Name))
            continue;
        library = GetModuleHandleA((const char *)(base + imp->Name));
        if (!library)
            continue;
        names = (const IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
        slots = (const IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        names_count = (image_size - imp->OriginalFirstThunk) / sizeof(*names);
        slots_count = (image_size - imp->FirstThunk) / sizeof(*slots);
        for (index = 0; index < names_count && index < slots_count; ++index) {
            FARPROC expected;
            ULONG_PTR name;
            if (!readable(&names[index], sizeof(*names)) ||
                !readable(&slots[index], sizeof(*slots)))
                break;
            name = names[index].u1.AddressOfData;
            if (!name)
                break;
            if (IMAGE_SNAP_BY_ORDINAL(name)) {
                expected = GetProcAddress(library, (LPCSTR)(uintptr_t)IMAGE_ORDINAL(name));
            } else {
                size_t offset = offsetof(IMAGE_IMPORT_BY_NAME, Name);
                if (name >= image_size || offset >= image_size - name ||
                    !terminated(base + name + offset, image_size - name - offset))
                    continue;
                expected = GetProcAddress(library, (LPCSTR)(base + name + offset));
            }
            if (expected && slots[index].u1.Function != (ULONG_PTR)expected)
                return 1;
        }
    }
    return 0;
}

int kagura_check_sw_breakpoints(void) {
    unsigned i;
    for (i = 0; i < 3; ++i) {
        const unsigned char *p = probe_address(i);
        if (!p)
            continue;
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
        if (readable(p, 1) && *p == 0xcc)
            return 1;
#elif defined(_M_ARM64) || defined(__aarch64__)
        uint32_t instruction;
        if (readable(p, sizeof(instruction))) {
            memcpy(&instruction, p, sizeof(instruction));
            if ((instruction & 0xffe0001fU) == 0xd4200000U)
                return 1;
        }
#endif
    }
    return 0;
}

/* A real CPU breakpoint trap supplies this thread's debug registers. A
 * software RaiseException context does not contain them, and GetThreadContext
 * on the current running thread is not valid. No helper thread or suspension
 * is needed, including when the probe runs from a DLL constructor. */
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#if defined(_MSC_VER)
static __declspec(thread) volatile int hardware_detected;
static __declspec(thread) volatile int hardware_active;
#else
static _Thread_local volatile int hardware_detected;
static _Thread_local volatile int hardware_active;
#endif
static LONG CALLBACK inspect_context(EXCEPTION_POINTERS *info) {
    if (!hardware_active || info->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT)
        return EXCEPTION_CONTINUE_SEARCH;
    hardware_detected = (info->ContextRecord->Dr7 & 0xffU) != 0;
    // Windows reports the faulting INT3 address; resume after its one byte.
#if defined(_M_X64) || defined(__x86_64__)
    if (info->ContextRecord->Rip == (DWORD64)(uintptr_t)info->ExceptionRecord->ExceptionAddress)
        ++info->ContextRecord->Rip;
#else
    if (info->ContextRecord->Eip == (DWORD)(uintptr_t)info->ExceptionRecord->ExceptionAddress)
        ++info->ContextRecord->Eip;
#endif
    return EXCEPTION_CONTINUE_EXECUTION;
}
#if defined(_MSC_VER)
__declspec(noinline) static void hardware_trap(void) { __debugbreak(); }
#else
__attribute__((noinline)) static void hardware_trap(void) { __asm__ volatile("int3" ::: "memory"); }
#endif
#endif

int kagura_check_hw_breakpoints(void) {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    PVOID handler = AddVectoredExceptionHandler(1, inspect_context);
    if (!handler)
        return 0;
    hardware_detected = 0;
    hardware_active = 1;
    hardware_trap();
    hardware_active = 0;
    RemoveVectoredExceptionHandler(handler);
    return hardware_detected;
#else
    return 0;
#endif
}

int kagura_check_emulator(void) {
    if (GetModuleHandleW(L"VBoxOGLrenderspu.dll") ||
        GetModuleHandleW(L"libOpenglRender.dll"))
        return 1;
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
    {
        unsigned a, b, c, d;
        char vendor[13];
#if defined(_MSC_VER)
        int regs[4];
        __cpuid(regs, 0x40000000);
        a = (unsigned)regs[0]; b = (unsigned)regs[1];
        c = (unsigned)regs[2]; d = (unsigned)regs[3];
#else
        __cpuid(0x40000000, a, b, c, d);
#endif
        (void)a;
        memcpy(vendor, &b, 4); memcpy(vendor + 4, &c, 4);
        memcpy(vendor + 8, &d, 4); vendor[12] = 0;
        /* Hyper-V and other hardware hypervisors are not emulators. */
        return strcmp(vendor, "TCGTCGTCGTCG") == 0;
    }
#else
    return 0;
#endif
}
#endif
