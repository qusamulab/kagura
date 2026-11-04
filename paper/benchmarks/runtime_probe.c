/* Diagnostic observation only: a positive signal is not proof of an attack. */
#include <stdio.h>
#include "../../runtime/internal.h"

int main(void) {
    printf("tracer=%d\n", kagura_check_tracer_pid());
    printf("inline_hooks=%d\n", kagura_check_inline_hooks());
    printf("got_hooks=%d\n", kagura_check_got_hooks());
    printf("sw_breakpoints=%d\n", kagura_check_sw_breakpoints());
    printf("hw_breakpoints=%d\n", kagura_check_hw_breakpoints());
    printf("emulator=%d\n", kagura_check_emulator());
    return 0;
}
