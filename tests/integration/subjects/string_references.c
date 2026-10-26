#include <stdint.h>
#include <stdio.h>

static const char text[] = "prefix-secret-data";
static const uint16_t wide[] = {72, 105, 0};
static const uint16_t *const table[] = {wide};

// Keep the accesses visible to the module passes after clang's -O2 pipeline.
__attribute__((noinline, optnone)) const char *base(void) { return text; }
__attribute__((noinline, optnone)) const char *substring(void) { return text + 7; }
__attribute__((noinline, optnone)) unsigned wide_direct(void) { return wide[0]; }
__attribute__((noinline, optnone)) unsigned wide_indirect(void) {
  return table[0][0];
}

__attribute__((noinline)) void clobber(void) {
  volatile unsigned char scratch[1024];
  for (unsigned i = 0; i < sizeof(scratch); ++i)
    scratch[i] = (unsigned char)i;
}

int main(void) {
  const char *p = base();
  const char *q = substring();
  clobber();
  printf("%d %s %u %u\n", p + 7 == q, q, wide_direct(), wide_indirect());
  return 0;
}
