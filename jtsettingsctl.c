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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static void
ctl_path(char *buf, size_t n)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !rt[0]) rt = "/tmp";
    snprintf(buf, n, "%s/jtsettings-ctl.sock", rt);
}

int
main(int argc, char **argv)
{
    char line[256] = "";
    char path[256];
    ctl_path(path, sizeof(path));

    if (argc < 2) {
        fprintf(stderr,
            "usage: %s accent '#RRGGBB' | page NAME | apply | reset | "
            "dump | quit\n", argv[0]);
        return 2;
    }

    if (!strcmp(argv[1], "accent") && argc >= 3)
        snprintf(line, sizeof(line), "accent %s\n", argv[2]);
    else if (!strcmp(argv[1], "page") && argc >= 3)
        snprintf(line, sizeof(line), "page %s\n", argv[2]);
    else if (!strcmp(argv[1], "apply") || !strcmp(argv[1], "reset") ||
             !strcmp(argv[1], "dump"))
        snprintf(line, sizeof(line), "%s\n", argv[1]);
    else if (!strcmp(argv[1], "quit"))
        snprintf(line, sizeof(line), "quit\n");
    else if (argv[1][0] == '#')
        snprintf(line, sizeof(line), "accent %s\n", argv[1]);
    else {
        fprintf(stderr, "%s: unknown command '%s'\n", argv[0], argv[1]);
        return 2;
    }

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
    ssize_t n = write(fd, line, len);
    close(fd);
    if (n < 0 || (size_t)n != len) {
        fprintf(stderr, "jtsettingsctl: write failed\n");
        return 1;
    }
    return 0;
}
