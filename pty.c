#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "common.h"

void pty_resize(int fd, int cols, int rows, int px, int py) {
    struct winsize ws = {
        .ws_row = (unsigned short)rows,
        .ws_col = (unsigned short)cols,
        .ws_xpixel = (unsigned short)px,
        .ws_ypixel = (unsigned short)py
    };
    ioctl(fd, TIOCSWINSZ, &ws);
}

static const char *pick_shell(void) {
    if (SHELL_OVERRIDE[0]) return SHELL_OVERRIDE;
    const char *s = getenv("SHELL");
    if (s && *s) return s;
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_shell && *pw->pw_shell) return pw->pw_shell;
    return "/bin/sh";
}

int pty_spawn(int cols, int rows, char *const argv[]) {
    int master;
    struct winsize ws = {
        .ws_row = (unsigned short)rows,
        .ws_col = (unsigned short)cols,
        .ws_xpixel = (unsigned short)(cols * font_cell_w()),
        .ws_ypixel = (unsigned short)(rows * font_cell_h())
    };

    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) {
        perror("titty: forkpty");
        return -1;
    }

    if (pid == 0) {
        setenv("TERM", TERM_NAME, 1);
        setenv("COLORTERM", "truecolor", 1);
        setenv("TERM_PROGRAM", "titty", 1);
        unsetenv("LINES");
        unsetenv("COLUMNS");

        signal(SIGCHLD, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGALRM, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);

        if (argv && argv[0]) {
            execvp(argv[0], argv);
            fprintf(stderr, "titty: exec %s: %s\r\n", argv[0], strerror(errno));
        } else {
            const char *sh = pick_shell();
            const char *base = strrchr(sh, '/');
            char arg0[256];
            snprintf(arg0, sizeof arg0, "-%s", base ? base + 1 : sh);
            char *sargv[] = { arg0, NULL };
            execv(sh, sargv);
            fprintf(stderr, "titty: exec %s: %s\r\n", sh, strerror(errno));
        }
        _exit(127);
    }

    int fl = fcntl(master, F_GETFL, 0);
    fcntl(master, F_SETFL, fl | O_NONBLOCK);
    fcntl(master, F_SETFD, FD_CLOEXEC);
    return master;
}
