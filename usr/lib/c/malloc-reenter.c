#include <stdio.h>
#include <stdint.h>
static void *last_caller;
void mt_reenter_log(void *lp, void *caller)
{
    if (caller != last_caller) {
        fprintf(stderr, "REENTER lp=%p caller=%p\n", lp, caller);
        last_caller = caller;
    }
}
