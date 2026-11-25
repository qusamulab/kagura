# Runtime Library

Some passes emit calls to symbols they do not define. Those targets require
linking `libkagura_runtime.a` (built at `build/runtime/libkagura_runtime.a`):

| Pass | Flag | Symbols it calls but does not define |
|:-----|:-----|:-------------------------------------|
| StringEncryptionAES | `-kagura-str-aes` | `kagura_aes128_ctr_decrypt`, `kagura_check_blob_integrity` |
| VMObfuscation | `-kagura-vm` | `kagura_vm_execute` |
| AntiDebug | `-kagura-anti-debug` | `kagura_check_tracer_pid`, `kagura_check_inline_hooks`, `kagura_check_got_hooks`, `kagura_check_sw_breakpoints`, `kagura_check_hw_breakpoints`, `kagura_check_emulator` |
| AntiTamper | `-kagura-tamper` | `kagura_self_check`, `kagura_tamper_detected`, `kagura_runtime_hash_check` |
| BasicBlockChecksum | `-kagura-bbcheck` | `kagura_bb_check`, `kagura_on_tamper_detected` |
| Telemetry | `-kagura-telemetry` | `kagura_telemetry_event` |
| CallIndirection | `-kagura-ci` | POSIX: `kagura_rtld_default_handle` + `dlsym`; Windows: `kagura_windows_resolve_symbol` |
| PointerAuth | `-kagura-pac` | `kagura_random_u64` |
| JNIObfuscation | `-kagura-jni` | `kagura_jni_get_env`, `kagura_jni_find_class`, `kagura_jni_register_native` |
| ObjCObfuscation | `-kagura-objc` | `kagura_objc_register_remap` |

Two names that look like they belong in that table do not, because the pass
**defines** them rather than importing them: `kagura_anti_debug_init` (the
module constructor AntiDebug builds) and `kagura_on_tamper_detected` (AntiDebug
emits a weak default that calls `abort()`; override it to choose your own
response). Every other pass — the string, control-flow and data passes —
generates self-contained code and needs no runtime at all.

```bash
clang your_file.c build/runtime/libkagura_runtime.a -o your_file
```

## Directly callable API

`include/kagura/runtime.h` is the public subset of the runtime: the detectors
you can call yourself, the hooks you can override, and the protected value
types.

```c
#include "kagura/runtime.h"

if (kagura_suspicious_lib_loaded())   { /* Frida gadget, Substrate, … */ }
if (kagura_check_tracer_pid())        { /* ptrace / debugger attached */ }
if (kagura_check_sw_breakpoints())    { /* int3 / BRK patched into code */ }
if (kagura_jailbreak_detected())      { /* Apple targets */ }
if (kagura_magisk_present())          { /* Android targets */ }
```

Use these from your `main()` (mobile apps) or your DLL `DllMain` (Windows) to
get the same defense without going through pass-injected init code — useful in
projects where you want explicit control over when checks fire.

### Predicates versus responses

Two shapes exist and they are not interchangeable:

| Shape | Returns | Behaviour |
|:------|:--------|:----------|
| `int kagura_check_*(void)` | nonzero == detected | Reports. Does nothing else. |
| `void kagura_*_check(void)` | nothing | Runs the predicates *and* invokes the tamper hook on a hit. |

`kagura_self_check()`, `kagura_check_hooks()` and `kagura_check_breakpoints()`
are in the second group despite their names, so `if (kagura_self_check() != 0)`
does not compile against this header. It used to appear to work when callers
hand-wrote their own `extern int` declaration — and then branched on a garbage
register. `runtime/ios/device_attest.c` documents the same failure hitting four
symbols at once, which is the reason this header exists.

Always `#include "kagura/runtime.h"` instead of declaring these by hand.

## Authenticated integrity reports

Migrate calls to `kagura_integrity_report_build()` to
`kagura_integrity_report_build_signed()`. The legacy function now returns `-1`
and clears a writable output buffer: its old FNV-based MAC was computable from
public report fields and could not authenticate a report.

```c
int kagura_integrity_report_build_signed(
    const char *nonce, char *out, size_t capacity,
    kagura_report_signer signer, void *userdata);
```

Supply a signer with signature
`int(const uint8_t *body, size_t len, uint8_t mac[32], void *userdata)`.
It must calculate HMAC-SHA256 over exactly `len` supplied bytes using an
independently provisioned secret shared with your server, fill all 32 MAC
bytes, and return `1`. Use an established cryptographic implementation and
your application's key management. The runtime provides neither a default
authentication key nor an HMAC implementation. `device_key_hash` is an
informational identifier and must never be used as the signing key.

The version-2 JSON includes `version`, `mac_algorithm`, `device_key_hash`,
`behavior_score`, `violation_flags`, `nonce`, `timestamp`, then a final `mac`
field containing 64 lowercase hex characters. The signer receives the exact
object without that final field, ending in `}`. The server must reconstruct
those bytes by removing the final `,"mac":"..."` field and restoring `}`;
parsing and reserializing JSON can change the authenticated bytes. Verify the
MAC and atomically validate and consume a server-issued challenge. A client
key alone does not prove that the client or its detector results are intact;
use platform attestation when that assurance is required.

Nonces contain 1–64 hex characters. Success returns the output byte count;
failure returns `-1` with an empty writable output. Signing failures and small
buffers do not consume the nonce, so callers can retry. Concurrent attempts
with the same nonce can return at most one successful report while it remains
in the local replay cache. That cache retains only 32 entries for 300 seconds
and resets on process restart; it cannot replace server-side replay checks.
Signers run outside internal locks and must support the application's calling
threads. Report and event APIs are not async-signal-safe.

`kagura_log_event`, `kagura_event_count`, `kagura_suspicion_score` and
`kagura_clear_events` synchronize the shared event ring. `kagura_flush_events`
calls the application once per event from a locked snapshot, after releasing
the lock, and does not clear the ring. Callbacks may safely log or clear events.

## Platform attestation API

Thin C bindings for the major platform attestation services. The C side
generates nonces and runs fast local pre-screens; the async signed-token
round-trip is wired up from your Swift / Kotlin code.

### Apple — DeviceCheck / App Attest (`runtime/ios/device_attest.c`)

```c
int kagura_devicecheck_available(void);     // iOS 11+, macOS 10.15+
int kagura_appattest_available(void);       // iOS 14+, A10+ hardware

int kagura_appattest_nonce(uint8_t *out, size_t len);
int kagura_appattest_local_check(void);     // fast (<5ms) env screen
```

Swift bridge example:

```swift
import DeviceCheck
let service = DCAppAttestService.shared
if service.isSupported && kagura_appattest_local_check() == 1 {
    var nonce = Data(count: 32)
    _ = nonce.withUnsafeMutableBytes { kagura_appattest_nonce($0.baseAddress, 32) }
    service.generateKey { keyId, err in /* server-side verification */ }
}
```

### Android — Play Integrity (`runtime/android/play_integrity.c`)

```c
void kagura_play_integrity_nonce(char *out_hex32, size_t len);
int  kagura_play_integrity_verdict_ok(const char *jwt_payload_b64url);
int  kagura_play_integrity_local_check(void);
```

The full JWT signature must be verified server-side — `verdict_ok` is a
**local fast-path**, not a security boundary. See the file header comment
for the Kotlin caller skeleton.

### Windows — ETW analysis-tool detection (`runtime/windows/etw_detection.c`)

```c
int kagura_etw_provider_present(const wchar_t *provider_guid);
int kagura_etw_analysis_tool_check(void);
```

Both entry points are stubs and always return zero. ETW provider enumeration
is unimplemented, including in builds that define `KAGURA_ETW_FULL`.

## Source layout

```
runtime/
├── core/         AES, VM interpreter, crash symbolication, device key
├── anti_debug/   Cross-platform POSIX anti-debug / anti-Frida
├── android/      Root detection, attestation, /proc, syscall probes (Android + Linux)
├── ios/          Jailbreak detection, Mach-O integrity (iOS + macOS)
├── windows/      IsDebuggerPresent, NtQueryInformationProcess, PE integrity
└── game/         Anti-cheat, IL2CPP protection, telemetry
```

## Windows pass helpers

AntiDebug's six entry points use Windows debugger APIs, API-entry hook and
breakpoint probes, normal IAT validation, current-thread debug registers on
x86/x64, and known emulator indicators. They are bounded heuristics, not an
exhaustive detector. IAT checking skips delay imports and bound imports without
an original name table. Hardware breakpoint checking returns no detection on
unsupported architectures.

CallIndirection resolves `dllimport` functions from already loaded modules with
`GetProcAddress`; it does not load DLLs. Ordinary external declarations,
including static CRT and other translation-unit definitions, remain direct
calls because their names need not be exported. Libraries must remain loaded
while cached pointers are used. Missing symbols invoke the tamper-response hook.
Windows inline-hook probes recognize x64 relative/indirect entry jumps; software
breakpoint probes cover x86/x64 INT3 and ARM64 BRK at selected API entries.

The original import operand retains the DLL dependency and identifies its
exporting module, preventing collisions between DLLs with the same symbol
name. Ordinal-only exports and import-library aliases retain the loader
address if a named lookup is unavailable. The PE import table remains visible.
