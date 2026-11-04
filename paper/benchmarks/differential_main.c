/* Deterministic, bounded inputs for the paper's four existing subjects.
 * This driver is compiled without Kagura. Each line is compared byte-for-byte
 * with the unobfuscated executable; no digest can hide a mismatched return.
 * Coordinates and ticks stay small to avoid signed overflow in the subject.
 */
#include <stdint.h>
#include <stdio.h>

typedef struct { uint32_t item_id, quantity, checksum; } ItemGrant;
typedef struct { int32_t x, y, z; } Vec3;
extern int eval_verify_score(uint32_t, uint32_t, uint32_t, uint32_t);
extern int eval_verify_item(const ItemGrant *, uint32_t);
extern int eval_check_movement(const Vec3 *, const Vec3 *, uint32_t);
extern int eval_validate_token(const uint8_t *, uint64_t, uint64_t, uint64_t);

static uint32_t state = 20261104u;
static uint32_t next(void) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
static uint32_t checksum(uint32_t id, uint32_t quantity) {
    uint32_t h = 2166136261u;
    h = (h ^ (id & 255u)) * 16777619u;
    h = (h ^ (id >> 8)) * 16777619u;
    h = (h ^ (quantity & 255u)) * 16777619u;
    return (h ^ (quantity >> 8)) * 16777619u;
}

int main(void) {
    for (unsigned i = 0; i < 1024; ++i) {
        /* Half the score inputs reach an accepting case by construction. */
        uint32_t score = next() & 9999999u;
        uint32_t prev = score / 2u, delta = score - prev;
        uint32_t key = ((prev ^ 0xA5C3E1B7u) + delta * 0x9e3779b9u) ^ score;
        if (i % 2) key ^= 1u;
        if (i % 17 == 0) score = 10000000u;
        if (i % 19 == 0) prev = score + 1u;
        int s = eval_verify_score(score, prev, delta, key);

        ItemGrant g = {next() % 1100u, next() % 1050u, 0};
        g.checksum = checksum(g.item_id, g.quantity);
        if (i % 2) g.checksum ^= 1u;
        uint32_t level = next() % 40u;
        int item = eval_verify_item(i % 31 == 0 ? NULL : &g, level);

        Vec3 a = {(int32_t)(next() % 201u) - 100,
                  (int32_t)(next() % 201u) - 100, 0};
        Vec3 b = {a.x + (int32_t)(next() % 61u) - 30,
                  a.y + (int32_t)(next() % 61u) - 30,
                  (int32_t)(next() % 51u) - 35};
        uint32_t ticks = next() % 5u;
        int movement = eval_check_movement(i % 29 == 0 ? NULL : &a,
                                          i % 37 == 0 ? NULL : &b, ticks);

        /* Non-null inputs exercise the token loop; no accepting MAC is claimed. */
        uint8_t token[32];
        for (unsigned j = 0; j < sizeof(token); ++j) token[j] = (uint8_t)next();
        uint64_t pid = next(), ts = next(), secret = next();
        int auth = eval_validate_token(i % 23 == 0 ? NULL : token, pid, ts, secret);
        printf("%d %d %d %d\n", s, item, movement, auth);
    }
    return 0;
}
