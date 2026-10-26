#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char text[] = "prefix-secret-data";
static atomic_uint ready;
static atomic_int go;

__attribute__((noinline, optnone)) const char *literal(void) { return text + 7; }

static void *reader(void *unused) {
  (void)unused;
  atomic_fetch_add(&ready, 1);
  while (!atomic_load(&go)) {}
  for (unsigned i = 0; i < 1000; ++i) {
    const char *p = literal();
    if (p[0] != 's' || p[5] != 't' || strlen(p) != 11)
      return (void *)(uintptr_t)1;
  }
  return 0;
}

int main(void) {
  pthread_t threads[8];
  for (unsigned i = 0; i < 8; ++i)
    if (pthread_create(&threads[i], 0, reader, 0))
      return 2;
  while (atomic_load(&ready) != 8) {}
  atomic_store(&go, 1);
  for (unsigned i = 0; i < 8; ++i) {
    void *result;
    if (pthread_join(threads[i], &result) || result)
      return 1;
  }
  puts("threaded literals ok");
  return 0;
}
