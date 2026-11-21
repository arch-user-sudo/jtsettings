/*
 * jtstartup — run the autostart entries configured in jtsettings.
 *
 * jtsettings writes ~/.config/jt/startup.conf whenever a Startup Apps toggle
 * or the launch delay changes:
 *
 *   # jtsettings autostart configuration
 *   delay_ms=4000
 *   run jtlab
 *   off foot --server
 *
 * jtstartup waits delay_ms, then starts every `run` command detached from the
 * session through /bin/sh.  `off` entries are listed by jtsettings but skipped
 * here.  Missing files, blank lines and # comments are ignored, so running it
 * without a saved configuration is harmless.
 *
 *   jtstartup
 *   jtstartup --dry-run
 *   jtstartup --config /path/to/startup.conf
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CONFIG_DIR      "jt"
#define CONFIG_FILE     "startup.conf"
#define PATH_MAX_LEN    512
#define LINE_MAX_LEN    2048
#define MAX_APPS        64
#define MAX_DELAY_MS    60000
#define STAGGER_MS      150

/* Never read executable startup commands from a world-writable fallback. */
static int
config_path(char *buf, size_t n)
{
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        struct passwd *pw = getpwuid(getuid());
        home = (pw && pw->pw_dir && pw->pw_dir[0]) ? pw->pw_dir : NULL;
    }
    if (!home)
        return 0;
    int len = snprintf(buf, n, "%s/.config/%s/%s", home,
                       CONFIG_DIR, CONFIG_FILE);
    return len >= 0 && (size_t)len < n;
}

static char *
trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

static void
sleep_ms(long ms)
{
    if (ms <= 0) return;
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L,
    };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
    }
}

/* Start one command in its own session, with stdio detached. SIGCHLD is set
 * to SIG_IGN in main(), so the child is reaped automatically when it exits. */
static void
spawn(const char *cmd)
{
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "jtstartup: fork: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        setsid();
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, STDIN_FILENO);
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO) close(fd);
        }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

static void
usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--config PATH] [--dry-run] [--help]\n"
            "\n"
            "Runs the autostart commands saved by jtsettings.\n"
            "  --config PATH  read PATH instead of the default config\n"
            "  --dry-run      print the commands without starting them\n",
            argv0);
}

int
main(int argc, char **argv)
{
    const char *override = NULL;
    bool dry_run = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) {
            override = argv[++i];
        } else if (!strncmp(argv[i], "--config=", 9)) {
            override = argv[i] + 9;
        } else if (!strcmp(argv[i], "--dry-run")) {
            dry_run = true;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "%s: unknown argument '%s'\n", argv[0], argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    char path[PATH_MAX_LEN];
    if (override) {
        int len = snprintf(path, sizeof path, "%s", override);
        if (len < 0 || (size_t)len >= sizeof path) {
            fprintf(stderr, "jtstartup: config path too long\n");
            return 2;
        }
    } else if (!config_path(path, sizeof path)) {
        fprintf(stderr, "jtstartup: cannot determine home directory\n");
        return 1;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno != ENOENT)
            fprintf(stderr, "jtstartup: %s: %s\n", path, strerror(errno));
        return 0;
    }

    long delay_ms = 0;
    char *apps[MAX_APPS];
    int n_apps = 0;
    char line[LINE_MAX_LEN];

    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == '#') continue;

        if (!strncmp(s, "delay_ms=", 9)) {
            char *end = NULL;
            errno = 0;
            long ms = strtol(s + 9, &end, 10);
            if (end != s + 9 && errno == 0 && ms >= 0) {
                if (ms > MAX_DELAY_MS) ms = MAX_DELAY_MS;
                delay_ms = ms;
            }
            continue;
        }

        if (strncmp(s, "run ", 4) != 0) continue;
        char *cmd = trim(s + 4);
        if (!*cmd) continue;
        if (n_apps >= MAX_APPS) {
            fprintf(stderr, "jtstartup: too many entries in %s\n", path);
            break;
        }
        apps[n_apps] = strdup(cmd);
        if (!apps[n_apps]) {
            fprintf(stderr, "jtstartup: out of memory\n");
            break;
        }
        n_apps++;
    }
    fclose(f);

    if (n_apps == 0) {
        for (int i = 0; i < n_apps; i++) free(apps[i]);
        return 0;
    }

    if (dry_run) {
        printf("delay_ms=%ld\n", delay_ms);
        for (int i = 0; i < n_apps; i++)
            printf("run %s\n", apps[i]);
        for (int i = 0; i < n_apps; i++) free(apps[i]);
        return 0;
    }

    signal(SIGCHLD, SIG_IGN);
    sleep_ms(delay_ms);

    for (int i = 0; i < n_apps; i++) {
        spawn(apps[i]);
        if (i + 1 < n_apps) sleep_ms(STAGGER_MS);
    }

    for (int i = 0; i < n_apps; i++) free(apps[i]);
    return 0;
}
