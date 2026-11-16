/*===-- runtime/behavior_log.c - Suspicious behavior logging --------------===
 *
 * Collect and report suspicious cheat-indicative behaviors.
 *
 * Rather than immediately banning on first detection, this module accumulates
 * suspicious event signals into a circular ring buffer.  The game can
 * periodically flush the log to a backend for analysis, or apply soft
 * responses (penalise, degrade experience) based on accumulated score.
 *
 * Event types
 * -----------
 *   KAGURA_EVENT_MEMORY_TAMPER  — value outside expected range
 *   KAGURA_EVENT_SPEED_HACK     — delta-time anomaly
 *   KAGURA_EVENT_HOOK_DETECTED  — runtime hook found
 *   KAGURA_EVENT_DEBUG_DETECTED — debugger or analysis tool
 *   KAGURA_EVENT_CUSTOM         — app-defined event (code in range [64,127])
 *
 * Public API
 * ----------
 *   void kagura_log_event(uint8_t type, uint32_t detail);
 *   int  kagura_event_count(uint8_t type);
 *   int  kagura_suspicion_score(void);   // weighted sum of all events
 *   void kagura_flush_events(void (*cb)(uint8_t type, uint32_t detail, void*),
 *                             void *userdata);
 *   void kagura_clear_events(void);
 *
 *===----------------------------------------------------------------------===*/

#include "../internal.h"
#include "../lock.h"

#include <stdint.h>
#include <string.h>

#define KAGURA_LOG_SIZE 128

/* ── Event type constants ────────────────────────────────────────────────── */
#define KAGURA_EVENT_MEMORY_TAMPER  0x01
#define KAGURA_EVENT_SPEED_HACK     0x02
#define KAGURA_EVENT_HOOK_DETECTED  0x03
#define KAGURA_EVENT_DEBUG_DETECTED 0x04
#define KAGURA_EVENT_REPACKAGED     0x05
#define KAGURA_EVENT_CUSTOM_BASE    0x40  /* app-defined: 0x40–0x7F */

typedef struct {
    uint8_t  type;
    uint32_t detail;
} kagura_event_t;

static kagura_event_t kEventBuf[KAGURA_LOG_SIZE];
static int kEventHead = 0;
static int kEventCount = 0;
static kagura_lock_t kEventLock = KAGURA_LOCK_INIT;

void kagura_log_event(uint8_t type, uint32_t detail) {
    kagura_lock(&kEventLock);
    kEventBuf[kEventHead % KAGURA_LOG_SIZE].type   = type;
    kEventBuf[kEventHead % KAGURA_LOG_SIZE].detail = detail;
    kEventHead = (kEventHead + 1) % KAGURA_LOG_SIZE;
    if (kEventCount < KAGURA_LOG_SIZE) ++kEventCount;
    kagura_unlock(&kEventLock);
}

int kagura_event_count(uint8_t type) {
    int count = 0;
    kagura_lock(&kEventLock);
    int start = (kEventHead - kEventCount + KAGURA_LOG_SIZE) % KAGURA_LOG_SIZE;
    for (int i = 0; i < kEventCount; ++i) {
        if (kEventBuf[(start + i) % KAGURA_LOG_SIZE].type == type)
            ++count;
    }
    kagura_unlock(&kEventLock);
    return count;
}

/* Weighted suspicion score */
int kagura_suspicion_score(void) {
    int score = 0;
    kagura_lock(&kEventLock);
    for (int i = 0; i < kEventCount; ++i) {
        switch (kEventBuf[(kEventHead - kEventCount + KAGURA_LOG_SIZE + i) % KAGURA_LOG_SIZE].type) {
        case KAGURA_EVENT_HOOK_DETECTED:
        case KAGURA_EVENT_DEBUG_DETECTED: score += 10; break;
        case KAGURA_EVENT_REPACKAGED: score += 15; break;
        case KAGURA_EVENT_MEMORY_TAMPER: score += 5; break;
        case KAGURA_EVENT_SPEED_HACK: score += 3; break;
        default: break;
        }
    }
    kagura_unlock(&kEventLock);
    return score;
}

void kagura_flush_events(
        void (*cb)(uint8_t type, uint32_t detail, void *userdata),
        void *userdata) {
    kagura_event_t snapshot[KAGURA_LOG_SIZE];
    kagura_lock(&kEventLock);
    int count = kEventCount;
    int start = (kEventHead - kEventCount + KAGURA_LOG_SIZE) % KAGURA_LOG_SIZE;
    for (int i = 0; i < count; ++i)
        snapshot[i] = kEventBuf[(start + i) % KAGURA_LOG_SIZE];
    kagura_unlock(&kEventLock);
    for (int i = 0; i < count; ++i) {
        kagura_event_t *e = &snapshot[i];
        if (cb) cb(e->type, e->detail, userdata);
    }
}

void kagura_clear_events(void) {
    kagura_lock(&kEventLock);
    kEventHead  = 0;
    kEventCount = 0;
    memset(kEventBuf, 0, sizeof(kEventBuf));
    kagura_unlock(&kEventLock);
}
