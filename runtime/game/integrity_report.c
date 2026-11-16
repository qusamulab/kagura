/* Authenticated application reports. Version 2 requires a caller-supplied
 * HMAC-SHA256 signer; there is no device-fingerprint-based authentication.
 * The server must verify that MAC and atomically consume its own challenge.
 * The bounded local replay cache is a convenience, not server-side protection.
 */

#include "../internal.h"
#include "../lock.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ---- Device identifier hash (not authentication) ------------------------- */

#define fnv1a64(data, len) kagura_fnv1a64_buf((data), (len))

/* ---- Nonce replay-prevention ring buffer ---------------------------------- */

#define NONCE_HISTORY  32
#define NONCE_MAX_LEN  64
#define NONCE_TTL_SECS 300

typedef struct {
    char   value[NONCE_MAX_LEN + 1];
    time_t seen_at;
} nonce_entry_t;

static nonce_entry_t g_nonce_ring[NONCE_HISTORY];
static int           g_nonce_head = 0;
static kagura_lock_t g_nonce_lock = KAGURA_LOCK_INIT;

static int valid_nonce(const char *nonce, size_t len) {
    if (!nonce || !len || len > NONCE_MAX_LEN) return 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)nonce[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return 0;
    }
    return 1;
}

/* Caller holds g_nonce_lock. Clock rollback retains entries conservatively. */
static int nonce_fresh_locked(const char *nonce, size_t len, time_t now) {
    for (int i = 0; i < NONCE_HISTORY; ++i) {
        nonce_entry_t *e = &g_nonce_ring[i];
        if (!e->value[0]) continue;
        if (now >= e->seen_at && difftime(now, e->seen_at) > NONCE_TTL_SECS) {
            e->value[0] = '\0';
            continue;
        }
        if (strlen(e->value) == len && memcmp(e->value, nonce, len) == 0)
            return 0;
    }
    return 1;
}

static void nonce_store_locked(const char *nonce, size_t len, time_t now) {
    nonce_entry_t *slot = &g_nonce_ring[g_nonce_head];
    memcpy(slot->value, nonce, len);
    slot->value[len] = '\0';
    slot->seen_at = now;
    g_nonce_head = (g_nonce_head + 1) % NONCE_HISTORY;
}

int kagura_nonce_is_fresh(const char *nonce, size_t len) {
    if (!valid_nonce(nonce, len)) return 0;
    kagura_lock(&g_nonce_lock);
    int fresh = nonce_fresh_locked(nonce, len, time(NULL));
    kagura_unlock(&g_nonce_lock);
    return fresh;
}

void kagura_nonce_consume(const char *nonce, size_t len) {
    if (!valid_nonce(nonce, len)) return;
    kagura_lock(&g_nonce_lock);
    time_t now = time(NULL);
    if (nonce_fresh_locked(nonce, len, now))
        nonce_store_locked(nonce, len, now);
    kagura_unlock(&g_nonce_lock);
}

/* ---- Device key hash ------------------------------------------------------ */

static uint64_t get_device_key_hash(void) {
    uint8_t key[16] = {0};
    /* kagura_device_key returns 1 on success and leaves the buffer untouched
     * on failure.  It used to be declared here as returning void, so the
     * status was invisible and an all-zero key hashed identically on every
     * device that had no stable identity. */
    if (!kagura_device_key(key))
        memset(key, 0, sizeof(key));
    return fnv1a64(key, sizeof(key));
}

/* ---- Violation flags bitmask ----------------------------------------------
 *
 * These five probes used to be `extern ... __attribute__((weak))` declarations
 * of kagura_root_check / kagura_frida_check / kagura_emulator_check /
 * kagura_debugger_check / kagura_integrity_check.  None of those symbols has
 * ever existed under any of those names, so every weak pointer was NULL and
 * collect_violation_flags() returned 0 unconditionally - the report always
 * claimed a clean device.
 *
 * Each probe now dispatches to the real detector for the platform.  The names
 * come from internal.h, so a rename is a compile error rather than a silent
 * regression back to "always clean".
 */

#define FLAG_ROOTED       (1u << 0)
#define FLAG_FRIDA        (1u << 1)
#define FLAG_EMULATOR     (1u << 2)
#define FLAG_DEBUGGER     (1u << 3)
#define FLAG_INTEGRITY    (1u << 4)

/* Root / jailbreak. */
static int probe_rooted(void) {
#if defined(__APPLE__)
    return kagura_jailbreak_detected();
#elif defined(__linux__) || defined(__ANDROID__)
    return kagura_magisk_present() || kagura_xposed_present();
#else
    return 0;   /* no equivalent concept on Windows */
#endif
}

/* Frida / Substrate / other injected instrumentation. */
static int probe_frida(void) {
#if defined(_WIN32)
    return kagura_check_injected_dlls() || kagura_check_frida_port();
#else
    return kagura_suspicious_lib_loaded() || kagura_check_frida_port();
#endif
}

/* Emulator / simulator. */
static int probe_emulator(void) {
#if defined(_WIN32)
    return 0;   /* runtime/windows has no emulator detector yet */
#else
    return kagura_check_emulator();
#endif
}

/* Attached debugger. */
static int probe_debugger(void) {
#if defined(_WIN32)
    return kagura_check_debugger_present() ||
           kagura_check_remote_debugger()  ||
           kagura_check_nt_debug_port();
#else
    return kagura_check_tracer_pid();
#endif
}

/* Executable image integrity. */
static int probe_integrity(void) {
#if defined(__APPLE__)
    return kagura_macho_tampered();
#elif defined(__linux__) || defined(__ANDROID__)
    return kagura_elf_tampered();
#elif defined(_WIN32)
    return !kagura_pe_checksum_valid();
#else
    return 0;
#endif
}

static uint32_t collect_violation_flags(void) {
    uint32_t flags = 0;
    if (probe_rooted())    flags |= FLAG_ROOTED;
    if (probe_frida())     flags |= FLAG_FRIDA;
    if (probe_emulator())  flags |= FLAG_EMULATOR;
    if (probe_debugger())  flags |= FLAG_DEBUGGER;
    if (probe_integrity()) flags |= FLAG_INTEGRITY;
    return flags;
}

/* ---- Report builder ---------------------------------------------------- */

int kagura_integrity_report_build_signed(const char *nonce, char *out_buf,
        size_t buf_len, kagura_report_signer signer, void *userdata) {
    if (!out_buf || !buf_len) return -1;
    out_buf[0] = '\0';
    if (!nonce || !signer) return -1;
    size_t len = 0;
    while (len <= NONCE_MAX_LEN && nonce[len]) ++len;
    if (!valid_nonce(nonce, len) || !kagura_nonce_is_fresh(nonce, len)) return -1;

    uint64_t dk_hash = get_device_key_hash(); /* Identifier only, never a key. */
    int bscore = kagura_suspicion_score();
    uint32_t vflags = collect_violation_flags();
    time_t ts = time(NULL);
    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "{\"version\":2,\"mac_algorithm\":\"HMAC-SHA256\","
        "\"device_key_hash\":\"%016llx\",\"behavior_score\":%d,"
        "\"violation_flags\":%u,\"nonce\":\"%s\",\"timestamp\":%lld}",
        (unsigned long long)dk_hash, bscore, vflags, nonce, (long long)ts);
    if (body_len <= 0 || (size_t)body_len >= sizeof(body)) return -1;

    uint8_t mac[32] = {0};
    // The signer owns key provisioning and may use a hardware-backed service.
    // No lock is held during detector or application calls.
    if (signer((const uint8_t *)body, (size_t)body_len, mac, userdata) != 1) {
        kagura_zero_buf(mac, sizeof(mac));
        return -1;
    }
    char hex[65];
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(mac); ++i) {
        hex[2*i] = digits[mac[i] >> 4];
        hex[2*i+1] = digits[mac[i] & 15];
    }
    hex[64] = '\0';
    kagura_zero_buf(mac, sizeof(mac));
    char report[640];
    int n = snprintf(report, sizeof(report), "%.*s,\"mac\":\"%s\"}",
                     body_len - 1, body, hex);
    if (n <= 0 || (size_t)n >= sizeof(report) || (size_t)n >= buf_len) return -1;

    // Check and consume atomically at commit; concurrent attempts cannot both
    // return a report. Failed signing/buffer attempts do not consume a nonce.
    kagura_lock(&g_nonce_lock);
    time_t now = time(NULL);
    int fresh = nonce_fresh_locked(nonce, len, now);
    if (fresh) nonce_store_locked(nonce, len, now);
    kagura_unlock(&g_nonce_lock);
    if (!fresh) return -1;
    memcpy(out_buf, report, (size_t)n + 1);
    return n;
}

int kagura_integrity_report_build(const char *nonce, char *out_buf, size_t buf_len) {
    // Retain the ABI, but never produce the forgeable legacy report.
    return kagura_integrity_report_build_signed(nonce, out_buf, buf_len, NULL, NULL);
}
