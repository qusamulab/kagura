/* Internal locks for short runtime state updates. Not async-signal-safe.
 * Never invoke application callbacks while holding one of these locks. */
#ifndef KAGURA_RUNTIME_LOCK_H
#define KAGURA_RUNTIME_LOCK_H

#if defined(_WIN32)
#include <windows.h>
typedef volatile LONG kagura_lock_t;
#define KAGURA_LOCK_INIT 0
static inline void kagura_lock(kagura_lock_t *lock) {
    while (InterlockedCompareExchange(lock, 1, 0) != 0) YieldProcessor();
}
static inline void kagura_unlock(kagura_lock_t *lock) {
    InterlockedExchange(lock, 0);
}
#else
#include <stdatomic.h>
typedef atomic_flag kagura_lock_t;
#define KAGURA_LOCK_INIT ATOMIC_FLAG_INIT
static inline void kagura_lock(kagura_lock_t *lock) {
    while (atomic_flag_test_and_set_explicit(lock, memory_order_acquire)) {}
}
static inline void kagura_unlock(kagura_lock_t *lock) {
    atomic_flag_clear_explicit(lock, memory_order_release);
}
#endif
#endif
