#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"

static Term term;

int main(int argc, char **argv) {
    setlocale(LC_CTYPE, "");
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    char *const *cmd = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-e") || !strcmp(argv[i], "--")) {
            if (i + 1 < argc) cmd = (char *const *)&argv[i + 1];
            break;
        }
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("titty - GPU accelerated wayland terminal\n"
                   "usage: titty [-e command [args...]]\n");
            return 0;
        }
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
            printf("titty 0.1\n");
            return 0;
        }
    }

    app.cols = WINDOW_COLS;
    app.rows = WINDOW_ROWS;
    app.running = true;
    app.focused = true;
    app.term = &term;
    app.cmd = cmd;

    term.ptyfd = -1;
    term_init(&term, app.cols, app.rows);

    wl_run(&app);

    if (term.ptyfd >= 0) close(term.ptyfd);
    term_free(&term);
    return 0;
}
