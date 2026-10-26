#include <stdio.h>
#include <wchar.h>

// Keep the table visible to the pass even after -O2. Its strings are reached
// through nested constant initializers, including a pointer into a literal.
struct Names {
    const char *items[3];
};
const struct Names coverage_names = {{"Alpha", "Bravo", "xxCharlie" + 2}};
const wchar_t *coverage_wide[] = {L"Wide"};

__attribute__((noinline)) const char *coverage_name(unsigned i) {
    return coverage_names.items[i % 3];
}

__attribute__((noinline)) const char *coverage_short(int which) {
    return which ? "ab" : "xy";
}

// Startup decryption must precede user constructors, not just main().
__attribute__((constructor(101))) static void before_main(void) {
    puts(coverage_name(0));
}

int main(int argc, char **argv) {
    (void)argv;
    const char *saved = coverage_name(1);
    for (unsigned i = 0; i < 3; ++i)
        printf("name[%u]=%s\n", i, coverage_name(i));
    // Repeated calls must leave a previously returned pointer valid.
    printf("saved=%s short=%s/%s value=%d\n", saved,
           coverage_short(argc), coverage_short(argc - 1), argc);
    printf("wide=%lc%lc%lc%lc\n", coverage_wide[0][0], coverage_wide[0][1],
           coverage_wide[0][2], coverage_wide[0][3]);
    return 0;
}
