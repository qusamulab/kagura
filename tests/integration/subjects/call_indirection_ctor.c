#include <stdio.h>

__attribute__((noinline)) void greet(void) { puts("constructor ready"); }
__attribute__((constructor(101))) static void early(void) { greet(); }

int main(void) {
  puts("main ready");
  return 0;
}
