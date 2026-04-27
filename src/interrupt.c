#include "interrupt.h"

#include <signal.h>

static volatile sig_atomic_t g_interrupted = 0;

static void handle_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

void interrupt_setup(void) {
    g_interrupted = 0;
    signal(SIGINT, handle_sigint);
}

void interrupt_reset(void) {
    g_interrupted = 0;
}

int interrupt_requested(void) {
    return g_interrupted != 0;
}