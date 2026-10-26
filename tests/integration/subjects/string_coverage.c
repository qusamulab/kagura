#include <stdio.h>
#include <wchar.h>

// Keep the table visible to the pass even after -O2. Its strings are reached
// through nested constant initializers, including a pointer into a literal.
struct Names {
    const char *items[3];
};
// MSVC pools anonymous literals into public COMDAT globals, which the
// conservative storage policy intentionally excludes. Named local storage
// keeps this coverage fixture eligible on every supported target.
static const char alpha[] = "Alpha";
static const char bravo[] = "Bravo";
static const char charlie[] = "xxCharlie";
static const char short_ab[] = "ab";
static const char short_xy[] = "xy";
static const wchar_t wide[] = L"Wide";
static const char name_format[] = "name[%u]=%s\n";
static const char saved_format[] = "saved=%s short=%s/%s value=%d\n";
static const char wide_format[] = "wide=%lc%lc%lc%lc\n";
const struct Names coverage_names = {{alpha, bravo, charlie + 2}};
const wchar_t *coverage_wide[] = {wide};

__attribute__((noinline)) const char *coverage_name(unsigned i) {
    return coverage_names.items[i % 3];
}

__attribute__((noinline)) const char *coverage_short(int which) {
    return which ? short_ab : short_xy;
}

// Startup decryption must precede user constructors, not just main().
__attribute__((constructor(101))) static void before_main(void) {
    puts(coverage_name(0));
}

int main(int argc, char **argv) {
    (void)argv;
    const char *saved = coverage_name(1);
    for (unsigned i = 0; i < 3; ++i)
        printf(name_format, i, coverage_name(i));
    // Repeated calls must leave a previously returned pointer valid.
    printf(saved_format, saved,
           coverage_short(argc), coverage_short(argc - 1), argc);
    printf(wide_format, coverage_wide[0][0], coverage_wide[0][1],
           coverage_wide[0][2], coverage_wide[0][3]);
    return 0;
}
