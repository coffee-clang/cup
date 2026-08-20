/* Test-only POSIX process-group control used to verify descendant cleanup. */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int parse_group(const char *text, pid_t *group) {
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value <= 1) {
        return 0;
    }
    *group = (pid_t)value;
    return 1;
}

static int signal_group(const char *text, int signal_number) {
    pid_t group;

    if (!parse_group(text, &group)) {
        return 2;
    }
    if (kill(-group, signal_number) == 0) {
        return 0;
    }
    return errno == ESRCH ? 1 : 2;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "run") == 0) {
        if (setpgid(0, 0) != 0) {
            perror("setpgid");
            return 2;
        }
        execvp(argv[2], &argv[2]);
        perror("execvp");
        return 127;
    }
    if (argc == 3 && strcmp(argv[1], "alive") == 0) {
        return signal_group(argv[2], 0);
    }
    if (argc == 3 && strcmp(argv[1], "term") == 0) {
        return signal_group(argv[2], SIGTERM);
    }
    if (argc == 3 && strcmp(argv[1], "kill") == 0) {
        return signal_group(argv[2], SIGKILL);
    }
    fprintf(stderr, "Usage: %s run <command> [args...] | alive|term|kill <pgid>\n", argv[0]);
    return 2;
}
