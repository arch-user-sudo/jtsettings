/* jtsettingsctl — send a command to a running jtsettings window.
 *
 * Usage:
 *   jtsettingsctl accent '#RRGGBB'   change the accent live
 *   jtsettingsctl '#RRGGBB'          shorthand for the same
 *   jtsettingsctl page keybindings   switch pages
 *   jtsettingsctl apply              apply pending display changes
 *   jtsettingsctl reset              discard pending display changes
 *   jtsettingsctl dump               print detected outputs to stderr
 *   jtsettingsctl quit               exit the window
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static int
ctl_path(char *buf, size_t n)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    int len;
    if (rt && rt[0])
        len = snprintf(buf, n, "%s/jtsettings-ctl.sock", rt);
    else
        len = snprintf(buf, n, "/tmp/jtsettings-ctl-%u.sock",
                       (unsigned)getuid());
    return len >= 0 && (size_t)len < n;
}

/* The server treats each line as one command.  Reject control characters so
 * an argument cannot smuggle a second command ("page profile\nquit"). */
static int
clean_arg(const char *s)
{
    if (!s)
        return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 0x20 || *p == 0x7F)
            return 0;
    return 1;
}

static int
build_line(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    if (len < 0 || (size_t)len >= n) {
        fprintf(stderr, "jtsettingsctl: command too long\n");
        return 0;
    }
    return 1;
}

int
main(int argc, char **argv)
{
    char line[256] = "";
    char path[256];
    if (!ctl_path(path, sizeof path)) {
        fprintf(stderr, "jtsettingsctl: control socket path too long\n");
        return 1;
    }

    if (argc < 2) {
        fprintf(stderr,
            "usage: %s accent '#RRGGBB' | page NAME | apply | reset | "
            "dump | quit\n", argv[0]);
        return 2;
    }

    int ok;
    if (!strcmp(argv[1], "accent") && argc >= 3) {
        if (!clean_arg(argv[2]))
            ok = 0;
        else
            ok = build_line(line, sizeof line, "accent %s\n", argv[2]);
    } else if (!strcmp(argv[1], "page") && argc >= 3) {
        if (!clean_arg(argv[2]))
            ok = 0;
        else
            ok = build_line(line, sizeof line, "page %s\n", argv[2]);
    } else if (!strcmp(argv[1], "apply") || !strcmp(argv[1], "reset") ||
               !strcmp(argv[1], "dump")) {
        ok = build_line(line, sizeof line, "%s\n", argv[1]);
    } else if (!strcmp(argv[1], "quit")) {
        ok = build_line(line, sizeof line, "quit\n");
    } else if (argv[1][0] == '#') {
        if (!clean_arg(argv[1]))
            ok = 0;
        else
            ok = build_line(line, sizeof line, "accent %s\n", argv[1]);
    } else {
        fprintf(stderr, "%s: unknown command '%s'\n", argv[0], argv[1]);
        return 2;
    }
    if (!ok)
        return 2;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("jtsettingsctl: socket");
        return 1;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "jtsettingsctl: path too long: %s\n", path);
        close(fd);
        return 1;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "jtsettingsctl: is jtsettings running? (%s)\n", path);
        close(fd);
        return 1;
    }

    size_t len = strlen(line);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, line + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("jtsettingsctl: write");
            close(fd);
            return 1;
        }
        if (n == 0) {
            fprintf(stderr, "jtsettingsctl: write failed\n");
            close(fd);
            return 1;
        }
        off += (size_t)n;
    }
    close(fd);
    return 0;
}
