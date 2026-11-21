/*
 * jtsettings — a zen-styled settings app for jtlabwc.
 *
 * Wayland + cairo + Pango.  No X11, no GTK, no Qt.
 *
 *   make
 *   ./jtsettings
 *   ./jtsettings '#378484'          (optional accent override)
 *   ./jtsettings --page startup     (open a particular page)
 *   ./jtsettingsctl accent '#378484'
 *   ./jtstartup                    (run the saved startup apps)
 *
 * Most of this is a GUI preview: controls are drawn and react locally, but
 * Startup Apps writes ~/.config/jt/startup.conf (read by jtstartup) and the
 * new Profile page writes ~/.config/jt/profile.config (read by JTLab).  The
 * Profile tab picks the weather location and temperature unit, and chooses a
 * profile photo through the XDG file-chooser portal with a zoom step before
 * the choice is saved.  The left panel mirrors the areas a real settings app
 * would cover: keybindings, startup apps, window rules, input, displays,
 * session behaviour, profile and an about page.
 *
 * The palette and typography follow ~/JTLab/config.h: #030303 background,
 * #404040 borders, #E8A03C amber accent by default, #66B366 success green,
 * and the Liberation font.  The accent can be overridden at launch or changed
 * live through the control socket, which is how JTLab sends the wallpaper
 * accent to its companion windows.
 */

#define _GNU_SOURCE

#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <libsoup/soup.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-client-protocol.h"
#include "xdg-toplevel-icon-client-protocol.h"
#include "cursor-shape-client-protocol.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Look & feel                                                        */
/* ------------------------------------------------------------------ */

#define DESIGN_W 1080.0
#define DESIGN_H 720.0
static double WIN_W = DESIGN_W;
static double WIN_H = DESIGN_H;

#define SIDEBAR_W 252.0
#define HEADER_H  84.0

#define CONTENT_X (SIDEBAR_W + 36.0)
#define CONTENT_W (WIN_W - CONTENT_X - 36.0)

/* Displays page: the action row is anchored close to the bottom of the
 * content column, and the scrolling display-card viewport ends just above
 * it.  Shared so drawing, hit-testing and scrolling always agree. */
#define DISPLAY_BTN_Y       (WIN_H - 110.0)
#define DISPLAY_VIEW_TOP    118.0
#define DISPLAY_VIEW_BOTTOM (DISPLAY_BTN_Y - 10.0)

#define FONT_FAMILY "Liberation"

/* Startup Apps page.  The list is loaded from ~/.config/jt/startup.conf;
 * there are no built-in entries, so a fresh install starts empty. */
#define STARTUP_MAX     8
#define STARTUP_CMD_MAX 512

/* Default Apps page: discovered .desktop files and saved selections. */
#define MAX_DESKTOP_APPS 160
#define APP_ID_MAX       128
#define APP_NAME_MAX     160
#define APP_EXEC_MAX     256
#define APP_MIMES_MAX    1024
#define DEFAULT_APPS_FILE "defaultapps.conf"

/* Profile page: the file is shared with JTLab, which re-reads it when we send
 * "reload" to its control socket. */
#define PROFILE_CONFIG_FILE "profile.config"
#define PROFILE_PATH_MAX    512
#define WX_LOC_MAX          96
#define WX_QUERY_MAX        96
#define WX_RESULTS          6
#define WX_RESULT_NAME_MAX  128
#define PROFILE_ZOOM_MIN    1.0
#define PROFILE_ZOOM_MAX    4.0

#define C_BG        0x030303
#define C_BG_TOP    0x070708
#define C_SIDEBAR   0x060607
#define C_FG        0xFFFFFF
#define C_DIM       0x8C8C8C
#define C_FAINT     0x5C5C5C
#define C_BORDER    0x404040
#define C_CARD      0x0F1011
#define C_CARD_TOP  0x131416
#define C_TRACK     0x1A1A1C
#define C_GREEN     0x66B366
#define C_RED       0xC05A5A

/* The accent can be overridden at launch or by the control socket.  These
 * aliases keep the drawing code unchanged while allowing live replacement. */
static unsigned accent_rgb = 0xE8A03C;
static unsigned accent_hi_rgb = 0xF2B25C;
static unsigned accent_lo_rgb = 0xCE8A2F;
static unsigned on_accent_rgb = 0x14100A;
#define C_ACCENT    accent_rgb
#define C_ACCENT_HI accent_hi_rgb
#define C_ACCENT_LO accent_lo_rgb
#define C_ON_ACCENT on_accent_rgb

#define PI 3.14159265358979323846

#define R_(h) ((((h) >> 16) & 0xFF) / 255.0)
#define G_(h) ((((h) >> 8) & 0xFF) / 255.0)
#define B_(h) (((h) & 0xFF) / 255.0)

static void rgba(cairo_t *cr, unsigned hex, double a)
{
    cairo_set_source_rgba(cr, R_(hex), G_(hex), B_(hex), a);
}

static void rounded_rect(cairo_t *cr, double x, double y,
                         double w, double h, double r)
{
    double d = 2.0 * r;
    if (d > w) d = w;
    if (d > h) d = h;
    r = d / 2.0;

    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -PI / 2.0, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, PI / 2.0, PI);
    cairo_arc(cr, x + r, y + r, r, PI, 3.0 * PI / 2.0);
    cairo_close_path(cr);
}

static double mono_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double clampd(double v, double lo, double hi)
{
    if (!isfinite(v))
        return lo;
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ------------------------------------------------------------------ */
/*  Accent colour                                                      */
/* ------------------------------------------------------------------ */

static int accent_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int accent_parse(const char *s, unsigned *out)
{
    if (!s)
        return 0;
    if (*s == '#')
        s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    if (strlen(s) != 6)
        return 0;

    unsigned value = 0;
    for (int i = 0; i < 6; i++) {
        int nibble = accent_hex_nibble(s[i]);
        if (nibble < 0)
            return 0;
        value = (value << 4) | (unsigned)nibble;
    }
    *out = value;
    return 1;
}

static double accent_linear_channel(unsigned channel)
{
    double c = channel / 255.0;
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double accent_luminance(unsigned rgb)
{
    return 0.2126 * accent_linear_channel((rgb >> 16) & 0xFF) +
           0.7152 * accent_linear_channel((rgb >> 8) & 0xFF) +
           0.0722 * accent_linear_channel(rgb & 0xFF);
}

static unsigned accent_ink(unsigned rgb)
{
    return accent_luminance(rgb) > 0.42 ? 0x14100A : 0xFFFFFF;
}

static unsigned accent_mix(unsigned rgb, unsigned toward, double amount)
{
    amount = clampd(amount, 0.0, 1.0);
    unsigned r = (unsigned)lround(((rgb >> 16) & 0xFF) * (1.0 - amount) +
                                  ((toward >> 16) & 0xFF) * amount);
    unsigned g = (unsigned)lround(((rgb >> 8) & 0xFF) * (1.0 - amount) +
                                  ((toward >> 8) & 0xFF) * amount);
    unsigned b = (unsigned)lround((rgb & 0xFF) * (1.0 - amount) +
                                  (toward & 0xFF) * amount);
    return (r << 16) | (g << 8) | b;
}

static void accent_set(unsigned rgb)
{
    accent_rgb = rgb;
    accent_hi_rgb = accent_mix(rgb, 0xFFFFFF, 0.16);
    accent_lo_rgb = accent_mix(rgb, 0x000000, 0.14);
    on_accent_rgb = accent_ink(rgb);
}

/* Remove a leading '#RRGGBB', a bare RRGGBB, or an explicit --accent COLOR. */
static int accent_cli(int *argc, char **argv)
{
    for (int i = 1; i < *argc; i++) {
        const char *value = NULL;
        int consumed = 0;

        if (!strcmp(argv[i], "--accent")) {
            if (i + 1 >= *argc) {
                fprintf(stderr, "%s: --accent requires a color\n", argv[0]);
                return 0;
            }
            value = argv[i + 1];
            consumed = 2;
        } else if (!strncmp(argv[i], "--accent=", 9)) {
            value = argv[i] + 9;
            consumed = 1;
        } else if (argv[i][0] == '#') {
            value = argv[i];
            consumed = 1;
        } else if (i == 1) {
            unsigned candidate;
            if (accent_parse(argv[i], &candidate)) {
                value = argv[i];
                consumed = 1;
            }
        }

        if (!value)
            continue;

        unsigned rgb;
        if (!accent_parse(value, &rgb)) {
            fprintf(stderr, "%s: invalid accent '%s' (expected #RRGGBB)\n",
                    argv[0], value);
            return 0;
        }
        accent_set(rgb);

        for (int j = i; j + consumed < *argc; j++)
            argv[j] = argv[j + consumed];
        *argc -= consumed;
        i--;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Text                                                               */
/* ------------------------------------------------------------------ */

enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

typedef struct {
    int key;
    PangoFontDescription *fd;
} FontEnt;

#define FONT_CACHE 256
static FontEnt g_fonts[FONT_CACHE];
static int g_font_count;

static PangoFontDescription *font_get(double size, int weight)
{
    int key = (int)lround(size * 10.0) * 10000 + weight;

    for (int i = 0; i < g_font_count; i++)
        if (g_fonts[i].key == key)
            return g_fonts[i].fd;

    PangoFontDescription *fd = pango_font_description_new();
    pango_font_description_set_family(fd, FONT_FAMILY);
    pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
    pango_font_description_set_weight(fd, (PangoWeight)weight);

    if (g_font_count < FONT_CACHE) {
        g_fonts[g_font_count].key = key;
        g_fonts[g_font_count].fd = fd;
        g_font_count++;
    }
    return fd;
}

static PangoLayout *layout_make(cairo_t *cr, const char *text,
                                double size, int weight, double spacing)
{
    PangoLayout *l = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(l, font_get(size, weight));
    pango_layout_set_text(l, text, -1);

    if (spacing > 0.0) {
        PangoAttrList *attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs,
            pango_attr_letter_spacing_new((int)lround(spacing * PANGO_SCALE)));
        pango_layout_set_attributes(l, attrs);
        pango_attr_list_unref(attrs);
    }
    return l;
}

static double text_width(cairo_t *cr, const char *text, double size,
                         int weight, double spacing)
{
    if (!text || !*text)
        return 0.0;
    PangoLayout *l = layout_make(cr, text, size, weight, spacing);
    PangoRectangle logical;
    pango_layout_get_pixel_extents(l, NULL, &logical);
    double w = logical.width;
    g_object_unref(l);
    return w;
}

static double text_draw(cairo_t *cr, const char *text, double size, int weight,
                        unsigned hex, double alpha, double x, double y,
                        int align, double spacing)
{
    if (!text || !*text)
        return 0.0;

    PangoLayout *l = layout_make(cr, text, size, weight, spacing);
    PangoRectangle logical;
    pango_layout_get_pixel_extents(l, NULL, &logical);

    double w = logical.width;
    if (align == ALIGN_CENTER)
        x -= w / 2.0;
    else if (align == ALIGN_RIGHT)
        x -= w;

    cairo_move_to(cr, x, y);
    rgba(cr, hex, alpha);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    return w;
}

/* Word-wrapped text inside a fixed width.  Returns the height used. */
static double text_wrap(cairo_t *cr, const char *text, double size, int weight,
                        unsigned hex, double alpha, double x, double y,
                        double width, int max_lines)
{
    if (!text || !*text)
        return 0.0;

    PangoLayout *l = layout_make(cr, text, size, weight, 0.0);
    pango_layout_set_width(l, (int)lround(width * PANGO_SCALE));
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
    pango_layout_set_height(l, max_lines > 0 ? max_lines : -1);

    cairo_move_to(cr, x, y);
    rgba(cr, hex, alpha);
    pango_cairo_show_layout(cr, l);

    PangoRectangle logical;
    pango_layout_get_pixel_extents(l, NULL, &logical);
    double h = logical.height;
    g_object_unref(l);
    return h;
}

/* ------------------------------------------------------------------ */
/*  Pages and local UI state                                           */
/* ------------------------------------------------------------------ */

enum {
    PAGE_KEYBINDINGS,
    PAGE_STARTUP,
    PAGE_DEFAULT_APPS,
    PAGE_RULES,
    PAGE_INPUT,
    PAGE_DISPLAYS,
    PAGE_SESSION,
    PAGE_PROFILE,
    PAGE_ABOUT,
    PAGE_COUNT
};

static const char *PAGE_NAMES[PAGE_COUNT] = {
    "Keybindings", "Startup Apps", "Default Apps", "Window Rules", "Input",
    "Displays", "Session", "Profile", "About",
};

static const char *PAGE_SUBS[PAGE_COUNT] = {
    "Every shortcut jtlabwc listens for",
    "Programs launched together with your session",
    "Handlers for links, files, media and the terminal",
    "Match windows and shape how they appear",
    "Pointer, touchpad and keyboard behaviour",
    "Outputs, resolution, refresh rate and scale",
    "Idle, lock and power commands",
    "Weather location, units and your JTLab avatar",
    "Version and build information",
};

enum {
    TOG_NATURAL,
    TOG_TAP,
    TOG_DRAG,
    TOG_DWT,
    TOG_LEFT,
    TOG_RULE_PIP,
    TOG_RULE_TERM,
    TOG_RULE_WEB,
    TOG_MIDDLE,
    TOG_IDLE_LOCK,
    TOG_DIM,
    TOG_LOCK_SUSPEND,
    TOG_RESTORE,
    TOG_LAST_WS,
    TOG_PRIMARY_A,
    TOG_PRIMARY_B,
    TOG_COUNT
};

enum {
    SL_POINTER,
    SL_REPEAT,
    SL_DELAY,
    SL_START_DELAY,
    SL_IDLE,
    SL_PHOTO_ZOOM,
    SL_COUNT
};

enum {
    SEG_SCROLL,
    SEG_ACCEL,
    SEG_KB_FILTER,
    SEG_REFRESH_A,
    SEG_SCALE_A,
    SEG_ORIENT_A,
    SEG_REFRESH_B,
    SEG_SCALE_B,
    SEG_ORIENT_B,
    SEG_WX_UNITS,
    SEG_COUNT
};

enum {
    HIT_NONE,
    HIT_NAV,
    HIT_TOGGLE,
    HIT_SLIDER,
    HIT_SEGMENT,
    HIT_ROW,
    HIT_BUTTON,
    HIT_START_TOGGLE,
    HIT_TERM_FIELD,
    HIT_DISP_RES,
    HIT_DISP_REFRESH,
    HIT_DISP_SCALE,
    HIT_DISP_ORIENT,
    HIT_DISP_ENABLE,
    HIT_DISP_PRIMARY,
    HIT_DISP_MODE,
    HIT_APP_FIELD,
    HIT_APP_PICK,
    HIT_APP_PICKER_CLOSE,
    HIT_WX_FIELD,
    HIT_WX_QUERY_FIELD,
    HIT_WX_RESULT,
    HIT_ZOOM_SCRIM
};

enum {
    BTN_NEW_RULE,
    BTN_DOCS,
    BTN_UPDATE,
    BTN_LOGOUT,
    BTN_REBOOT,
    BTN_SHUTDOWN,
    BTN_START_ADD,
    BTN_POSITION,
    BTN_POS_DONE,
    BTN_DISPLAYS_APPLY,
    BTN_DISPLAYS_RESET,
    BTN_MODE_CLOSE,
    BTN_WX_SEARCH,
    BTN_PHOTO_PICK,
    BTN_PHOTO_CLEAR,
    BTN_ZOOM_CANCEL,
    BTN_ZOOM_SAVE,

    /* Dynamic IDs must come after every fixed button ID; otherwise a
     * startup Remove button can swallow an unrelated button press. */
    BTN_START_REMOVE_FIRST,   /* + row index */
    BTN_START_REMOVE_LAST = BTN_START_REMOVE_FIRST + STARTUP_MAX - 1
};

typedef struct {
    double x, y, w, h;
    int kind;
    int a, b;
} Hit;

#define MAX_HITS 192
static Hit g_hits[MAX_HITS];
static int g_hit_count;

typedef struct {
    const char *key;
    const char *label;
    const char *desc;
    const char *mimes[4];
    int mime_count;
    const char *preferred;
    int terminal;
} AppCategory;

static const AppCategory APP_CATEGORIES[] = {
    { "terminal", "Terminal", "Used by Super + Return",
      { NULL }, 0, "footclient.desktop", 1 },
    { "browser", "Web browser", "HTTP links and HTML pages",
      { "text/html", "x-scheme-handler/http", "x-scheme-handler/https" },
      3, "vivaldi-stable.desktop", 0 },
    { "filemanager", "File manager", "Folders and mounted drives",
      { "inode/directory" }, 1, "nemo.desktop", 0 },
    { "texteditor", "Text editor", "Plain text and source files",
      { "text/plain" }, 1, "nvim.desktop", 0 },
    { "image", "Image viewer", "Photos and pictures",
      { "image/png", "image/jpeg" }, 2, "viewnior.desktop", 0 },
    { "video", "Video player", "Movies and clips",
      { "video/mp4", "video/x-matroska" }, 2, "mpv.desktop", 0 },
    { "audio", "Music player", "Audio tracks and playlists",
      { "audio/mpeg", "audio/flac" }, 2, "mpv.desktop", 0 },
    { "pdf", "PDF viewer", "Portable documents",
      { "application/pdf" }, 1, "org.pwmt.zathura.desktop", 0 },
};
#define APP_CAT_COUNT ((int)(sizeof APP_CATEGORIES / sizeof APP_CATEGORIES[0]))

typedef struct {
    char id[APP_ID_MAX];
    char name[APP_NAME_MAX];
    char exec[APP_EXEC_MAX];
    char mimes[APP_MIMES_MAX];
} DesktopApp;

static DesktopApp g_desktop_apps[MAX_DESKTOP_APPS];
static int g_desktop_app_count;
static char g_default_apps[APP_CAT_COUNT][APP_ID_MAX];

/* Profile page state.  g_profile_cfg mirrors ~/.config/jt/profile.config;
 * the search results and pending photo are local to this session. */
typedef struct {
    char name[WX_RESULT_NAME_MAX];
    double lat, lon;
} WxResult;

typedef struct {
    char location[WX_LOC_MAX];
    char photo[PROFILE_PATH_MAX];
    double zoom;
    int fahrenheit;
    int have_coords;
    double lat, lon;
} ProfileCfg;

static ProfileCfg g_profile_cfg;
static WxResult g_wx_results[WX_RESULTS];
static int g_wx_result_count;
static int g_wx_searching;
static uint64_t g_wx_gen;
typedef struct {
    uint64_t gen;
    SoupMessage *msg;
} WxSearchOp;
static char g_pending_photo[PROFILE_PATH_MAX];
static double g_pending_zoom;
static int g_zoom_open;
static cairo_surface_t *g_photo_preview;
/* Async portal/search callbacks run inside our GLib main context on the same
 * thread; they set this so the Wayland loop repaints. */
static int g_profile_dirty;

typedef struct {
    int page;
    double t;
    int toggles[TOG_COUNT];
    double sliders[SL_COUNT];
    int segments[SEG_COUNT];
    int selected[PAGE_COUNT];
    int hover;
    int pressed;
    double pointer_x, pointer_y;
    int startup_adding;
    char startup_input[STARTUP_CMD_MAX];
    int terminal_editing;
    char terminal_input[128];
    char terminal[128];
    double kb_scroll;
    int kb_drag;
    int kb_drag_moved;
    double kb_drag_y;
    double kb_drag_scroll;
    char toast[96];
    double toast_t;
    int pos_editor_open;
    int pos_layout_valid;
    int mode_picker_open;
    int mode_picker_head;
    int app_picker_open;
    int app_picker_cat;
    int app_picker_scroll;
    int disp_scroll;
    int disp_drag;
    double disp_drag_ox;
    double disp_drag_oy;
    double pos_scale;
    double pos_offx;
    double pos_offy;
    int wx_editing;
    char wx_location_input[WX_LOC_MAX];
    int wx_query_editing;
    char wx_query_input[WX_QUERY_MAX];
} Ui;

static Ui g_ui;

/* ------------------------------------------------------------------ */
/*  Output management                                                  */
/* ------------------------------------------------------------------ */

typedef struct App App;

#define MAX_HEADS 8
#define MAX_MODES 64
#define MAX_RESOLUTIONS 24

typedef struct {
    struct zwlr_output_mode_v1 *obj;
    int32_t width, height, refresh;  /* refresh in mHz */
    int preferred;
    int available;
} HeadMode;

typedef struct {
    int used;
    struct zwlr_output_head_v1 *head;
    char name[64];
    char description[160];
    char make[64];
    char model[64];
    int32_t phys_w, phys_h;
    int enabled;
    int32_t x, y;
    int32_t transform;
    double scale;
    int32_t cur_w, cur_h, cur_refresh;
    struct zwlr_output_mode_v1 *current_mode;
    HeadMode modes[MAX_MODES];
    int mode_count;

    /* Pending configuration, edited in the UI and applied as one config. */
    int p_enabled;
    int p_mode;             /* index into modes, -1 when unknown */
    double p_scale;
    int32_t p_transform;
    int32_t p_x, p_y;
    int pending_ready;
    int dirty;
} Head;

typedef struct {
    int32_t w, h;
} Resolution;

static Head g_heads[MAX_HEADS];
static App *g_app;
static int g_output_manager_available;

/* Defined below, once the startup list is in scope. */
static int startup_load(void);
static void startup_save(void);

/* Pointer, touchpad and keyboard repeat settings live in
 * ~/.config/jt/input.conf and are applied by jtlabwc over its control
 * socket. */
static int input_load(void);
static void input_save(void);
static void jtlabwc_reload(void);
static void input_commit(void);
static int input_toggle(int toggle);

/* The default terminal command lives in ~/.config/jt/keybinds.conf. */
static int keybinds_load(void);
static void keybinds_save(void);
static void terminal_input_commit(void);
static void terminal_input_cancel(void);

/* Default app discovery and persistence, defined with the config code. */
static void defaultapps_mark_dirty(void);
static void defaultapps_load(void);
static void defaultapps_save(void);
static void defaultapps_choose(int cat, int candidate);

/* Profile page / shared configuration. */
static void profile_config_load(void);
static void profile_save(void);
static int profile_preview_load(const char *path);
static void jtlab_reload(void);
static void profile_open_picker(void);
static void wx_search_start(void);
static void glib_services_init(void);
static void glib_services_cleanup(void);
static void zoom_cancel(void);

static void ui_toast(const char *msg)
{
    snprintf(g_ui.toast, sizeof g_ui.toast, "%s", msg);
    g_ui.toast_t = mono_sec();
}

/* Page changes must not leave a modal, text field, or drag owning input. */
static void ui_clear_interaction(void)
{
    g_ui.hover = -1;
    g_ui.pressed = -1;
    /* Page/modality changes invalidate the hit list from the previous paint. */
    g_hit_count = 0;
    g_ui.startup_adding = 0;
    g_ui.terminal_editing = 0;
    g_ui.wx_editing = 0;
    g_ui.wx_query_editing = 0;
    g_ui.mode_picker_open = 0;
    g_ui.mode_picker_head = -1;
    g_ui.pos_editor_open = 0;
    g_ui.pos_layout_valid = 0;
    g_ui.app_picker_open = 0;
    g_ui.app_picker_scroll = 0;
    g_ui.disp_drag = -1;
    g_ui.kb_drag = 0;
    if (g_zoom_open)
        zoom_cancel();
}

static void ui_set_page(int page)
{
    if (page < 0 || page >= PAGE_COUNT || page == g_ui.page)
        return;
    ui_clear_interaction();
    g_ui.page = page;
}

static void ui_init(void)
{
    memset(&g_ui, 0, sizeof g_ui);
    g_ui.page = PAGE_KEYBINDINGS;
    g_ui.hover = -1;
    g_ui.pressed = -1;
    g_ui.pointer_x = -1;
    g_ui.pointer_y = -1;
    g_ui.toast_t = -1.0;
    g_ui.pos_editor_open = 0;
    g_ui.pos_layout_valid = 0;
    g_ui.mode_picker_open = 0;
    g_ui.mode_picker_head = -1;
    g_ui.app_picker_open = 0;
    g_ui.app_picker_cat = -1;
    g_ui.app_picker_scroll = 0;
    g_ui.disp_scroll = 0;
    g_ui.disp_drag = -1;
    g_ui.disp_drag_ox = 0.0;
    g_ui.disp_drag_oy = 0.0;
    g_ui.pos_scale = 0.2;
    g_ui.pos_offx = 0.0;
    g_ui.pos_offy = 0.0;

    g_ui.toggles[TOG_NATURAL] = 1;
    g_ui.toggles[TOG_TAP] = 1;
    g_ui.toggles[TOG_DRAG] = 1;
    g_ui.toggles[TOG_DWT] = 1;
    g_ui.toggles[TOG_LEFT] = 0;
    g_ui.toggles[TOG_RULE_PIP] = 1;
    g_ui.toggles[TOG_RULE_TERM] = 1;
    g_ui.toggles[TOG_RULE_WEB] = 0;
    g_ui.toggles[TOG_MIDDLE] = 0;
    g_ui.toggles[TOG_IDLE_LOCK] = 1;
    g_ui.toggles[TOG_DIM] = 1;
    g_ui.toggles[TOG_LOCK_SUSPEND] = 1;
    g_ui.toggles[TOG_RESTORE] = 1;
    g_ui.toggles[TOG_LAST_WS] = 0;
    g_ui.toggles[TOG_PRIMARY_A] = 1;
    g_ui.toggles[TOG_PRIMARY_B] = 0;

    g_ui.sliders[SL_POINTER] = 0.52;
    g_ui.sliders[SL_REPEAT] = (45.0 - 15.0) / 65.0;
    g_ui.sliders[SL_DELAY] = (250.0 - 150.0) / 550.0;
    g_ui.sliders[SL_START_DELAY] = 0.0;
    g_ui.sliders[SL_IDLE] = 0.45;

    g_ui.segments[SEG_SCROLL] = 0;
    g_ui.segments[SEG_KB_FILTER] = 0;
    g_ui.segments[SEG_REFRESH_A] = 1;
    g_ui.segments[SEG_SCALE_A] = 0;
    g_ui.segments[SEG_ORIENT_A] = 0;
    g_ui.segments[SEG_REFRESH_B] = 0;
    g_ui.segments[SEG_SCALE_B] = 0;
    g_ui.segments[SEG_ORIENT_B] = 0;

    for (int i = 0; i < PAGE_COUNT; i++)
        g_ui.selected[i] = 0;

    /* A saved autostart configuration overrides the defaults above.  With no
     * file, every startup entry starts disabled rather than showing preview
     * defaults; enabling one writes the file. */
    (void)startup_load();

    /* Saved pointer settings override the defaults in the same way. */
    (void)input_load();

    /* Default terminal command; falls back to footclient. */
    (void)keybinds_load();

    /* Installed application desktop files and XDG defaults. */
    defaultapps_load();

    /* Profile page: fallbacks first, then the shared config file. */
    memset(&g_profile_cfg, 0, sizeof g_profile_cfg);
    snprintf(g_profile_cfg.location, sizeof g_profile_cfg.location,
             "Kimberley");
    g_profile_cfg.zoom = 1.0;
    g_wx_result_count = 0;
    g_wx_searching = 0;
    g_zoom_open = 0;
    g_photo_preview = NULL;
    profile_config_load();

    g_ui.sliders[SL_PHOTO_ZOOM] =
        (g_profile_cfg.zoom - PROFILE_ZOOM_MIN) /
        (PROFILE_ZOOM_MAX - PROFILE_ZOOM_MIN);
    g_ui.segments[SEG_WX_UNITS] = g_profile_cfg.fahrenheit ? 1 : 0;
    if (g_profile_cfg.photo[0])
        profile_preview_load(g_profile_cfg.photo);
}

/* ---- display helpers --------------------------------------------- */

static Head *head_get(int idx)
{
    if (idx < 0 || idx >= MAX_HEADS || !g_heads[idx].used)
        return NULL;
    return &g_heads[idx];
}

static int head_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_HEADS; i++)
        if (g_heads[i].used)
            n++;
    return n;
}

static int head_enabled_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_HEADS; i++)
        if (g_heads[i].used && g_heads[i].p_enabled)
            n++;
    return n;
}

static int head_mode_valid(const Head *h)
{
    return h->p_mode >= 0 && h->p_mode < h->mode_count &&
           h->modes[h->p_mode].available &&
           h->modes[h->p_mode].width > 0;
}

static void head_logical_size(const Head *h, double *w, double *hgt)
{
    double mw = h->cur_w > 0 ? (double)h->cur_w : 1920.0;
    double mh = h->cur_h > 0 ? (double)h->cur_h : 1080.0;
    if (head_mode_valid(h)) {
        mw = (double)h->modes[h->p_mode].width;
        mh = (double)h->modes[h->p_mode].height;
    }
    double s = h->p_scale > 0.0 ? h->p_scale : 1.0;
    mw /= s;
    mh /= s;
    if (h->p_transform == 1 || h->p_transform == 3) {
        double t = mw;
        mw = mh;
        mh = t;
    }
    *w = mw;
    *hgt = mh;
}

static int head_resolutions(const Head *h, Resolution *out, int max)
{
    int n = 0;
    for (int i = 0; i < h->mode_count && n < max; i++) {
        const HeadMode *m = &h->modes[i];
        if (!m->available || m->width <= 0 || m->height <= 0)
            continue;
        int seen = 0;
        for (int j = 0; j < n; j++) {
            if (out[j].w == m->width && out[j].h == m->height) {
                seen = 1;
                break;
            }
        }
        if (!seen) {
            out[n].w = m->width;
            out[n].h = m->height;
            n++;
        }
    }
    return n;
}

static int head_refreshes(const Head *h, int32_t w, int32_t hgt,
                          int32_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < h->mode_count && n < max; i++) {
        const HeadMode *m = &h->modes[i];
        if (!m->available || m->width != w || m->height != hgt)
            continue;
        int seen = 0;
        for (int j = 0; j < n; j++) {
            if (out[j] == m->refresh) {
                seen = 1;
                break;
            }
        }
        if (!seen)
            out[n++] = m->refresh;
    }
    /* Lowest to highest reads better in the segmented control. */
    for (int i = 1; i < n; i++) {
        int32_t v = out[i];
        int j = i - 1;
        while (j >= 0 && out[j] > v) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = v;
    }
    return n;
}

static int head_mode_index(const Head *h, int32_t w, int32_t hgt,
                           int32_t refresh)
{
    for (int i = 0; i < h->mode_count; i++) {
        const HeadMode *m = &h->modes[i];
        if (m->available && m->width == w && m->height == hgt &&
            m->refresh == refresh)
            return i;
    }
    return -1;
}

static void head_choose_resolution(Head *h, int32_t w, int32_t hgt)
{
    int32_t want = h->cur_refresh;
    if (head_mode_valid(h))
        want = h->modes[h->p_mode].refresh;

    int pick = -1;
    int32_t best_refresh = -1;
    for (int i = 0; i < h->mode_count; i++) {
        const HeadMode *m = &h->modes[i];
        if (!m->available || m->width != w || m->height != hgt)
            continue;
        if (m->refresh == want) {
            pick = i;
            break;
        }
        if (m->refresh > best_refresh) {
            best_refresh = m->refresh;
            pick = i;
        }
    }
    if (pick >= 0) {
        h->p_mode = pick;
        h->dirty = 1;
    }
}

static void head_choose_refresh(Head *h, int32_t w, int32_t hgt,
                                int32_t refresh)
{
    int idx = head_mode_index(h, w, hgt, refresh);
    if (idx >= 0) {
        h->p_mode = idx;
        h->dirty = 1;
    }
}

static const double DISP_SCALES[] = { 1.0, 1.25, 1.5, 2.0 };
static const char *const DISP_SCALE_LABELS[] = {
    "100%", "125%", "150%", "200%"
};
#define DISP_SCALE_COUNT ((int)(sizeof DISP_SCALES / sizeof DISP_SCALES[0]))

static int head_scale_index(const Head *h)
{
    int best = 0;
    double dist = 1e9;
    for (int i = 0; i < DISP_SCALE_COUNT; i++) {
        double d = fabs(h->p_scale - DISP_SCALES[i]);
        if (d < dist) {
            dist = d;
            best = i;
        }
    }
    return best;
}

static int head_orient_index(const Head *h)
{
    return (h->p_transform == 1 || h->p_transform == 3) ? 1 : 0;
}

static int head_is_primary(int idx)
{
    Head *h = head_get(idx);
    return h && h->p_enabled && h->p_x == 0 && h->p_y == 0;
}

static void displays_sync_pending(Head *h)
{
    h->p_enabled = h->enabled;
    h->p_scale = h->scale > 0.0 ? h->scale : 1.0;
    h->p_transform = h->transform;
    h->p_x = h->x;
    h->p_y = h->y;
    h->p_mode = -1;
    for (int i = 0; i < h->mode_count; i++) {
        const HeadMode *m = &h->modes[i];
        if (m->available && m->width == h->cur_w &&
            m->height == h->cur_h && m->refresh == h->cur_refresh) {
            h->p_mode = i;
            break;
        }
    }
    h->dirty = 0;
    h->pending_ready = 1;
}

static void displays_sync_all(void)
{
    for (int i = 0; i < MAX_HEADS; i++)
        if (g_heads[i].used)
            displays_sync_pending(&g_heads[i]);
}

static void displays_make_primary(int idx)
{
    Head *h = head_get(idx);
    if (!h)
        return;
    if (!h->p_enabled) {
        h->p_enabled = 1;
        h->dirty = 1;
    }
    if (h->p_x == 0 && h->p_y == 0)
        return;

    int32_t old_x = h->p_x, old_y = h->p_y;
    for (int i = 0; i < MAX_HEADS; i++) {
        Head *o = &g_heads[i];
        if (i == idx || !o->used || !o->p_enabled)
            continue;
        if (o->p_x == 0 && o->p_y == 0) {
            o->p_x = old_x;
            o->p_y = old_y;
            o->dirty = 1;
            break;
        }
    }
    h->p_x = 0;
    h->p_y = 0;
    h->dirty = 1;
}

static void displays_toggle_enabled(int idx)
{
    Head *h = head_get(idx);
    if (!h)
        return;
    if (h->p_enabled && head_enabled_count() <= 1) {
        ui_toast("At least one display must stay enabled");
        return;
    }
    h->p_enabled = !h->p_enabled;
    h->dirty = 1;

    if (h->p_enabled && !head_mode_valid(h)) {
        int pick = -1;
        for (int i = 0; i < h->mode_count; i++) {
            const HeadMode *m = &h->modes[i];
            if (!m->available || m->width <= 0)
                continue;
            if (m->preferred) {
                pick = i;
                break;
            }
            if (pick < 0)
                pick = i;
        }
        if (pick >= 0)
            h->p_mode = pick;
    }
}


static int add_hit(int kind, int a, int b,
                   double x, double y, double w, double h)
{
    /* Display cards scroll inside a clipped viewport.  Do not register
     * controls that are scrolled underneath the header or action row. */
    if (kind >= HIT_DISP_RES && kind <= HIT_DISP_MODE &&
        !g_ui.mode_picker_open &&
        (y < DISPLAY_VIEW_TOP || y + h > DISPLAY_VIEW_BOTTOM))
        return -1;

    if (g_hit_count >= MAX_HITS)
        return -1;
    Hit *hit = &g_hits[g_hit_count];
    hit->kind = kind;
    hit->a = a;
    hit->b = b;
    hit->x = x;
    hit->y = y;
    hit->w = w;
    hit->h = h;
    return g_hit_count++;
}

static int hit_at(double x, double y)
{
    for (int i = g_hit_count - 1; i >= 0; i--) {
        Hit *h = &g_hits[i];
        if (x >= h->x && x <= h->x + h->w &&
            y >= h->y && y <= h->y + h->h)
            return i;
    }
    return -1;
}

static int hit_hover(int idx)
{
    return idx >= 0 && idx == g_ui.hover;
}

/* ------------------------------------------------------------------ */
/*  Small widgets                                                      */
/* ------------------------------------------------------------------ */

static void paint_card(cairo_t *cr, double x, double y, double w, double h)
{
    cairo_pattern_t *fill = cairo_pattern_create_linear(x, y, x, y + h);
    cairo_pattern_add_color_stop_rgba(fill, 0.0,
        R_(C_CARD_TOP), G_(C_CARD_TOP), B_(C_CARD_TOP), 0.96);
    cairo_pattern_add_color_stop_rgba(fill, 1.0,
        R_(0x0A0A0C), G_(0x0A0A0C), B_(0x0A0A0C), 0.96);
    cairo_set_source(cr, fill);
    rounded_rect(cr, x, y, w, h, 10.0);
    cairo_fill(cr);
    cairo_pattern_destroy(fill);

    rgba(cr, C_BORDER, 0.42);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0, 9.5);
    cairo_stroke(cr);
}

static void section_label(cairo_t *cr, const char *text, double x, double y)
{
    text_draw(cr, text, 9.5, 600, C_FAINT, 0.95,
              x, y, ALIGN_LEFT, 1.6);
}

static void paint_toggle_row(cairo_t *cr, double x, double y, double w,
                             const char *label, const char *desc,
                             int on, int toggle_id)
{
    int idx = add_hit(HIT_TOGGLE, toggle_id, 0, x, y - 4.0, w, 40.0);
    int hot = hit_hover(idx);

    text_draw(cr, label, 12.0, 600, hot ? C_FG : 0xD8D8D8, 0.96,
              x, y, ALIGN_LEFT, 0.1);
    if (desc && *desc)
        text_draw(cr, desc, 9.3, 400, C_DIM, 0.78,
                  x, y + 17.0, ALIGN_LEFT, 0.1);

    double tw = 38.0, th = 20.0;
    double tx = x + w - tw;
    double ty = y + 1.0;

    rgba(cr, on ? C_ACCENT : C_TRACK, on ? (hot ? 0.95 : 0.85) : 0.9);
    rounded_rect(cr, tx, ty, tw, th, th / 2.0);
    cairo_fill(cr);

    if (!on) {
        rgba(cr, C_BORDER, 0.55);
        cairo_set_line_width(cr, 1.0);
        rounded_rect(cr, tx + 0.5, ty + 0.5, tw - 1.0, th - 1.0,
                     th / 2.0 - 0.5);
        cairo_stroke(cr);
    }

    double kx = on ? tx + tw - th / 2.0 : tx + th / 2.0;
    rgba(cr, on ? C_ON_ACCENT : C_DIM, on ? 1.0 : 0.75);
    cairo_arc(cr, kx, ty + th / 2.0, 6.4, 0.0, 2.0 * PI);
    cairo_fill(cr);
}

static void paint_slider_row(cairo_t *cr, double x, double y, double w,
                             const char *label, const char *value,
                             double t, int slider_id)
{
    int idx = add_hit(HIT_SLIDER, slider_id, 0, x - 6.0, y + 8.0,
                      w + 12.0, 24.0);
    int hot = hit_hover(idx);

    text_draw(cr, label, 11.5, 500, hot ? C_FG : 0xD2D2D2, 0.94,
              x, y, ALIGN_LEFT, 0.1);
    text_draw(cr, value, 10.0, 500, C_ACCENT, 0.95,
              x + w, y + 0.5, ALIGN_RIGHT, 0.2);

    double ty = y + 23.0;
    rgba(cr, C_TRACK, 1.0);
    rounded_rect(cr, x, ty - 2.0, w, 4.0, 2.0);
    cairo_fill(cr);

    double fill = clampd(t, 0.0, 1.0) * w;
    rgba(cr, C_ACCENT, 0.92);
    rounded_rect(cr, x, ty - 2.0, fill, 4.0, 2.0);
    cairo_fill(cr);

    double kx = x + fill;
    rgba(cr, C_ACCENT, 0.16);
    cairo_arc(cr, kx, ty, hot ? 9.0 : 7.5, 0.0, 2.0 * PI);
    cairo_fill(cr);
    rgba(cr, C_FG, 0.98);
    cairo_arc(cr, kx, ty, 3.6, 0.0, 2.0 * PI);
    cairo_fill(cr);
}

static void paint_segment_ex(cairo_t *cr, double x, double y, double w,
                             double h, const char *const *items, int count,
                             int sel, int kind, int a)
{
    if (count <= 0)
        return;

    rgba(cr, C_TRACK, 0.95);
    rounded_rect(cr, x, y, w, h, h / 2.0);
    cairo_fill(cr);
    rgba(cr, C_BORDER, 0.40);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0, h / 2.0 - 0.5);
    cairo_stroke(cr);

    double iw = w / (double)count;
    for (int i = 0; i < count; i++) {
        double ix = x + i * iw;
        int idx = add_hit(kind, a, i, ix, y, iw, h);
        int on = sel == i;
        int hot = hit_hover(idx);

        if (on) {
            rgba(cr, C_ACCENT, 0.92);
            rounded_rect(cr, ix + 2.0, y + 2.0, iw - 4.0, h - 4.0,
                         (h - 4.0) / 2.0);
            cairo_fill(cr);
        } else if (hot) {
            rgba(cr, C_FG, 0.045);
            rounded_rect(cr, ix + 2.0, y + 2.0, iw - 4.0, h - 4.0,
                         (h - 4.0) / 2.0);
            cairo_fill(cr);
        }

        text_draw(cr, items[i], 9.5, 600,
                  on ? C_ON_ACCENT : (hot ? C_FG : C_DIM),
                  on ? 1.0 : 0.86, ix + iw / 2.0, y + (h - 12.0) / 2.0,
                  ALIGN_CENTER, 0.2);
    }
}


static void paint_segment(cairo_t *cr, double x, double y, double w,
                          double h, const char *const *items, int count,
                          int sel, int seg_id)
{
    rgba(cr, C_TRACK, 0.95);
    rounded_rect(cr, x, y, w, h, h / 2.0);
    cairo_fill(cr);
    rgba(cr, C_BORDER, 0.40);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0, h / 2.0 - 0.5);
    cairo_stroke(cr);

    double iw = w / (double)count;
    for (int i = 0; i < count; i++) {
        double ix = x + i * iw;
        int idx = add_hit(HIT_SEGMENT, seg_id, i, ix, y, iw, h);
        int on = sel == i;
        int hot = hit_hover(idx);

        if (on) {
            rgba(cr, C_ACCENT, 0.92);
            rounded_rect(cr, ix + 2.0, y + 2.0, iw - 4.0, h - 4.0,
                         (h - 4.0) / 2.0);
            cairo_fill(cr);
        } else if (hot) {
            rgba(cr, C_FG, 0.045);
            rounded_rect(cr, ix + 2.0, y + 2.0, iw - 4.0, h - 4.0,
                         (h - 4.0) / 2.0);
            cairo_fill(cr);
        }

        text_draw(cr, items[i], 9.5, 600,
                  on ? C_ON_ACCENT : (hot ? C_FG : C_DIM),
                  on ? 1.0 : 0.86, ix + iw / 2.0, y + (h - 12.0) / 2.0,
                  ALIGN_CENTER, 0.2);
    }
}

static void paint_button_ex(cairo_t *cr, double x, double y, double w,
                            double h, const char *label, int primary,
                            int kind, int a, int b)
{
    int idx = add_hit(kind, a, b, x, y, w, h);
    int hot = hit_hover(idx);
    int down = idx >= 0 && idx == g_ui.pressed;

    if (primary) {
        unsigned fill = down ? C_ACCENT_LO : (hot ? C_ACCENT_HI : C_ACCENT);
        rgba(cr, fill, 1.0);
        rounded_rect(cr, x, y, w, h, 8.0);
        cairo_fill(cr);
        text_draw(cr, label, 11.0, 600, C_ON_ACCENT, 1.0,
                  x + w / 2.0, y + (h - 14.0) / 2.0, ALIGN_CENTER, 0.25);
    } else {
        rgba(cr, C_FG, down ? 0.10 : (hot ? 0.065 : 0.035));
        rounded_rect(cr, x, y, w, h, 8.0);
        cairo_fill(cr);
        rgba(cr, C_BORDER, hot ? 0.85 : 0.55);
        cairo_set_line_width(cr, 1.0);
        rounded_rect(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0, 7.5);
        cairo_stroke(cr);
        text_draw(cr, label, 11.0, 600, hot ? C_FG : C_DIM, 0.95,
                  x + w / 2.0, y + (h - 14.0) / 2.0, ALIGN_CENTER, 0.25);
    }
}


static void paint_button(cairo_t *cr, double x, double y, double w, double h,
                         const char *label, int primary, int button_id)
{
    int idx = add_hit(HIT_BUTTON, button_id, 0, x, y, w, h);
    int hot = hit_hover(idx);
    int down = idx >= 0 && idx == g_ui.pressed;

    if (primary) {
        unsigned fill = down ? C_ACCENT_LO : (hot ? C_ACCENT_HI : C_ACCENT);
        rgba(cr, fill, 1.0);
        rounded_rect(cr, x, y, w, h, 8.0);
        cairo_fill(cr);
        text_draw(cr, label, 11.0, 600, C_ON_ACCENT, 1.0,
                  x + w / 2.0, y + (h - 14.0) / 2.0, ALIGN_CENTER, 0.25);
    } else {
        rgba(cr, C_FG, down ? 0.10 : (hot ? 0.065 : 0.035));
        rounded_rect(cr, x, y, w, h, 8.0);
        cairo_fill(cr);
        rgba(cr, C_BORDER, hot ? 0.85 : 0.55);
        cairo_set_line_width(cr, 1.0);
        rounded_rect(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0, 7.5);
        cairo_stroke(cr);
        text_draw(cr, label, 11.0, 600, hot ? C_FG : C_DIM, 0.95,
                  x + w / 2.0, y + (h - 14.0) / 2.0, ALIGN_CENTER, 0.25);
    }
}

static void paint_dashed_button(cairo_t *cr, double x, double y,
                                double w, double h, const char *label,
                                int button_id)
{
    int idx = add_hit(HIT_BUTTON, button_id, 0, x, y, w, h);
    int hot = hit_hover(idx);

    rgba(cr, C_FG, hot ? 0.045 : 0.018);
    rounded_rect(cr, x, y, w, h, 9.0);
    cairo_fill(cr);

    const double dash[] = { 5.0, 4.0 };
    cairo_set_dash(cr, dash, 2, 0.0);
    rgba(cr, hot ? C_ACCENT : C_BORDER, hot ? 0.75 : 0.55);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0, 8.5);
    cairo_stroke(cr);
    cairo_set_dash(cr, NULL, 0, 0.0);

    double tw = text_width(cr, label, 10.5, 600, 0.2);
    double plus = 9.0, gap = 8.0;
    double gx = x + (w - (tw + plus + gap)) / 2.0;
    double cy = y + h / 2.0;

    rgba(cr, hot ? C_ACCENT : C_DIM, 0.9);
    cairo_set_line_width(cr, 1.4);
    cairo_move_to(cr, gx, cy);
    cairo_line_to(cr, gx + plus, cy);
    cairo_move_to(cr, gx + plus / 2.0, cy - plus / 2.0);
    cairo_line_to(cr, gx + plus / 2.0, cy + plus / 2.0);
    cairo_stroke(cr);

    text_draw(cr, label, 10.5, 600, hot ? C_FG : C_DIM, 0.95,
              gx + plus + gap, cy - 7.0, ALIGN_LEFT, 0.2);
}

static double chip_width(cairo_t *cr, const char *text)
{
    return text_width(cr, text, 9.0, 600, 0.3) + 20.0;
}

static double paint_chip(cairo_t *cr, const char *text, double x, double y,
                         int active)
{
    double w = chip_width(cr, text);
    double h = 22.0;

    rgba(cr, active ? C_ACCENT : C_TRACK, active ? 0.16 : 0.85);
    rounded_rect(cr, x, y, w, h, h / 2.0);
    cairo_fill(cr);
    rgba(cr, active ? C_ACCENT : C_BORDER, active ? 0.65 : 0.45);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0, h / 2.0 - 0.5);
    cairo_stroke(cr);
    text_draw(cr, text, 9.0, 600, active ? C_ACCENT : C_DIM, 0.96,
              x + w / 2.0, y + 5.0, ALIGN_CENTER, 0.3);
    return w;
}

static double keycap_width(cairo_t *cr, const char *text)
{
    return text_width(cr, text, 9.0, 600, 0.3) + 16.0;
}

static double paint_keycap(cairo_t *cr, const char *text, double x, double y)
{
    double w = keycap_width(cr, text);
    double h = 21.0;

    rgba(cr, 0x1C1D1F, 1.0);
    rounded_rect(cr, x, y, w, h, 5.0);
    cairo_fill(cr);
    rgba(cr, C_BORDER, 0.70);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0, 4.5);
    cairo_stroke(cr);
    rgba(cr, C_FG, 0.06);
    rounded_rect(cr, x + 2.0, y + 2.0, w - 4.0, h - 4.0, 3.5);
    cairo_fill(cr);

    text_draw(cr, text, 9.0, 600, 0xE8E8E8, 0.95,
              x + w / 2.0, y + 4.5, ALIGN_CENTER, 0.3);
    return w;
}

static void paint_field(cairo_t *cr, double x, double y, double w, double h,
                        const char *text, int chevron)
{
    rgba(cr, C_TRACK, 0.9);
    rounded_rect(cr, x, y, w, h, 7.0);
    cairo_fill(cr);
    rgba(cr, C_BORDER, 0.50);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0, 6.5);
    cairo_stroke(cr);

    text_draw(cr, text, 10.5, 500, 0xD6D6D6, 0.92,
              x + 12.0, y + (h - 13.0) / 2.0, ALIGN_LEFT, 0.1);

    if (chevron) {
        double cx = x + w - 16.0;
        double cy = y + h / 2.0;
        rgba(cr, C_DIM, 0.85);
        cairo_set_line_width(cr, 1.4);
        cairo_move_to(cr, cx - 4.0, cy - 2.0);
        cairo_line_to(cr, cx, cy + 2.5);
        cairo_line_to(cr, cx + 4.0, cy - 2.0);
        cairo_stroke(cr);
    }
}

static void paint_divider(cairo_t *cr, double x, double y, double w)
{
    rgba(cr, C_BORDER, 0.30);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, x, y + 0.5);
    cairo_line_to(cr, x + w, y + 0.5);
    cairo_stroke(cr);
}

static const char *slider_text(int slider, double t, char *buf, size_t n)
{
    t = clampd(t, 0.0, 1.0);
    switch (slider) {
    case SL_POINTER:
        snprintf(buf, n, "%+.2f", t * 2.0 - 1.0);
        break;
    case SL_REPEAT:
        snprintf(buf, n, "%.0f /s", 15.0 + 65.0 * t);
        break;
    case SL_DELAY:
        snprintf(buf, n, "%.0f ms", 150.0 + 550.0 * t);
        break;
    case SL_START_DELAY:
        snprintf(buf, n, "%.1f s", 20.0 * t);
        break;
    case SL_IDLE:
        snprintf(buf, n, "%.0f min", 1.0 + 59.0 * t);
        break;
    default:
        snprintf(buf, n, "%.0f%%", t * 100.0);
        break;
    }
    return buf;
}

/* ------------------------------------------------------------------ */
/*  Chrome: sidebar, header, footer                                    */
/* ------------------------------------------------------------------ */

static void paint_nav_icon(cairo_t *cr, int page, double x, double y,
                           unsigned col, double alpha)
{
    double cx = x + 9.0;
    double cy = y + 9.0;
    /* Start with an empty path: otherwise cairo_arc() joins its first arc
     * to the current point left over from the previous icon, which draws a
     * stray diagonal across the sidebar. */
    cairo_new_path(cr);
    rgba(cr, col, alpha);
    cairo_set_line_width(cr, 1.35);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    switch (page) {
    case PAGE_KEYBINDINGS:
        rounded_rect(cr, x + 1.0, y + 3.0, 16.0, 12.0, 3.0);
        cairo_stroke(cr);
        for (int i = 0; i < 3; i++) {
            cairo_move_to(cr, x + 4.5 + i * 4.5, y + 7.0);
            cairo_line_to(cr, x + 5.8 + i * 4.5, y + 7.0);
        }
        cairo_move_to(cr, x + 4.5, y + 11.0);
        cairo_line_to(cr, x + 13.5, y + 11.0);
        cairo_stroke(cr);
        break;
    case PAGE_STARTUP:
        for (int r = 0; r < 2; r++)
            for (int c = 0; c < 2; c++) {
                rounded_rect(cr, x + 1.0 + c * 8.5, y + 1.5 + r * 8.5,
                             6.5, 6.5, 1.6);
                cairo_stroke(cr);
            }
        break;
    case PAGE_DEFAULT_APPS:
        rounded_rect(cr, x + 1.5, y + 2.0, 15.0, 14.0, 3.0);
        cairo_stroke(cr);
        cairo_new_path(cr);
        cairo_arc(cr, x + 6.0, y + 9.0, 2.2, 0.0, 2.0 * PI);
        cairo_stroke(cr);
        cairo_move_to(cr, x + 11.5, y + 6.0);
        cairo_line_to(cr, x + 14.0, y + 6.0);
        cairo_move_to(cr, x + 11.5, y + 10.0);
        cairo_line_to(cr, x + 14.0, y + 10.0);
        cairo_stroke(cr);
        break;
    case PAGE_RULES:
        rounded_rect(cr, x + 1.0, y + 2.0, 16.0, 14.0, 2.5);
        cairo_stroke(cr);
        cairo_move_to(cr, x + 1.0, y + 6.5);
        cairo_line_to(cr, x + 17.0, y + 6.5);
        cairo_move_to(cr, x + 5.0, y + 10.0);
        cairo_line_to(cr, x + 13.0, y + 10.0);
        cairo_stroke(cr);
        break;
    case PAGE_INPUT:
        rounded_rect(cr, x + 4.0, y + 1.5, 10.0, 15.0, 5.0);
        cairo_stroke(cr);
        cairo_move_to(cr, x + 9.0, y + 4.0);
        cairo_line_to(cr, x + 9.0, y + 7.5);
        cairo_stroke(cr);
        break;
    case PAGE_DISPLAYS:
        rounded_rect(cr, x + 1.0, y + 2.0, 16.0, 11.0, 2.0);
        cairo_stroke(cr);
        cairo_move_to(cr, x + 9.0, y + 13.0);
        cairo_line_to(cr, x + 9.0, y + 16.0);
        cairo_move_to(cr, x + 5.0, y + 16.5);
        cairo_line_to(cr, x + 13.0, y + 16.5);
        cairo_stroke(cr);
        break;
    case PAGE_SESSION:
        /* power: a ring with a gap at the top and a stem into the centre */
        cairo_arc(cr, cx, cy, 6.6, -PI / 2.0 + 0.55,
                  3.0 * PI / 2.0 - 0.55);
        cairo_stroke(cr);
        cairo_move_to(cr, cx, cy - 7.4);
        cairo_line_to(cr, cx, cy);
        cairo_stroke(cr);
        break;
    case PAGE_PROFILE:
        /* a head above a shallow shoulder arc */
        cairo_new_path(cr);
        cairo_arc(cr, cx, cy - 3.6, 3.0, 0.0, 2.0 * PI);
        cairo_stroke(cr);
        cairo_new_path(cr);
        cairo_arc(cr, cx, cy + 8.2, 6.0, PI, 2.0 * PI);
        cairo_stroke(cr);
        break;
    default:
        /* info: dot above a short stem */
        cairo_arc(cr, cx, cy, 7.4, 0.0, 2.0 * PI);
        cairo_stroke(cr);
        cairo_arc(cr, cx, cy - 3.8, 1.15, 0.0, 2.0 * PI);
        cairo_fill(cr);
        cairo_move_to(cr, cx, cy - 1.2);
        cairo_line_to(cr, cx, cy + 4.0);
        cairo_stroke(cr);
        break;
    }
}

/* Soft radial glows are extremely expensive for Cairo to evaluate per pixel
 * over a full window, so pre-render each one once into a small sprite and
 * paint that scaled.  The glows are low-alpha and smooth, so bilinear
 * scaling is visually indistinguishable. */
#define GLOW_SIZE 256

static cairo_surface_t *g_ambient_glow;
static cairo_surface_t *g_sidebar_glow;
static unsigned g_glow_accent;

static cairo_surface_t *glow_make(int size, double inner_ratio,
                                  unsigned rgb, double a0, double amid)
{
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                    size, size);
    cairo_t *cr = cairo_create(s);
    double c = size / 2.0;
    cairo_pattern_t *p = cairo_pattern_create_radial(
        c, c, c * inner_ratio, c, c, c);
    cairo_pattern_add_color_stop_rgba(p, 0.0,
        R_(rgb), G_(rgb), B_(rgb), a0);
    if (amid >= 0.0)
        cairo_pattern_add_color_stop_rgba(p, 0.55,
            R_(rgb), G_(rgb), B_(rgb), amid);
    cairo_pattern_add_color_stop_rgba(p, 1.0,
        R_(rgb), G_(rgb), B_(rgb), 0.0);
    cairo_set_source(cr, p);
    cairo_paint(cr);
    cairo_pattern_destroy(p);
    cairo_destroy(cr);
    return s;
}

static void glow_ensure(void)
{
    if (g_glow_accent == accent_rgb && g_ambient_glow && g_sidebar_glow)
        return;
    if (g_ambient_glow) {
        cairo_surface_destroy(g_ambient_glow);
        g_ambient_glow = NULL;
    }
    if (g_sidebar_glow) {
        cairo_surface_destroy(g_sidebar_glow);
        g_sidebar_glow = NULL;
    }
    g_ambient_glow = glow_make(GLOW_SIZE, 30.0 / 520.0, accent_rgb, 0.035, -1.0);
    g_sidebar_glow = glow_make(GLOW_SIZE, 8.0 / 260.0, accent_rgb, 0.075, 0.018);
    g_glow_accent = accent_rgb;
}

static void glow_paint(cairo_t *cr, cairo_surface_t *s,
                       double cx, double cy, double radius)
{
    int size = cairo_image_surface_get_width(s);
    double k = (2.0 * radius) / size;
    cairo_save(cr);
    cairo_translate(cr, cx - radius, cy - radius);
    cairo_scale(cr, k, k);
    cairo_set_source_surface(cr, s, 0.0, 0.0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_restore(cr);
}

static void paint_sidebar(cairo_t *cr, double now)
{
    (void)now;
    rgba(cr, C_SIDEBAR, 1.0);
    cairo_rectangle(cr, 0, 0, SIDEBAR_W, WIN_H);
    cairo_fill(cr);

    glow_ensure();
    glow_paint(cr, g_sidebar_glow, 36.0, 30.0, 260.0);

    /* logo mark: four equaliser bars */
    rgba(cr, C_ACCENT, 0.14);
    rounded_rect(cr, 18.0, 18.0, 42.0, 42.0, 12.0);
    cairo_fill(cr);
    rgba(cr, C_ACCENT, 0.95);
    rounded_rect(cr, 22.0, 22.0, 34.0, 34.0, 9.0);
    cairo_fill(cr);
    rgba(cr, C_ON_ACCENT, 0.92);
    for (int i = 0; i < 4; i++) {
        double h = 8.0 + i * 4.0;
        rounded_rect(cr, 28.0 + i * 5.4, 39.0 - h / 2.0, 3.0, h, 1.5);
        cairo_fill(cr);
    }

    text_draw(cr, "JTLab", 17.0, 700, C_FG, 0.97,
              72.0, 22.0, ALIGN_LEFT, 0.1);
    text_draw(cr, "SETTINGS", 8.0, 600, C_DIM, 0.78,
              73.0, 44.0, ALIGN_LEFT, 2.4);

    double y = 100.0;
    for (int i = 0; i < PAGE_COUNT; i++) {
        int idx = add_hit(HIT_NAV, i, 0, 12.0, y, SIDEBAR_W - 24.0, 38.0);
        int active = g_ui.page == i;
        int hot = hit_hover(idx);

        if (active || hot) {
            rgba(cr, active ? C_ACCENT : C_FG, active ? 0.105 : 0.04);
            rounded_rect(cr, 12.0, y, SIDEBAR_W - 24.0, 38.0, 8.0);
            cairo_fill(cr);
        }
        if (active) {
            rgba(cr, C_ACCENT, 0.95);
            rounded_rect(cr, 12.0, y + 10.0, 3.0, 18.0, 1.5);
            cairo_fill(cr);
        }

        paint_nav_icon(cr, i, 26.0, y + 10.0,
                       active ? C_ACCENT : (hot ? C_FG : C_DIM),
                       active ? 0.95 : (hot ? 0.82 : 0.62));
        text_draw(cr, PAGE_NAMES[i], 11.5, active ? 600 : 500,
                  active ? C_FG : (hot ? 0xE0E0E0 : C_DIM),
                  active ? 0.98 : 0.88,
                  54.0, y + 11.0, ALIGN_LEFT, 0.1);

        if (active) {
            rgba(cr, C_ACCENT, 0.9);
            cairo_arc(cr, SIDEBAR_W - 28.0, y + 19.0, 2.0, 0.0, 2.0 * PI);
            cairo_fill(cr);
        }
        y += 41.0;
    }

    paint_divider(cr, 22.0, WIN_H - 128.0, SIDEBAR_W - 44.0);

    text_draw(cr, "PREVIEW BUILD", 8.0, 600, C_FAINT, 0.9,
              24.0, WIN_H - 110.0, ALIGN_LEFT, 1.7);
    text_draw(cr, "jtsettings 0.1.0", 10.5, 600, 0xD6D6D6, 0.92,
              24.0, WIN_H - 92.0, ALIGN_LEFT, 0.1);

    rgba(cr, C_ACCENT, 0.95);
    cairo_arc(cr, 29.0, WIN_H - 62.0, 4.0, 0.0, 2.0 * PI);
    cairo_fill(cr);
    char hex[16];
    snprintf(hex, sizeof hex, "#%06X", C_ACCENT);
    text_draw(cr, hex, 9.5, 500, C_DIM, 0.9,
              40.0, WIN_H - 68.0, ALIGN_LEFT, 0.6);

    text_draw(cr, "↑ ↓  switch pages", 8.5, 400, C_FAINT, 0.8,
              24.0, WIN_H - 42.0, ALIGN_LEFT, 0.2);

    rgba(cr, C_BORDER, 0.35);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, SIDEBAR_W - 0.5, 0.0);
    cairo_line_to(cr, SIDEBAR_W - 0.5, WIN_H);
    cairo_stroke(cr);
}

static void paint_header(cairo_t *cr)
{
    paint_divider(cr, SIDEBAR_W, HEADER_H, WIN_W - SIDEBAR_W);

    text_draw(cr, PAGE_NAMES[g_ui.page], 22.0, 700, C_FG, 0.98,
              CONTENT_X, 18.0, ALIGN_LEFT, 0.0);
    text_draw(cr, PAGE_SUBS[g_ui.page], 10.0, 400, C_DIM, 0.82,
              CONTENT_X, 51.0, ALIGN_LEFT, 0.15);

    char hex[16];
    snprintf(hex, sizeof hex, "#%06X", C_ACCENT);
    double pill_w = 116.0;
    double px = WIN_W - 36.0 - pill_w;

    rgba(cr, C_FG, 0.025);
    rounded_rect(cr, px, 16.0, pill_w, 28.0, 14.0);
    cairo_fill(cr);
    rgba(cr, C_BORDER, 0.45);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, px + 0.5, 16.5, pill_w - 1.0, 27.0, 13.5);
    cairo_stroke(cr);

    rgba(cr, C_ACCENT, 0.16);
    cairo_arc(cr, px + 17.0, 30.0, 7.0, 0.0, 2.0 * PI);
    cairo_fill(cr);
    rgba(cr, C_ACCENT, 1.0);
    cairo_arc(cr, px + 17.0, 30.0, 3.6, 0.0, 2.0 * PI);
    cairo_fill(cr);
    text_draw(cr, hex, 9.5, 600, 0xDCDCDC, 0.92,
              px + 30.0, 23.0, ALIGN_LEFT, 0.8);

    text_draw(cr, "JT · Wayland", 9.0, 400, C_FAINT, 0.85,
              WIN_W - 36.0, 51.0, ALIGN_RIGHT, 0.2);
}

static void pos_canvas_bounds(double *cx, double *cy,
                              double *cw, double *ch)
{
    double pw = 700.0, ph = 420.0;
    double px = (WIN_W - pw) / 2.0;
    double py = (WIN_H - ph) / 2.0;
    *cx = px + 24.0;
    *cy = py + 70.0;
    *cw = pw - 48.0;
    *ch = ph - 140.0;
}

static int head_in_arrangement(int idx)
{
    Head *h = head_get(idx);
    if (!h)
        return 0;
    return h->p_enabled || head_enabled_count() == 0;
}

static void pos_compute_layout(void)
{
    double cx, cy, cw, ch;
    pos_canvas_bounds(&cx, &cy, &cw, &ch);

    double minx = 0.0, miny = 0.0, maxx = 0.0, maxy = 0.0;
    int first = 1;
    for (int i = 0; i < MAX_HEADS; i++) {
        if (!head_in_arrangement(i))
            continue;
        Head *h = &g_heads[i];
        double lw, lh;
        head_logical_size(h, &lw, &lh);
        if (first) {
            minx = h->p_x;
            miny = h->p_y;
            maxx = h->p_x + lw;
            maxy = h->p_y + lh;
            first = 0;
        } else {
            if (h->p_x < minx) minx = h->p_x;
            if (h->p_y < miny) miny = h->p_y;
            if (h->p_x + lw > maxx) maxx = h->p_x + lw;
            if (h->p_y + lh > maxy) maxy = h->p_y + lh;
        }
    }
    if (first) {
        minx = 0.0; miny = 0.0; maxx = 1920.0; maxy = 1080.0;
    }

    double spanx = maxx - minx;
    double spany = maxy - miny;
    if (spanx < 1.0) spanx = 1.0;
    if (spany < 1.0) spany = 1.0;

    double avail_w = cw - 80.0;
    double avail_h = ch - 80.0;
    double s = avail_w / spanx;
    if (avail_h / spany < s)
        s = avail_h / spany;
    s = clampd(s, 0.02, 0.35);

    g_ui.pos_scale = s;
    g_ui.pos_offx = cx + 40.0 + (avail_w - spanx * s) / 2.0 - minx * s;
    g_ui.pos_offy = cy + 40.0 + (avail_h - spany * s) / 2.0 - miny * s;
}

static void paint_position_editor(cairo_t *cr)
{
    if (!g_ui.pos_editor_open)
        return;

    /* Compute the fit once per editor session.  Recomputing on every
     * frame would re-centre the boxes after each drag and make them
     * appear to snap back. */
    if (!g_ui.pos_layout_valid) {
        pos_compute_layout();
        g_ui.pos_layout_valid = 1;
    }

    rgba(cr, 0x000000, 0.55);
    cairo_rectangle(cr, 0, 0, WIN_W, WIN_H);
    cairo_fill(cr);

    double pw = 700.0, ph = 420.0;
    double px = (WIN_W - pw) / 2.0;
    double py = (WIN_H - ph) / 2.0;

    paint_card(cr, px, py, pw, ph);

    text_draw(cr, "Arrange displays", 18.0, 700, C_FG, 0.98,
              px + 24.0, py + 20.0, ALIGN_LEFT, 0.0);
    text_draw(cr, "Drag the displays to match your physical setup", 9.5, 400,
              C_DIM, 0.8, px + 24.0, py + 48.0, ALIGN_LEFT, 0.1);

    double cx, cy, cw, ch;
    pos_canvas_bounds(&cx, &cy, &cw, &ch);
    rgba(cr, C_TRACK, 0.6);
    rounded_rect(cr, cx, cy, cw, ch, 8.0);
    cairo_fill(cr);
    rgba(cr, C_BORDER, 0.5);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, cx + 0.5, cy + 0.5, cw - 1.0, ch - 1.0, 7.5);
    cairo_stroke(cr);

    int any = 0;
    for (int i = 0; i < MAX_HEADS; i++) {
        if (!head_in_arrangement(i))
            continue;
        any = 1;
        Head *h = &g_heads[i];
        double lw, lh;
        head_logical_size(h, &lw, &lh);
        double rx = g_ui.pos_offx + h->p_x * g_ui.pos_scale;
        double ry = g_ui.pos_offy + h->p_y * g_ui.pos_scale;
        double rw = lw * g_ui.pos_scale;
        double rh = lh * g_ui.pos_scale;
        if (rw < 28.0) rw = 28.0;
        if (rh < 20.0) rh = 20.0;

        int is_drag = (g_ui.disp_drag == i);
        int enabled = h->p_enabled;
        rgba(cr, is_drag ? C_ACCENT : 0x1A1B1D,
             is_drag ? 0.28 : (enabled ? 1.0 : 0.5));
        rounded_rect(cr, rx, ry, rw, rh, 6.0);
        cairo_fill(cr);
        rgba(cr, is_drag ? C_ACCENT : C_BORDER,
             is_drag ? 0.95 : (enabled ? 0.6 : 0.3));
        cairo_set_line_width(cr, is_drag ? 2.0 : 1.0);
        rounded_rect(cr, rx + 0.5, ry + 0.5, rw - 1.0, rh - 1.0, 5.5);
        cairo_stroke(cr);

        text_draw(cr, h->name[0] ? h->name : "Output", 11.0, 600,
                  enabled ? C_FG : C_DIM, enabled ? 0.95 : 0.6,
                  rx + rw / 2.0, ry + rh / 2.0 - 12.0, ALIGN_CENTER, 0.1);
        char posbuf[64];
        snprintf(posbuf, sizeof posbuf, "%d, %d", h->p_x, h->p_y);
        text_draw(cr, posbuf, 8.5, 400, C_DIM, 0.85,
                  rx + rw / 2.0, ry + rh / 2.0 + 4.0, ALIGN_CENTER, 0.1);
    }

    if (!any) {
        text_draw(cr, "No displays detected", 11.0, 500, C_DIM, 0.9,
                  cx + cw / 2.0, cy + ch / 2.0 - 6.0, ALIGN_CENTER, 0.2);
    }

    paint_button(cr, px + pw - 24.0 - 120.0, py + ph - 24.0 - 36.0, 120.0, 36.0,
                 "Done", 1, BTN_POS_DONE);
}

static void mode_picker_rect(const Head *h, double *px, double *py,
                             double *pw, double *ph)
{
    Resolution res[MAX_RESOLUTIONS];
    int rn = head_resolutions(h, res, MAX_RESOLUTIONS);
    int rows = rn > 10 ? 10 : rn;

    *pw = 460.0;
    *ph = 104.0 + rows * 34.0 + (rn == 0 ? 46.0 : 0.0) + 52.0;
    *px = (WIN_W - *pw) / 2.0;
    *py = (WIN_H - *ph) / 2.0;
}

static void paint_mode_picker(cairo_t *cr)
{
    if (!g_ui.mode_picker_open)
        return;

    Head *h = head_get(g_ui.mode_picker_head);
    if (!h) {
        g_ui.mode_picker_open = 0;
        g_ui.mode_picker_head = -1;
        return;
    }

    Resolution res[MAX_RESOLUTIONS];
    int rn = head_resolutions(h, res, MAX_RESOLUTIONS);
    int rows = rn > 10 ? 10 : rn;

    double pw, ph, px, py;
    mode_picker_rect(h, &px, &py, &pw, &ph);

    rgba(cr, 0x000000, 0.55);
    cairo_rectangle(cr, 0, 0, WIN_W, WIN_H);
    cairo_fill(cr);

    paint_card(cr, px, py, pw, ph);
    text_draw(cr, "Resolution", 16.0, 700, C_FG, 0.98,
              px + 22.0, py + 18.0, ALIGN_LEFT, 0.0);
    text_draw(cr, h->name[0] ? h->name : "Output", 9.5, 400, C_DIM, 0.8,
              px + 22.0, py + 44.0, ALIGN_LEFT, 0.1);

    if (rn == 0) {
        text_draw(cr, "No modes reported by the compositor.", 9.5, 400,
                  C_DIM, 0.85, px + 22.0, py + 82.0, ALIGN_LEFT, 0.1);
    }

    int32_t curw = h->cur_w, curh = h->cur_h;
    if (head_mode_valid(h)) {
        curw = h->modes[h->p_mode].width;
        curh = h->modes[h->p_mode].height;
    }

    for (int i = 0; i < rows; i++) {
        double ry = py + 76.0 + i * 34.0;
        int on = (res[i].w == curw && res[i].h == curh);
        int idx = add_hit(HIT_DISP_MODE, g_ui.mode_picker_head, i,
                          px + 16.0, ry, pw - 32.0, 28.0);
        int hot = hit_hover(idx);

        rgba(cr, on ? C_ACCENT : C_TRACK, on ? 0.16 : (hot ? 0.9 : 0.7));
        rounded_rect(cr, px + 16.0, ry, pw - 32.0, 28.0, 7.0);
        cairo_fill(cr);
        rgba(cr, on ? C_ACCENT : C_BORDER, on ? 0.65 : 0.35);
        cairo_set_line_width(cr, 1.0);
        rounded_rect(cr, px + 16.5, ry + 0.5, pw - 33.0, 27.0, 6.5);
        cairo_stroke(cr);

        char label[48];
        snprintf(label, sizeof label, "%d × %d", res[i].w, res[i].h);
        text_draw(cr, label, 10.5, 500, on ? C_ACCENT : C_FG,
                  on ? 1.0 : 0.92, px + 30.0, ry + 7.0, ALIGN_LEFT, 0.2);
        if (on) {
            rgba(cr, C_ACCENT, 0.95);
            cairo_arc(cr, px + pw - 32.0, ry + 14.0, 4.0, 0.0, 2.0 * PI);
            cairo_fill(cr);
        }
    }

    paint_button(cr, px + pw - 22.0 - 100.0, py + ph - 22.0 - 32.0,
                 100.0, 32.0, "Cancel", 0, BTN_MODE_CLOSE);
}


static void paint_footer(cairo_t *cr)
{
    paint_divider(cr, SIDEBAR_W, WIN_H - 30.0, WIN_W - SIDEBAR_W);

    double age = mono_sec() - g_ui.toast_t;
    const char *msg = g_ui.page == PAGE_STARTUP
        ? "AUTOSTART · ~/.config/jt/startup.conf"
        : g_ui.page == PAGE_KEYBINDINGS
        ? "BUILT-IN SHORTCUTS · jtlabwc"
        : g_ui.page == PAGE_INPUT
        ? "INPUT · ~/.config/jt/input.conf"
        : g_ui.page == PAGE_PROFILE
        ? "PROFILE · ~/.config/jt/profile.config"
        : "PREVIEW · changes are not saved";
    unsigned col = C_FAINT;
    double alpha = 0.9;

    if (g_ui.toast[0] && age >= 0.0 && age < 3.0) {
        msg = g_ui.toast;
        col = C_ACCENT;
        alpha = age < 0.25 ? age / 0.25 : (age > 2.4 ? (3.0 - age) / 0.6 : 1.0);
        alpha = clampd(alpha, 0.0, 1.0) * 0.95;
    }

    rgba(cr, col, alpha);
    cairo_arc(cr, CONTENT_X + 3.0, WIN_H - 16.0, 2.5, 0.0, 2.0 * PI);
    cairo_fill(cr);
    text_draw(cr, msg, 9.0, 500, col, alpha,
              CONTENT_X + 12.0, WIN_H - 22.0, ALIGN_LEFT, 0.35);

    char counter[32];
    snprintf(counter, sizeof counter, "%02d / %02d", g_ui.page + 1,
             PAGE_COUNT);
    text_draw(cr, counter, 9.0, 600, C_FAINT, 0.85,
              WIN_W - 36.0, WIN_H - 22.0, ALIGN_RIGHT, 0.8);
    text_draw(cr, "↑ ↓ · Esc to quit", 9.0, 400, C_FAINT, 0.7,
              WIN_W - 96.0, WIN_H - 22.0, ALIGN_RIGHT, 0.2);
}

/* ------------------------------------------------------------------ */
/*  Page: Keybindings                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *action;
    const char *context;
    const char *keys[3];
    int nkeys;
} KeybindRow;

/* These mirror the built-in key_combos table in jtlabwc.c.  The page is a
 * read-only view of what the compositor actually binds at startup. */
static const KeybindRow KEYBINDS[] = {
    { "Next window",          "Window",    { "Super", "J" }, 2 },
    { "Previous window",      "Window",    { "Super", "K" }, 2 },
    { "Open terminal",        "Session",   { "Super", "Return" }, 2 },
    { "Close window",         "Window",    { "Super", "Shift", "Q" }, 3 },
    { "Toggle maximize",      "Window",    { "Super", "A" }, 2 },
    { "Minimize window",      "Window",    { "Super", "M" }, 2 },
    { "Show desktop",         "Window",    { "Super", "D" }, 2 },
    { "Toggle fullscreen",    "Window",    { "Super", "Y" }, 2 },
    { "Workspace 1",          "Workspace", { "Super", "1" }, 2 },
    { "Workspace 2",          "Workspace", { "Super", "2" }, 2 },
    { "Workspace 3",          "Workspace", { "Super", "3" }, 2 },
    { "Workspace 4",          "Workspace", { "Super", "4" }, 2 },
    { "Workspace 5",          "Workspace", { "Super", "5" }, 2 },
    { "Workspace 6",          "Workspace", { "Super", "6" }, 2 },
    { "Workspace 7",          "Workspace", { "Super", "7" }, 2 },
    { "Workspace 8",          "Workspace", { "Super", "8" }, 2 },
    { "Workspace 9",          "Workspace", { "Super", "9" }, 2 },
    { "Send to workspace 1",  "Workspace", { "Super", "Shift", "1" }, 3 },
    { "Send to workspace 2",  "Workspace", { "Super", "Shift", "2" }, 3 },
    { "Send to workspace 3",  "Workspace", { "Super", "Shift", "3" }, 3 },
    { "Send to workspace 4",  "Workspace", { "Super", "Shift", "4" }, 3 },
    { "Send to workspace 5",  "Workspace", { "Super", "Shift", "5" }, 3 },
    { "Send to workspace 6",  "Workspace", { "Super", "Shift", "6" }, 3 },
    { "Send to workspace 7",  "Workspace", { "Super", "Shift", "7" }, 3 },
    { "Send to workspace 8",  "Workspace", { "Super", "Shift", "8" }, 3 },
    { "Send to workspace 9",  "Workspace", { "Super", "Shift", "9" }, 3 },
    { "Snap left",            "Window",    { "Super", "Shift", "H" }, 3 },
    { "Snap right",           "Window",    { "Super", "Shift", "L" }, 3 },
    { "Snap up",              "Window",    { "Super", "Shift", "K" }, 3 },
    { "Snap down",            "Window",    { "Super", "Shift", "J" }, 3 },
    { "Unsnap window",        "Window",    { "Super", "Shift", "A" }, 3 },
    { "Window menu",          "Window",    { "Super", "I" }, 2 },
    { "Application menu",     "Session",   { "Super", "Space" }, 2 },
    { "Volume down",          "Media",     { "Vol-" }, 1 },
    { "Volume up",            "Media",     { "Vol+" }, 1 },
    { "Mute audio",           "Media",     { "Mute" }, 1 },
    { "Brightness up",        "Media",     { "Bri+" }, 1 },
    { "Brightness down",      "Media",     { "Bri-" }, 1 },
};
#define KEYBIND_COUNT ((int)(sizeof KEYBINDS / sizeof KEYBINDS[0]))

static const char *const KB_FILTERS[] = {
    "All", "Window", "Workspace", "Session", "Media",
};
#define KB_FILTER_COUNT ((int)(sizeof KB_FILTERS / sizeof KB_FILTERS[0]))

static int kb_filter_active(void)
{
    int f = g_ui.segments[SEG_KB_FILTER];
    return (f >= 0 && f < KB_FILTER_COUNT) ? f : 0;
}

static int kb_row_matches(int i)
{
    int f = kb_filter_active();
    return f == 0 || !strcmp(KEYBINDS[i].context, KB_FILTERS[f]);
}

static int kb_filtered_count(void)
{
    int n = 0;
    for (int i = 0; i < KEYBIND_COUNT; i++)
        if (kb_row_matches(i))
            n++;
    return n;
}

#define KB_ROW_H     43.0
#define KB_HEADER_H  44.0
#define KB_BOTTOM_GAP 62.0

static void kb_geom(double *ty, double *th, double *view_h)
{
    /* Leave a gap below the search bar before the list box begins. */
    *ty = 164.0;
    *th = WIN_H - KB_BOTTOM_GAP - *ty;
    *view_h = *th - KB_HEADER_H;
}

static double kb_max_scroll(void)
{
    double ty, th, view_h;
    kb_geom(&ty, &th, &view_h);
    (void)ty;
    (void)th;
    double total = kb_filtered_count() * KB_ROW_H;
    return total > view_h ? total - view_h : 0.0;
}

static void paint_search_icon(cairo_t *cr, double x, double y)
{
    rgba(cr, C_DIM, 0.75);
    cairo_set_line_width(cr, 1.3);
    cairo_arc(cr, x, y, 5.0, 0.0, 2.0 * PI);
    cairo_stroke(cr);
    cairo_move_to(cr, x + 3.8, y + 3.8);
    cairo_line_to(cr, x + 8.0, y + 8.0);
    cairo_stroke(cr);
}

static void page_keybindings(cairo_t *cr)
{
    double fx = CONTENT_X, fy = 104.0, fw = 380.0, fh = 34.0;
    rgba(cr, C_TRACK, 0.9);
    rounded_rect(cr, fx, fy, fw, fh, 7.0);
    cairo_fill(cr);
    rgba(cr, C_BORDER, 0.50);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, fx + 0.5, fy + 0.5, fw - 1.0, fh - 1.0, 6.5);
    cairo_stroke(cr);
    paint_search_icon(cr, fx + 17.0, fy + 17.0);
    text_draw(cr, "Search shortcuts…", 10.5, 500, C_DIM, 0.72,
              fx + 32.0, fy + 10.0, ALIGN_LEFT, 0.1);

    double total = 0.0;
    for (int i = 0; i < KB_FILTER_COUNT; i++) {
        total += chip_width(cr, KB_FILTERS[i]);
        if (i < KB_FILTER_COUNT - 1) total += 8.0;
    }
    double cx = WIN_W - 36.0 - total;
    int active_filter = kb_filter_active();
    for (int i = 0; i < KB_FILTER_COUNT; i++) {
        double w = chip_width(cr, KB_FILTERS[i]);
        (void)add_hit(HIT_SEGMENT, SEG_KB_FILTER, i, cx, 110.0, w, 22.0);
        paint_chip(cr, KB_FILTERS[i], cx, 110.0, i == active_filter);
        cx += w + 8.0;
    }

    double tx = CONTENT_X, ty, th, view_h;
    kb_geom(&ty, &th, &view_h);
    double tw = CONTENT_W;
    double max_scroll = kb_max_scroll();
    g_ui.kb_scroll = clampd(g_ui.kb_scroll, 0.0, max_scroll);
    double scroll = g_ui.kb_scroll;

    paint_card(cr, tx, ty, tw, th);

    text_draw(cr, "ACTION", 8.0, 600, C_FAINT, 0.9,
              tx + 20.0, ty + 14.0, ALIGN_LEFT, 1.4);
    text_draw(cr, "CONTEXT", 8.0, 600, C_FAINT, 0.9,
              tx + 330.0, ty + 14.0, ALIGN_LEFT, 1.4);
    text_draw(cr, "SHORTCUT", 8.0, 600, C_FAINT, 0.9,
              tx + tw - 20.0, ty + 14.0, ALIGN_RIGHT, 1.4);
    paint_divider(cr, tx + 20.0, ty + 36.0, tw - 40.0);

    double list_top = ty + KB_HEADER_H;
    double list_bottom = ty + th;

    cairo_save(cr);
    cairo_rectangle(cr, tx, list_top, tw, view_h);
    cairo_clip(cr);

    int visible_count = kb_filtered_count();
    int row = 0;
    for (int i = 0; i < KEYBIND_COUNT; i++) {
        if (!kb_row_matches(i))
            continue;
        double ry = list_top + row * KB_ROW_H - scroll;
        int last = row == visible_count - 1;
        row++;
        if (ry + KB_ROW_H < list_top || ry > list_bottom)
            continue;

        int idx = -1;
        if (ry >= list_top && ry + KB_ROW_H <= list_bottom)
            idx = add_hit(HIT_ROW, i, 0, tx + 8.0, ry,
                          tw - 16.0, 40.0);
        int sel = g_ui.selected[PAGE_KEYBINDINGS] == i;
        int hot = hit_hover(idx);

        if (sel || hot) {
            rgba(cr, sel ? C_ACCENT : C_FG, sel ? 0.075 : 0.032);
            rounded_rect(cr, tx + 8.0, ry, tw - 16.0, 40.0, 7.0);
            cairo_fill(cr);
        }
        if (sel) {
            rgba(cr, C_ACCENT, 0.9);
            rounded_rect(cr, tx + 8.0, ry + 10.0, 3.0, 20.0, 1.5);
            cairo_fill(cr);
        }

        text_draw(cr, KEYBINDS[i].action, 11.5, sel ? 600 : 500,
                  sel ? C_FG : (hot ? 0xECECEC : 0xD2D2D2), 0.96,
                  tx + 24.0, ry + 12.0, ALIGN_LEFT, 0.1);
        text_draw(cr, KEYBINDS[i].context, 9.0, 400, C_DIM, 0.72,
                  tx + 330.0, ry + 14.0, ALIGN_LEFT, 0.3);

        double kw = 0.0;
        for (int k = 0; k < KEYBINDS[i].nkeys; k++) {
            kw += keycap_width(cr, KEYBINDS[i].keys[k]);
            if (k < KEYBINDS[i].nkeys - 1) kw += 5.0;
        }
        double kx = tx + tw - 20.0 - kw;
        for (int k = 0; k < KEYBINDS[i].nkeys; k++) {
            kx += paint_keycap(cr, KEYBINDS[i].keys[k], kx, ry + 9.0) + 5.0;
        }

        if (!last)
            paint_divider(cr, tx + 20.0, ry + 41.0, tw - 40.0);
    }
    cairo_restore(cr);

    if (max_scroll > 0.0) {
        double track_x = tx + tw - 7.0;
        double track_y = list_top + 4.0;
        double track_h = view_h - 8.0;
        double total_h = visible_count * KB_ROW_H;
        double thumb_h = track_h * (view_h / total_h);
        if (thumb_h < 28.0) thumb_h = 28.0;
        double thumb_y = track_y +
            (track_h - thumb_h) * (scroll / max_scroll);

        rgba(cr, C_FG, 0.05);
        rounded_rect(cr, track_x, track_y, 3.0, track_h, 1.5);
        cairo_fill(cr);
        rgba(cr, C_FG, 0.20);
        rounded_rect(cr, track_x, thumb_y, 3.0, thumb_h, 1.5);
        cairo_fill(cr);
    }

    text_draw(cr, "Built-in shortcuts · jtlabwc", 9.0, 400, C_FAINT, 0.8,
              WIN_W - 36.0, WIN_H - 52.0, ALIGN_RIGHT, 0.2);
}

/* ------------------------------------------------------------------ */
/*  Page: Startup Apps                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    char command[STARTUP_CMD_MAX];
    int enabled;
} StartupEntry;

static StartupEntry g_startups[STARTUP_MAX];
static int g_startup_count;

/* ------------------------------------------------------------------ */
/*  Autostart configuration                                            */
/* ------------------------------------------------------------------ */

#define STARTUP_DIR       "jt"
#define STARTUP_FILE      "startup.conf"
#define STARTUP_PATH_MAX  512
#define STARTUP_DELAY_MAX 60000L
#define STARTUP_DELAY_SCALE 20000.0   /* slider 1.0 == 20 s */

/* Config files contain commands and paths consumed by other JTLab tools, so a
 * missing HOME must never silently redirect them to a world-writable /tmp. */
static const char *
user_home(void)
{
    const char *home = getenv("HOME");
    if (home && home[0])
        return home;
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir && pw->pw_dir[0])
        return pw->pw_dir;
    return "/nonexistent";
}

/* Keep this path and the file format in sync with jtstartup.c. */
static void
startup_config_path(char *buf, size_t n)
{
    snprintf(buf, n, "%s/.config/%s/%s", user_home(),
             STARTUP_DIR, STARTUP_FILE);
}

static void
startup_mkdirs(const char *path)
{
    char dir[STARTUP_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) return;
    *slash = '\0';

    for (char *p = dir + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        (void)mkdir(dir, 0700);
        *p = '/';
    }
    (void)mkdir(dir, 0700);
}

static char *
startup_trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

/* Human-friendly label: the basename of the first word in the command. */
static void
startup_name(const char *cmd, char *buf, size_t n)
{
    const char *start = cmd;
    while (*start && isspace((unsigned char)*start)) start++;
    const char *end = start;
    while (*end && !isspace((unsigned char)*end)) end++;

    const char *base = start;
    for (const char *p = start; p < end; p++)
        if (*p == '/') base = p + 1;

    size_t len = (size_t)(end - base);
    if (len == 0) {
        snprintf(buf, n, "Application");
        return;
    }
    if (len >= n) len = n - 1;
    memcpy(buf, base, len);
    buf[len] = '\0';
}

static void
startup_save(void)
{
    char path[STARTUP_PATH_MAX];
    startup_config_path(path, sizeof path);
    startup_mkdirs(path);

    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "# jtsettings autostart configuration\n");
    fprintf(f, "# Written by jtsettings, read by jtstartup.\n");
    long ms = lround(clampd(g_ui.sliders[SL_START_DELAY], 0.0, 1.0) *
                     STARTUP_DELAY_SCALE);
    fprintf(f, "delay_ms=%ld\n", ms);
    for (int i = 0; i < g_startup_count; i++)
        fprintf(f, "%s %s\n", g_startups[i].enabled ? "run" : "off",
                g_startups[i].command);
    fclose(f);
}

static int
startup_load(void)
{
    char path[STARTUP_PATH_MAX];
    startup_config_path(path, sizeof path);

    /* The file is the complete list.  No file means an empty list, not a set
     * of preview defaults. */
    g_startup_count = 0;

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *s = startup_trim(line);
        if (!*s || *s == '#') continue;

        if (!strncmp(s, "delay_ms=", 9)) {
            char *end = NULL;
            long ms = strtol(s + 9, &end, 10);
            if (end != s + 9 && ms >= 0) {
                if (ms > STARTUP_DELAY_MAX) ms = STARTUP_DELAY_MAX;
                g_ui.sliders[SL_START_DELAY] =
                    clampd((double)ms / STARTUP_DELAY_SCALE, 0.0, 1.0);
            }
            continue;
        }

        int enabled;
        const char *rest;
        if (!strncmp(s, "run ", 4)) {
            enabled = 1;
            rest = s + 4;
        } else if (!strncmp(s, "off ", 4)) {
            enabled = 0;
            rest = s + 4;
        } else {
            continue;
        }

        char *cmd = startup_trim((char *)rest);
        if (!*cmd || g_startup_count >= STARTUP_MAX) continue;
        snprintf(g_startups[g_startup_count].command, STARTUP_CMD_MAX,
                 "%s", cmd);
        g_startups[g_startup_count].enabled = enabled;
        g_startup_count++;
    }
    fclose(f);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Pointer & touchpad settings                                        */
/* ------------------------------------------------------------------ */

#define INPUT_DIR       "jt"
#define INPUT_FILE      "input.conf"
#define INPUT_PATH_MAX  512

/* Keep this path and the file format in sync with jtlabwc.c. */
static void
input_config_path(char *buf, size_t n)
{
    snprintf(buf, n, "%s/.config/%s/%s", user_home(), INPUT_DIR, INPUT_FILE);
}

static int
input_toggle(int toggle)
{
    switch (toggle) {
    case TOG_NATURAL:
    case TOG_TAP:
    case TOG_DRAG:
    case TOG_LEFT:
    case TOG_DWT:
    case TOG_MIDDLE:
        return 1;
    default:
        return 0;
    }
}

static void
input_save(void)
{
    char path[INPUT_PATH_MAX];
    input_config_path(path, sizeof path);
    startup_mkdirs(path);

    FILE *f = fopen(path, "w");
    if (!f) return;

    /* jtsettings works in 0..1; libinput wants -1..1. */
    double speed = 2.0 * clampd(g_ui.sliders[SL_POINTER], 0.0, 1.0) - 1.0;
    static const char *const methods[] = { "two_finger", "edge", "button" };
    int method = g_ui.segments[SEG_SCROLL];
    if (method < 0 || method > 2) method = 0;
    int rate = (int)(15.0 + 65.0 * clampd(g_ui.sliders[SL_REPEAT], 0.0, 1.0) + 0.5);
    int delay = (int)(150.0 + 550.0 * clampd(g_ui.sliders[SL_DELAY], 0.0, 1.0) + 0.5);

    fprintf(f, "# jtsettings input configuration\n");
    fprintf(f, "# Written by jtsettings, read by jtlabwc.\n");
    fprintf(f, "pointer_speed=%.3f\n", speed);
    fprintf(f, "natural_scroll=%d\n", g_ui.toggles[TOG_NATURAL] ? 1 : 0);
    fprintf(f, "tap=%d\n", g_ui.toggles[TOG_TAP] ? 1 : 0);
    fprintf(f, "tap_and_drag=%d\n", g_ui.toggles[TOG_DRAG] ? 1 : 0);
    fprintf(f, "disable_while_typing=%d\n",
            g_ui.toggles[TOG_DWT] ? 1 : 0);
    fprintf(f, "left_handed=%d\n", g_ui.toggles[TOG_LEFT] ? 1 : 0);
    fprintf(f, "middle_emulation=%d\n", g_ui.toggles[TOG_MIDDLE] ? 1 : 0);
    fprintf(f, "scroll_method=%s\n", methods[method]);
    fprintf(f, "accel_profile=%s\n",
            g_ui.segments[SEG_ACCEL] == 1 ? "flat" : "adaptive");
    fprintf(f, "repeat_rate=%d\n", rate);
    fprintf(f, "repeat_delay=%d\n", delay);
    fclose(f);
}

static int
input_load(void)
{
    char path[INPUT_PATH_MAX];
    input_config_path(path, sizeof path);

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *s = startup_trim(line);
        if (!*s || *s == '#') continue;

        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = startup_trim(s);
        char *value = startup_trim(eq + 1);

        if (!strcmp(key, "pointer_speed")) {
            double speed = strtod(value, NULL);
            speed = clampd(speed, -1.0, 1.0);
            g_ui.sliders[SL_POINTER] = clampd((speed + 1.0) / 2.0, 0.0, 1.0);
        } else if (!strcmp(key, "natural_scroll")) {
            g_ui.toggles[TOG_NATURAL] = atoi(value) != 0;
        } else if (!strcmp(key, "tap")) {
            g_ui.toggles[TOG_TAP] = atoi(value) != 0;
        } else if (!strcmp(key, "tap_and_drag")) {
            g_ui.toggles[TOG_DRAG] = atoi(value) != 0;
        } else if (!strcmp(key, "disable_while_typing")) {
            g_ui.toggles[TOG_DWT] = atoi(value) != 0;
        } else if (!strcmp(key, "left_handed")) {
            g_ui.toggles[TOG_LEFT] = atoi(value) != 0;
        } else if (!strcmp(key, "middle_emulation")) {
            g_ui.toggles[TOG_MIDDLE] = atoi(value) != 0;
        } else if (!strcmp(key, "scroll_method")) {
            if (!strcmp(value, "two_finger"))
                g_ui.segments[SEG_SCROLL] = 0;
            else if (!strcmp(value, "edge"))
                g_ui.segments[SEG_SCROLL] = 1;
            else if (!strcmp(value, "button"))
                g_ui.segments[SEG_SCROLL] = 2;
        } else if (!strcmp(key, "accel_profile")) {
            g_ui.segments[SEG_ACCEL] = !strcmp(value, "flat") ? 1 : 0;
        } else if (!strcmp(key, "repeat_rate")) {
            double rate = clampd(atoi(value), 15.0, 80.0);
            g_ui.sliders[SL_REPEAT] = clampd((rate - 15.0) / 65.0, 0.0, 1.0);
        } else if (!strcmp(key, "repeat_delay")) {
            double delay = clampd(atoi(value), 150.0, 700.0);
            g_ui.sliders[SL_DELAY] = clampd((delay - 150.0) / 550.0, 0.0, 1.0);
        }
    }
    fclose(f);
    return 1;
}

/* Ask jtlabwc to re-read its configuration files.  A missing compositor is
 * not an error: the settings stay on disk and are picked up at the next
 * session start. */
static void
jtlabwc_reload(void)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char path[256];
    snprintf(path, sizeof path, "%s/jtlabwc-ctl.sock",
             (rt && *rt) ? rt : "/tmp");

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof addr.sun_path)
        return;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
        static const char msg[] = "reload\n";
        (void)!write(fd, msg, sizeof msg - 1);
    }
    close(fd);
}

static void
input_commit(void)
{
    input_save();
    jtlabwc_reload();
    ui_toast("Input settings applied");
}

/* ------------------------------------------------------------------ */
/*  Profile page: shared config, XDG portal and location search          */
/* ------------------------------------------------------------------ */

static GMainContext *g_glctx;
static GDBusConnection *g_bus;
static GPid g_own_bus_pid = -1;
static SoupSession *g_wx_session;
static guint g_portal_signal;
static char g_portal_path[256];
static uint64_t g_portal_gen;
typedef struct {
    uint64_t gen;
} PortalOp;
static PortalOp *g_portal_op;

/* Ask the bar to re-read profile.config so changes are live. */
static void
jtlab_reload(void)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char path[256];
    snprintf(path, sizeof path, "%s/jtlab-ctl.sock",
             (rt && *rt) ? rt : "/tmp");

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof addr.sun_path)
        return;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
        static const char msg[] = "reload\n";
        (void)!write(fd, msg, sizeof msg - 1);
    }
    close(fd);
}

static void
profile_config_path(char *buf, size_t n)
{
    snprintf(buf, n, "%s/.config/jt/%s", user_home(),
             PROFILE_CONFIG_FILE);
}

static int
profile_config_coord(const char *s, double lo, double hi, double *out)
{
    if (!s || !*s)
        return 0;
    char *end = NULL;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || *end || errno == ERANGE || !isfinite(v) ||
        v < lo || v > hi)
        return 0;
    *out = v;
    return 1;
}

static void
profile_config_load(void)
{
    char path[512];
    profile_config_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;

    int have_lat = 0, have_lon = 0;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *s = startup_trim(line);
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = startup_trim(s);
        char *val = startup_trim(eq + 1);

        if (!strcmp(key, "weather_location")) {
            if (*val)
                snprintf(g_profile_cfg.location, sizeof g_profile_cfg.location,
                         "%s", val);
        } else if (!strcmp(key, "weather_units")) {
            g_profile_cfg.fahrenheit =
                !strcmp(val, "fahrenheit") || !strcmp(val, "f") ||
                !strcmp(val, "degrees") || !strcmp(val, "°f");
        } else if (!strcmp(key, "weather_latitude")) {
            have_lat = profile_config_coord(val, -90.0, 90.0,
                                            &g_profile_cfg.lat);
        } else if (!strcmp(key, "weather_longitude")) {
            have_lon = profile_config_coord(val, -180.0, 180.0,
                                            &g_profile_cfg.lon);
        } else if (!strcmp(key, "profile_photo")) {
            snprintf(g_profile_cfg.photo, sizeof g_profile_cfg.photo,
                     "%s", val);
        } else if (!strcmp(key, "profile_zoom")) {
            double z = strtod(val, NULL);
            if (z >= PROFILE_ZOOM_MIN && z <= PROFILE_ZOOM_MAX)
                g_profile_cfg.zoom = z;
        }
    }
    fclose(f);

    /* A location is only usable when both coordinates were present and valid;
     * otherwise the saved weather city would resolve to longitude zero. */
    g_profile_cfg.have_coords = have_lat && have_lon;
}

static void
profile_save(void)
{
    char path[512];
    profile_config_path(path, sizeof path);
    startup_mkdirs(path);

    FILE *f = fopen(path, "w");
    if (!f) {
        ui_toast("Could not write profile.config");
        return;
    }
    fprintf(f, "# jtsettings profile configuration\n");
    fprintf(f, "# Read by JTLab; reload is sent over its control socket.\n");
    fprintf(f, "weather_location=%s\n", g_profile_cfg.location);
    fprintf(f, "weather_units=%s\n",
            g_profile_cfg.fahrenheit ? "fahrenheit" : "celsius");
    if (g_profile_cfg.have_coords) {
        fprintf(f, "weather_latitude=%.6f\n", g_profile_cfg.lat);
        fprintf(f, "weather_longitude=%.6f\n", g_profile_cfg.lon);
    }
    fprintf(f, "profile_photo=%s\n", g_profile_cfg.photo);
    fprintf(f, "profile_zoom=%.3f\n", g_profile_cfg.zoom);
    fclose(f);
    jtlab_reload();
}

/* GdkPixbuf -> premultiplied ARGB32 for the zoom preview. */
static cairo_surface_t *
pixbuf_surface(GdkPixbuf *pix)
{
    int w = gdk_pixbuf_get_width(pix);
    int h = gdk_pixbuf_get_height(pix);
    int nch = gdk_pixbuf_get_n_channels(pix);
    int rs = gdk_pixbuf_get_rowstride(pix);
    const guchar *src = gdk_pixbuf_get_pixels(pix);

    cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(sf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(sf);
        return NULL;
    }
    cairo_surface_flush(sf);
    unsigned char *dst = cairo_image_surface_get_data(sf);
    int stride = cairo_image_surface_get_stride(sf);

    for (int y = 0; y < h; y++) {
        guint32 *row = (guint32 *)(void *)(dst + y * stride);
        for (int x = 0; x < w; x++) {
            const guchar *p = src + y * rs + x * nch;
            guint32 r = p[0], g = p[1], b = p[2];
            guint32 a = (nch >= 4) ? p[3] : 255U;
            if (a == 0U)
                row[x] = 0U;
            else if (a == 255U)
                row[x] = 0xFF000000U | (r << 16) | (g << 8) | b;
            else
                row[x] = (a << 24) |
                         ((r * a / 255U) << 16) |
                         ((g * a / 255U) << 8) |
                         (b * a / 255U);
        }
    }
    cairo_surface_mark_dirty(sf);
    return sf;
}

static int
profile_preview_load(const char *path)
{
    if (g_photo_preview) {
        cairo_surface_destroy(g_photo_preview);
        g_photo_preview = NULL;
    }
    GError *err = NULL;
    GdkPixbuf *pix = gdk_pixbuf_new_from_file_at_scale(path, 320, 320,
                                                       TRUE, &err);
    if (err) g_error_free(err);
    if (!pix) return 0;
    g_photo_preview = pixbuf_surface(pix);
    g_object_unref(pix);
    return g_photo_preview != NULL;
}

/* ---- location search (Open-Meteo geocoding, same API the bar uses) ---- */

static const char *
json_key(const char *js, const char *key)
{
    size_t klen = strlen(key);
    for (const char *p = js; (p = strchr(p, '"')) != NULL; p++) {
        if (strncmp(p + 1, key, klen) != 0 || p[1 + klen] != '"') continue;
        const char *q = p + 2 + klen;
        while (*q == ' ' || *q == '\t' || *q == '\n') q++;
        if (*q == ':') return q + 1;
    }
    return NULL;
}

static const char *
json_obj_end(const char *p)
{
    if (!p || *p != '{')
        return NULL;
    int depth = 0;
    for (; *p; p++) {
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1])
                    p++;
                p++;
            }
            if (!*p)
                return NULL;
        } else if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            if (--depth == 0)
                return p;
        }
    }
    return NULL;
}

/* Lookup that cannot cross from one JSON object into the next.  Strings are
 * skipped as whole tokens so an escaped \"name\" inside a value cannot be
 * mistaken for a key in the surrounding object. */
static const char *
json_key_in(const char *p, const char *end, const char *key)
{
    if (!p || !end || end <= p)
        return NULL;
    size_t klen = strlen(key);
    for (const char *q = p; q < end;) {
        if (*q != '"') {
            q++;
            continue;
        }

        const char *str = q++;
        while (q < end && *q != '"') {
            if (*q == '\\' && q + 1 < end)
                q++;
            q++;
        }
        if (q >= end)
            return NULL;

        if ((size_t)(q - str) == klen + 1 && !strncmp(str + 1, key, klen)) {
            const char *v = q + 1;
            while (v < end && (*v == ' ' || *v == '\t' ||
                               *v == '\n' || *v == '\r'))
                v++;
            if (v < end && *v == ':')
                return v + 1;
        }
        q++;
    }
    return NULL;
}

static int
json_num(const char *p, double *out)
{
    if (!p) return 0;
    char *end;
    double v = strtod(p, &end);
    if (end == p || !isfinite(v)) return 0;
    *out = v;
    return 1;
}

static int
json_str(const char *p, char *out, size_t n)
{
    if (!out || n == 0) return 0;
    out[0] = '\0';
    if (!p || *p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p++;
        if (i + 1 < n) out[i++] = *p;
        p++;
    }
    if (*p != '"') return 0;
    out[i] = '\0';
    return 1;
}

static const char *
json_arr_at(const char *p, int i)
{
    if (!p) return NULL;
    while (*p && *p != '[') p++;
    if (*p != '[') return NULL;
    p++;
    while (i-- > 0) {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && p[1]) p++;
                    p++;
                }
                if (!*p) return NULL;
            } else if (*p == '[' || *p == '{') {
                depth++;
            } else if (*p == ']' || *p == '}') {
                if (depth == 0) return NULL;
                depth--;
            } else if (*p == ',' && depth == 0) {
                break;
            }
            p++;
        }
        if (*p != ',') return NULL;
        p++;
    }
    return p;
}

static void
wx_url_encode(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if (o + 4 >= n) break;
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~')
            out[o++] = (char)*p;
        else
            o += (size_t)snprintf(out + o, n - o, "%%%02X", *p);
    }
    out[o] = '\0';
}

static void
wx_search_finish(GObject *src, GAsyncResult *res, gpointer data)
{
    WxSearchOp *op = data;
    GError *err = NULL;
    GBytes *body = soup_session_send_and_read_finish(SOUP_SESSION(src), res,
                                                     &err);
    if (op && op->msg)
        g_object_unref(op->msg);

    /* A newer search has superseded this reply; leave its state untouched. */
    if (!op || op->gen != g_wx_gen) {
        if (body)
            g_bytes_unref(body);
        if (err)
            g_error_free(err);
        g_free(op);
        return;
    }

    g_wx_searching = 0;
    g_wx_result_count = 0;
    if (body) {
        gsize len = 0;
        const char *js = g_bytes_get_data(body, &len);
        if (js) {
            char *copy = g_strndup(js, len);
            const char *arr = json_key(copy, "results");
            for (int i = 0; i < WX_RESULTS && arr; i++) {
                const char *hit = json_arr_at(arr, i);
                if (!hit) break;
                const char *hit_end = json_obj_end(hit);
                if (!hit_end) continue;
                char name[64], country[64], admin[64];
                name[0] = country[0] = admin[0] = '\0';
                if (!json_str(json_key_in(hit, hit_end, "name"),
                              name, sizeof name))
                    continue;
                json_str(json_key_in(hit, hit_end, "country"),
                         country, sizeof country);
                json_str(json_key_in(hit, hit_end, "admin1"),
                         admin, sizeof admin);
                WxResult *r = &g_wx_results[g_wx_result_count];
                r->name[0] = '\0';
                r->lat = r->lon = 0.0;
                snprintf(r->name, sizeof r->name, "%s, %s",
                         name, country[0] ? country : admin);
                if (!json_num(json_key_in(hit, hit_end, "latitude"),
                              &r->lat) ||
                    !json_num(json_key_in(hit, hit_end, "longitude"),
                              &r->lon))
                    continue;
                g_wx_result_count++;
            }
            g_free(copy);
        }
        g_bytes_unref(body);
    }
    if (err) g_error_free(err);
    if (g_wx_result_count == 0)
        ui_toast("No matching places found");
    g_profile_dirty = 1;
    g_free(op);
}

static void
wx_search_start(void)
{
    if (!g_wx_session) {
        ui_toast("Weather search unavailable");
        return;
    }
    if (!g_ui.wx_query_input[0]) {
        ui_toast("Type a place name first");
        return;
    }
    char enc[WX_QUERY_MAX * 4];
    wx_url_encode(g_ui.wx_query_input, enc, sizeof enc);
    char url[768];
    snprintf(url, sizeof url,
             "https://geocoding-api.open-meteo.com/v1/search?name=%s"
             "&count=%d&language=en&format=json",
             enc, WX_RESULTS);
    SoupMessage *msg = soup_message_new("GET", url);
    if (!msg) {
        ui_toast("Could not build the search request");
        return;
    }
    WxSearchOp *op = g_new0(WxSearchOp, 1);
    op->gen = ++g_wx_gen;
    op->msg = msg;
    g_wx_searching = 1;
    g_wx_result_count = 0;
    g_profile_dirty = 1;
    soup_session_send_and_read_async(g_wx_session, msg, G_PRIORITY_DEFAULT,
                                     NULL, wx_search_finish, op);
}

/* ---- XDG file chooser portal ---- */

static void
portal_unsubscribe(void)
{
    if (g_bus && g_portal_signal) {
        g_dbus_connection_signal_unsubscribe(g_bus, g_portal_signal);
        g_portal_signal = 0;
    }
    g_portal_op = NULL;
}

static void
portal_response(GDBusConnection *conn, const char *sender, const char *path,
                const char *iface, const char *signal, GVariant *params,
                gpointer data)
{
    (void)conn; (void)sender; (void)path; (void)iface; (void)signal;

    PortalOp *op = data;
    if (!op || op != g_portal_op || op->gen != g_portal_gen)
        return;             /* response to a superseded picker */

    guint32 response = 0;
    GVariant *results = NULL;
    g_variant_get(params, "(u@a{sv})", &response, &results);

    if (response == 0) {
        GVariant *uris = g_variant_lookup_value(results, "uris",
                                                G_VARIANT_TYPE_STRING_ARRAY);
        if (uris && g_variant_n_children(uris) > 0) {
            const char *uri = NULL;
            g_variant_get_child(uris, 0, "&s", &uri);
            if (uri) {
                char *local = g_filename_from_uri(uri, NULL, NULL);
                if (local) {
                    snprintf(g_pending_photo, sizeof g_pending_photo,
                             "%s", local);
                    g_pending_zoom = !strcmp(local, g_profile_cfg.photo)
                                     ? g_profile_cfg.zoom : 1.0;
                    if (profile_preview_load(g_pending_photo)) {
                        g_ui.sliders[SL_PHOTO_ZOOM] =
                            (g_pending_zoom - PROFILE_ZOOM_MIN) /
                            (PROFILE_ZOOM_MAX - PROFILE_ZOOM_MIN);
                        g_zoom_open = 1;
                        ui_toast("Adjust the zoom, then Use photo");
                    } else {
                        ui_toast("Could not read that image");
                    }
                    g_free(local);
                }
            }
        }
        if (uris) g_variant_unref(uris);
    }
    if (results) g_variant_unref(results);
    portal_unsubscribe();
    g_profile_dirty = 1;
}

static void
portal_open_cb(GObject *src, GAsyncResult *res, gpointer data)
{
    PortalOp *op = data;
    GError *err = NULL;
    GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src),
                                                  res, &err);
    if (!op || op->gen != g_portal_gen) {
        if (ret)
            g_variant_unref(ret);
        if (err)
            g_error_free(err);
        g_free(op);
        return;
    }
    if (!ret) {
        ui_toast("Could not open the file chooser portal");
        if (err) g_error_free(err);
        g_free(op);
        g_profile_dirty = 1;
        return;
    }

    const char *handle = NULL;
    g_variant_get(ret, "(&o)", &handle);
    portal_unsubscribe();
    if (handle) {
        snprintf(g_portal_path, sizeof g_portal_path, "%s", handle);
        guint id = g_dbus_connection_signal_subscribe(
            g_bus, "org.freedesktop.portal.Desktop",
            "org.freedesktop.portal.Request", "Response",
            g_portal_path, NULL, G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
            portal_response, op, g_free);
        if (id) {
            g_portal_signal = id;
            g_portal_op = op;
            op = NULL;      /* owned by the signal subscription */
        }
    }
    g_variant_unref(ret);
    g_free(op);
}

static void
profile_open_picker(void)
{
    if (!g_bus) {
        ui_toast("File picker unavailable — no D-Bus session bus");
        return;
    }
    if (g_portal_signal)
        portal_unsubscribe();
    PortalOp *op = g_new0(PortalOp, 1);
    op->gen = ++g_portal_gen;

    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&opts, "{sv}", "modal", g_variant_new_boolean(TRUE));
    g_dbus_connection_call(
        g_bus,
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.FileChooser",
        "OpenFile",
        g_variant_new("(ss@a{sv})", "", "Choose profile picture",
                      g_variant_builder_end(&opts)),
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 10000,
        NULL, portal_open_cb, op);
    ui_toast("Waiting for the file chooser…");
    g_profile_dirty = 1;
}

/* A bare jtlabwc session need not export DBUS_SESSION_BUS_ADDRESS, and GLib's
 * autolaunch path needs an X11 $DISPLAY. When there is no bus to talk to, start
 * a private one so the XDG file chooser still works. The portal itself is
 * then activated over D-Bus like any other service. */
static int
start_own_session_bus(void)
{
    GError *err = NULL;
    gchar *out = NULL, *errout = NULL;
    gint status = 0;

    if (!g_spawn_command_line_sync(
            "dbus-daemon --session --fork --print-address=1 --print-pid=1",
            &out, &errout, &status, &err) ||
        status != 0 || !out || !*out) {
        fprintf(stderr, "jtsettings: could not start session bus: %s\n",
                err ? err->message : (errout && *errout ? errout : "unknown"));
        if (err) g_error_free(err);
        g_free(out);
        g_free(errout);
        return 0;
    }

    int got_address = 0;
    gchar **lines = g_strsplit(out, "\n", -1);
    if (lines[0] && *lines[0]) {
        g_setenv("DBUS_SESSION_BUS_ADDRESS", lines[0], TRUE);
        got_address = 1;
        for (int i = 1; lines[i]; i++) {
            gint64 pid = g_ascii_strtoll(lines[i], NULL, 10);
            if (pid > 0) {
                g_own_bus_pid = (GPid)pid;
                break;
            }
        }
    }
    g_strfreev(lines);
    g_free(out);
    g_free(errout);
    return got_address;
}

static void
glib_services_init(void)
{
    g_glctx = g_main_context_new();
    g_main_context_push_thread_default(g_glctx);

    GError *err = NULL;
    g_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!g_bus) {
        if (err) {
            g_error_free(err);
            err = NULL;
        }
        if (start_own_session_bus())
            g_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
        if (!g_bus) {
            fprintf(stderr, "jtsettings: XDG file portal unavailable: %s\n",
                    err ? err->message : "no session bus");
            if (err) g_error_free(err);
        }
    }
    g_wx_session = soup_session_new_with_options("timeout", 10, NULL);
}

static void
glib_services_cleanup(void)
{
    portal_unsubscribe();
    if (g_wx_session) {
        g_object_unref(g_wx_session);
        g_wx_session = NULL;
    }
    if (g_bus) {
        g_object_unref(g_bus);
        g_bus = NULL;
    }
    if (g_photo_preview) {
        cairo_surface_destroy(g_photo_preview);
        g_photo_preview = NULL;
    }
    if (g_glctx) {
        g_main_context_pop_thread_default(g_glctx);
        g_main_context_unref(g_glctx);
        g_glctx = NULL;
    }
    if (g_own_bus_pid > 0) {
        kill((pid_t)g_own_bus_pid, SIGTERM);
        g_own_bus_pid = -1;
        g_unsetenv("DBUS_SESSION_BUS_ADDRESS");
    }
}

#define DISPLAYS_FILE "displays.conf"
static void
displays_config_path(char *buf, size_t n)
{
    snprintf(buf, n, "%s/.config/jt/%s", user_home(), DISPLAYS_FILE);
}
static void
displays_save(void)
{
    char path[256];
    displays_config_path(path, sizeof path);
    startup_mkdirs(path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# jtsettings displays configuration\n");
    fprintf(f, "# Written by jtsettings, read by jtlabwc.\n");
    for (int i = 0; i < MAX_HEADS; i++) {
        const Head *h = &g_heads[i];
        if (!h->used || !h->name[0])
            continue;
        fprintf(f, "output=%s,x=%d,y=%d\n", h->name, h->p_x, h->p_y);
    }
    fclose(f);
}
static void
displays_commit(void)
{
    /* Configuration is already live through wlr-output-management; the
     * config file only preserves output positions across restarts. */
    displays_save();
}

/* ------------------------------------------------------------------ */
/*  Keybinding settings                                                */
/* ------------------------------------------------------------------ */

#define KEYBINDS_FILE "keybinds.conf"
#define TERMINAL_DEFAULT "footclient"

/* Keep this path and the file format in sync with jtlabwc.c. */
static void
keybinds_config_path(char *buf, size_t n)
{
    snprintf(buf, n, "%s/.config/jt/%s", user_home(), KEYBINDS_FILE);
}

static void
keybinds_save(void)
{
    char path[INPUT_PATH_MAX];
    keybinds_config_path(path, sizeof path);
    startup_mkdirs(path);

    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "# jtsettings keybinding configuration\n");
    fprintf(f, "# Written by jtsettings, read by jtlabwc.\n");
    fprintf(f, "terminal=%s\n", g_ui.terminal);
    fclose(f);
}

static int
keybinds_load(void)
{
    snprintf(g_ui.terminal, sizeof g_ui.terminal, "%s", TERMINAL_DEFAULT);

    char path[INPUT_PATH_MAX];
    keybinds_config_path(path, sizeof path);

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *s = startup_trim(line);
        if (!*s || *s == '#') continue;

        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = startup_trim(s);
        char *value = startup_trim(eq + 1);

        if (!strcmp(key, "terminal") && *value)
            snprintf(g_ui.terminal, sizeof g_ui.terminal, "%s", value);
    }
    fclose(f);
    return 1;
}

static void
terminal_input_cancel(void)
{
    g_ui.terminal_editing = 0;
    g_ui.terminal_input[0] = '\0';
}

static void
terminal_input_commit(void)
{
    char *cmd = startup_trim(g_ui.terminal_input);
    if (*cmd)
        snprintf(g_ui.terminal, sizeof g_ui.terminal, "%s", cmd);
    terminal_input_cancel();
    g_default_apps[0][0] = '\0';
    keybinds_save();
    defaultapps_save();
    jtlabwc_reload();
    ui_toast("Default terminal saved");
}

/* ------------------------------------------------------------------ */
/*  Default apps                                                       */
/* ------------------------------------------------------------------ */

static int
defaultapps_valid_id(const char *s)
{
    if (!s || !*s)
        return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' ||
            *p == '@' || *p == '+')
            continue;
        return 0;
    }
    return 1;
}

static void
defaultapps_config_path(char *buf, size_t n)
{
    snprintf(buf, n, "%s/.config/jt/%s", user_home(),
             DEFAULT_APPS_FILE);
}

static void
defaultapps_clean_exec(char *exec)
{
    char *out = exec;
    for (char *p = exec; *p; p++) {
        if (*p == '%' && p[1]) {
            p++;
            continue;
        }
        *out++ = *p;
    }
    *out = '\0';
    char *t = startup_trim(exec);
    if (t != exec)
        memmove(exec, t, strlen(t) + 1);
}

static int
defaultapps_parse_desktop(const char *path, DesktopApp *d)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    int nodisplay = 0, hidden = 0, type_ok = 0;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *s = startup_trim(line);
        if (!*s || *s == '#')
            continue;
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        const char *key = startup_trim(s);
        char *value = startup_trim(eq + 1);

        if (!strcmp(key, "Name") && *value)
            snprintf(d->name, sizeof d->name, "%s", value);
        else if (!strcmp(key, "Exec") && *value)
            snprintf(d->exec, sizeof d->exec, "%s", value);
        else if (!strcmp(key, "MimeType") && *value)
            snprintf(d->mimes, sizeof d->mimes, "%s", value);
        else if (!strcmp(key, "Type"))
            type_ok = !strcmp(value, "Application");
        else if (!strcmp(key, "NoDisplay"))
            nodisplay = !strcasecmp(value, "true");
        else if (!strcmp(key, "Hidden"))
            hidden = !strcasecmp(value, "true");
    }
    fclose(f);

    if (!type_ok || nodisplay || hidden || !d->name[0] || !d->exec[0])
        return 0;
    defaultapps_clean_exec(d->exec);
    return 1;
}

static void
defaultapps_scan_dir(const char *dir)
{
    DIR *dp = opendir(dir);
    if (!dp)
        return;

    struct dirent *de;
    while ((de = readdir(dp))) {
        size_t len = strlen(de->d_name);
        if (len < 9 || strcmp(de->d_name + len - 8, ".desktop"))
            continue;
        if (!defaultapps_valid_id(de->d_name))
            continue;

        int duplicate = 0;
        for (int i = 0; i < g_desktop_app_count; i++) {
            if (!strcmp(g_desktop_apps[i].id, de->d_name)) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate || g_desktop_app_count >= MAX_DESKTOP_APPS)
            continue;

        char path[768];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        DesktopApp d;
        memset(&d, 0, sizeof d);
        snprintf(d.id, sizeof d.id, "%s", de->d_name);
        if (defaultapps_parse_desktop(path, &d))
            g_desktop_apps[g_desktop_app_count++] = d;
    }
    closedir(dp);
}

static void
defaultapps_scan(void)
{
    memset(g_desktop_apps, 0, sizeof g_desktop_apps);
    g_desktop_app_count = 0;

    defaultapps_scan_dir("/usr/local/share/applications");
    defaultapps_scan_dir("/usr/share/applications");

    const char *home = getenv("HOME");
    if (home && home[0]) {
        char dir[512];
        snprintf(dir, sizeof dir, "%s/.local/share/applications", home);
        defaultapps_scan_dir(dir);
    }

    const char *data_dirs = getenv("XDG_DATA_DIRS");
    if (data_dirs && *data_dirs) {
        char copy[2048];
        snprintf(copy, sizeof copy, "%s", data_dirs);
        char *save = NULL;
        for (char *item = strtok_r(copy, ":", &save); item;
             item = strtok_r(NULL, ":", &save)) {
            char dir[640];
            snprintf(dir, sizeof dir, "%s/applications", item);
            defaultapps_scan_dir(dir);
        }
    }
}

static int
defaultapps_app_index(const char *id)
{
    if (!id || !*id)
        return -1;
    for (int i = 0; i < g_desktop_app_count; i++)
        if (!strcmp(g_desktop_apps[i].id, id))
            return i;
    return -1;
}

static int
defaultapps_matches(int cat, const DesktopApp *d)
{
    if (cat < 0 || cat >= APP_CAT_COUNT || !d)
        return 0;

    const AppCategory *c = &APP_CATEGORIES[cat];
    if (c->terminal) {
        if (strcasestr(d->id, "server"))
            return 0;
        static const char *const hints[] = {
            "terminal", "term", "foot", "kitty", "alacritty",
            "wezterm", "xterm", "konsole", "ghostty", NULL
        };
        char hay[APP_ID_MAX + APP_NAME_MAX + APP_EXEC_MAX];
        snprintf(hay, sizeof hay, "%s %s %s", d->id, d->name, d->exec);
        for (int i = 0; hints[i]; i++)
            if (strcasestr(hay, hints[i]))
                return 1;
        return 0;
    }

    for (int i = 0; i < c->mime_count; i++)
        if (c->mimes[i] && strstr(d->mimes, c->mimes[i]))
            return 1;
    return 0;
}

static int
defaultapps_candidates(int cat, int *out, int max)
{
    int n = 0;
    if (cat < 0 || cat >= APP_CAT_COUNT)
        return 0;

    int pref = defaultapps_app_index(APP_CATEGORIES[cat].preferred);
    if (pref >= 0 && defaultapps_matches(cat, &g_desktop_apps[pref]) && n < max)
        out[n++] = pref;

    for (int i = 0; i < g_desktop_app_count && n < max; i++) {
        if (i == pref || !defaultapps_matches(cat, &g_desktop_apps[i]))
            continue;
        out[n++] = i;
    }
    return n;
}

static void
defaultapps_query_id(const char *command, char *out, size_t n)
{
    out[0] = '\0';
    FILE *p = popen(command, "r");
    if (!p)
        return;
    char line[APP_ID_MAX + 64];
    if (fgets(line, sizeof line, p)) {
        char *s = startup_trim(line);
        if (defaultapps_valid_id(s))
            snprintf(out, n, "%s", s);
    }
    pclose(p);
}

static void
defaultapps_load(void)
{
    defaultapps_scan();
    memset(g_default_apps, 0, sizeof g_default_apps);

    for (int i = 1; i < APP_CAT_COUNT; i++) {
        if (APP_CATEGORIES[i].mime_count) {
            char cmd[320];
            snprintf(cmd, sizeof cmd, "xdg-mime query default %s 2>/dev/null",
                     APP_CATEGORIES[i].mimes[0]);
            defaultapps_query_id(cmd, g_default_apps[i],
                                 sizeof g_default_apps[i]);
        }
        if (i == 1) {
            defaultapps_query_id(
                "xdg-settings get default-web-browser 2>/dev/null",
                g_default_apps[i], sizeof g_default_apps[i]);
        }
    }

    char path[512];
    defaultapps_config_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f)
        return;

    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = startup_trim(line);
        if (!*s || *s == '#')
            continue;
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = startup_trim(s);
        char *value = startup_trim(eq + 1);
        if (!defaultapps_valid_id(value))
            continue;
        for (int i = 0; i < APP_CAT_COUNT; i++) {
            if (!strcmp(key, APP_CATEGORIES[i].key)) {
                snprintf(g_default_apps[i], sizeof g_default_apps[i],
                         "%s", value);
                break;
            }
        }
    }
    fclose(f);
}

static void
defaultapps_save(void)
{
    char path[512];
    defaultapps_config_path(path, sizeof path);
    startup_mkdirs(path);

    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "# jtsettings default applications\n");
    fprintf(f, "# Written by jtsettings; XDG handlers are updated live.\n");
    for (int i = 0; i < APP_CAT_COUNT; i++)
        if (g_default_apps[i][0])
            fprintf(f, "%s=%s\n", APP_CATEGORIES[i].key, g_default_apps[i]);
    fclose(f);
}

static void
defaultapps_selected_name(int cat, char *buf, size_t n)
{
    if (cat < 0 || cat >= APP_CAT_COUNT) {
        snprintf(buf, n, "Not set");
        return;
    }
    if (cat == 0 && !g_default_apps[0][0]) {
        snprintf(buf, n, "%s", g_ui.terminal);
        return;
    }
    if (!g_default_apps[cat][0]) {
        snprintf(buf, n, "Not set");
        return;
    }
    int idx = defaultapps_app_index(g_default_apps[cat]);
    if (idx >= 0 && g_desktop_apps[idx].name[0])
        snprintf(buf, n, "%s", g_desktop_apps[idx].name);
    else
        snprintf(buf, n, "%s", g_default_apps[cat]);
}

static void
defaultapps_choose(int cat, int candidate)
{
    if (cat < 0 || cat >= APP_CAT_COUNT)
        return;

    if (candidate < 0) {
        g_ui.app_picker_open = 0;
        g_ui.terminal_editing = 1;
        snprintf(g_ui.terminal_input, sizeof g_ui.terminal_input,
                 "%s", g_ui.terminal);
        defaultapps_mark_dirty();
        return;
    }

    int ids[MAX_DESKTOP_APPS];
    int count = defaultapps_candidates(cat, ids, MAX_DESKTOP_APPS);
    if (candidate >= count)
        return;

    const DesktopApp *d = &g_desktop_apps[ids[candidate]];
    char id[APP_ID_MAX];
    snprintf(id, sizeof id, "%s", d->id);
    if (!defaultapps_valid_id(id))
        return;

    g_ui.app_picker_open = 0;

    if (cat == 0) {
        char cmd[APP_EXEC_MAX];
        snprintf(cmd, sizeof cmd, "%s", d->exec);
        defaultapps_clean_exec(cmd);
        if (!cmd[0])
            return;
        snprintf(g_ui.terminal, sizeof g_ui.terminal, "%s", cmd);
        snprintf(g_default_apps[0], sizeof g_default_apps[0], "%s", id);
        keybinds_save();
        defaultapps_save();
        jtlabwc_reload();
        ui_toast("Default apps saved");
        defaultapps_mark_dirty();
        return;
    }

    char cmd[1024];
    size_t used = snprintf(cmd, sizeof cmd, "xdg-mime default %s", id);
    for (int i = 0; i < APP_CATEGORIES[cat].mime_count &&
                    used < sizeof cmd; i++)
        used += (size_t)snprintf(cmd + used, sizeof cmd - used, " %s",
                                 APP_CATEGORIES[cat].mimes[i]);
    snprintf(cmd + used, sizeof cmd - used, " >/dev/null 2>&1");

    int ok = system(cmd) == 0;
    if (cat == 1) {
        char browser_cmd[512];
        snprintf(browser_cmd, sizeof browser_cmd,
                 "xdg-settings set default-web-browser %s >/dev/null 2>&1", id);
        if (system(browser_cmd) != 0) { }
    }

    if (!ok) {
        ui_toast("Could not set that default app");
        defaultapps_mark_dirty();
        return;
    }

    snprintf(g_default_apps[cat], sizeof g_default_apps[cat], "%s", id);
    defaultapps_save();
    ui_toast("Default apps saved");
    defaultapps_mark_dirty();
}

#define APP_PICKER_MAX_ROWS 10

static int
app_picker_visible_rows(int count, int custom)
{
    int rows = count < APP_PICKER_MAX_ROWS ? count : APP_PICKER_MAX_ROWS;
    /* Category 0 keeps its last row for "Custom command...". */
    if (custom && rows == APP_PICKER_MAX_ROWS)
        rows--;
    return rows;
}

static int
app_picker_scroll_max(void)
{
    int cat = g_ui.app_picker_cat;
    if (cat < 0 || cat >= APP_CAT_COUNT)
        return 0;
    int ids[MAX_DESKTOP_APPS];
    int count = defaultapps_candidates(cat, ids, MAX_DESKTOP_APPS);
    int visible = app_picker_visible_rows(count, cat == 0);
    int max = count - visible;
    return max > 0 ? max : 0;
}

static void
app_picker_rect(double *px, double *py, double *pw, double *ph)
{
    int cat = g_ui.app_picker_cat;
    int ids[MAX_DESKTOP_APPS];
    int count = (cat >= 0 && cat < APP_CAT_COUNT)
        ? defaultapps_candidates(cat, ids, MAX_DESKTOP_APPS) : 0;
    int rows = app_picker_visible_rows(count, cat == 0);
    if (cat == 0)
        rows++;

    *pw = 560.0;
    *ph = 116.0 + rows * 36.0 + 56.0;
    *px = (WIN_W - *pw) / 2.0;
    *py = (WIN_H - *ph) / 2.0;
}

static void
paint_app_picker(cairo_t *cr)
{
    if (!g_ui.app_picker_open)
        return;
    int cat = g_ui.app_picker_cat;
    if (cat < 0 || cat >= APP_CAT_COUNT) {
        g_ui.app_picker_open = 0;
        return;
    }

    int ids[MAX_DESKTOP_APPS];
    int count = defaultapps_candidates(cat, ids, MAX_DESKTOP_APPS);
    int custom = cat == 0;
    int visible = app_picker_visible_rows(count, custom);
    int max_scroll = app_picker_scroll_max();
    if (g_ui.app_picker_scroll > max_scroll) {
        g_ui.app_picker_scroll = max_scroll;
        g_ui.hover = -1;
        g_ui.pressed = -1;
    }

    double px, py, pw, ph;
    app_picker_rect(&px, &py, &pw, &ph);

    rgba(cr, 0x000000, 0.58);
    cairo_rectangle(cr, 0.0, 0.0, WIN_W, WIN_H);
    cairo_fill(cr);
    paint_card(cr, px, py, pw, ph);

    text_draw(cr, APP_CATEGORIES[cat].label, 16.0, 700, C_FG, 0.98,
              px + 22.0, py + 18.0, ALIGN_LEFT, 0.0);
    text_draw(cr, "Select a detected application", 9.5, 400, C_DIM, 0.8,
              px + 22.0, py + 44.0, ALIGN_LEFT, 0.1);
    if (max_scroll > 0) {
        char hint[32];
        snprintf(hint, sizeof hint, "Scroll · %d/%d",
                 g_ui.app_picker_scroll + 1, count);
        text_draw(cr, hint, 8.5, 500, C_FAINT, 0.8,
                  px + pw - 22.0, py + 44.0, ALIGN_RIGHT, 0.1);
    }

    if (count == 0 && !custom) {
        text_draw(cr, "No matching applications were found.", 10.0, 400,
                  C_DIM, 0.85, px + 22.0, py + 84.0, ALIGN_LEFT, 0.1);
    }

    for (int row = 0; row < visible; row++) {
        int i = g_ui.app_picker_scroll + row;
        double ry = py + 76.0 + row * 36.0;
        if (i >= count)
            continue;

        const DesktopApp *d = &g_desktop_apps[ids[i]];
        int on = !strcmp(g_default_apps[cat], d->id);
        if (cat == 0 && !g_default_apps[0][0] &&
            !strcmp(g_ui.terminal, d->exec))
            on = 1;
        int idx = add_hit(HIT_APP_PICK, cat, i,
                          px + 16.0, ry, pw - 32.0, 32.0);
        int hot = hit_hover(idx);

        rgba(cr, on ? C_ACCENT : C_TRACK, on ? 0.16 : (hot ? 0.9 : 0.7));
        rounded_rect(cr, px + 16.0, ry, pw - 32.0, 32.0, 7.0);
        cairo_fill(cr);
        rgba(cr, on ? C_ACCENT : C_BORDER, on ? 0.65 : 0.32);
        cairo_set_line_width(cr, 1.0);
        rounded_rect(cr, px + 16.5, ry + 0.5, pw - 33.0, 31.0, 6.5);
        cairo_stroke(cr);

        text_draw(cr, d->name, 10.5, 500, on ? C_ACCENT : C_FG,
                  on ? 1.0 : 0.92, px + 30.0, ry + 8.0, ALIGN_LEFT, 0.1);
        text_draw(cr, d->id, 8.5, 400, C_FAINT, 0.75,
                  px + pw - 30.0, ry + 9.0, ALIGN_RIGHT, 0.1);
    }

    if (custom) {
        double ry = py + 76.0 + visible * 36.0;
        int idx = add_hit(HIT_APP_PICK, cat, -1,
                          px + 16.0, ry, pw - 32.0, 32.0);
        int hot = hit_hover(idx);
        rgba(cr, hot ? C_TRACK : C_CARD, hot ? 0.95 : 0.8);
        rounded_rect(cr, px + 16.0, ry, pw - 32.0, 32.0, 7.0);
        cairo_fill(cr);
        text_draw(cr, "Custom command…", 10.5, 500, C_FG, 0.92,
                  px + 30.0, ry + 9.0, ALIGN_LEFT, 0.1);
    }

    double bx = px + pw - 22.0 - 92.0;
    double by = py + ph - 20.0 - 32.0;
    int close_idx = add_hit(HIT_APP_PICKER_CLOSE, 0, 0, bx, by, 92.0, 32.0);
    int close_hot = hit_hover(close_idx);
    rgba(cr, close_hot ? C_TRACK : C_CARD, 0.95);
    rounded_rect(cr, bx, by, 92.0, 32.0, 7.0);
    cairo_fill(cr);
    rgba(cr, C_BORDER, 0.45);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, bx + 0.5, by + 0.5, 91.0, 31.0, 6.5);
    cairo_stroke(cr);
    text_draw(cr, "Cancel", 10.0, 500, C_DIM, 0.9,
              bx + 46.0, by + 9.0, ALIGN_CENTER, 0.2);
}

static void
startup_add(const char *cmd)
{
    if (g_startup_count >= STARTUP_MAX)
        return;
    snprintf(g_startups[g_startup_count].command, STARTUP_CMD_MAX,
             "%s", cmd);
    g_startups[g_startup_count].enabled = 1;
    g_startup_count++;
    startup_save();
}

static void
startup_remove(int index)
{
    if (index < 0 || index >= g_startup_count)
        return;
    for (int i = index; i + 1 < g_startup_count; i++)
        g_startups[i] = g_startups[i + 1];
    g_startup_count--;
    startup_save();
}

static void
startup_input_cancel(void)
{
    g_ui.startup_adding = 0;
    g_ui.startup_input[0] = '\0';
}

static void
startup_input_commit(void)
{
    char *cmd = startup_trim(g_ui.startup_input);
    if (*cmd)
        startup_add(cmd);
    startup_input_cancel();
}

static void paint_app_icon(cairo_t *cr, double x, double y, double size,
                           const char *label, unsigned tint)
{
    rgba(cr, tint, 0.13);
    rounded_rect(cr, x, y, size, size, 8.0);
    cairo_fill(cr);
    rgba(cr, tint, 0.55);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, x + 0.5, y + 0.5, size - 1.0, size - 1.0, 7.5);
    cairo_stroke(cr);

    char letter[2] = { label[0], '\0' };
    text_draw(cr, letter, 12.0, 700, tint, 0.95,
              x + size / 2.0, y + (size - 15.0) / 2.0, ALIGN_CENTER, 0.0);
}

static void paint_switch(cairo_t *cr, double x, double y, int on,
                         int kind, int id)
{
    double tw = 38.0, th = 20.0;
    int idx = add_hit(kind, id, 0, x - 4.0, y - 4.0,
                      tw + 8.0, th + 8.0);
    int hot = hit_hover(idx);

    rgba(cr, on ? C_ACCENT : C_TRACK, on ? (hot ? 0.95 : 0.85) : 0.9);
    rounded_rect(cr, x, y, tw, th, th / 2.0);
    cairo_fill(cr);
    if (!on) {
        rgba(cr, C_BORDER, 0.55);
        cairo_set_line_width(cr, 1.0);
        rounded_rect(cr, x + 0.5, y + 0.5, tw - 1.0, th - 1.0,
                     th / 2.0 - 0.5);
        cairo_stroke(cr);
    }
    double kx = on ? x + tw - th / 2.0 : x + th / 2.0;
    rgba(cr, on ? C_ON_ACCENT : C_DIM, on ? 1.0 : 0.75);
    cairo_arc(cr, kx, y + th / 2.0, 6.4, 0.0, 2.0 * PI);
    cairo_fill(cr);
}

static void paint_mini_toggle_ex(cairo_t *cr, double x, double y,
                                 int on, int kind, int a)
{
    double tw = 38.0, th = 20.0;
    int idx = add_hit(kind, a, 0, x - 4.0, y - 4.0,
                      tw + 8.0, th + 8.0);
    int hot = hit_hover(idx);

    rgba(cr, on ? C_ACCENT : C_TRACK, on ? (hot ? 0.95 : 0.85) : 0.9);
    rounded_rect(cr, x, y, tw, th, th / 2.0);
    cairo_fill(cr);
    if (!on) {
        rgba(cr, C_BORDER, 0.55);
        cairo_set_line_width(cr, 1.0);
        rounded_rect(cr, x + 0.5, y + 0.5, tw - 1.0, th - 1.0,
                     th / 2.0 - 0.5);
        cairo_stroke(cr);
    }
    double kx = on ? x + tw - th / 2.0 : x + th / 2.0;
    rgba(cr, on ? C_ON_ACCENT : C_DIM, on ? 1.0 : 0.75);
    cairo_arc(cr, kx, y + th / 2.0, 6.4, 0.0, 2.0 * PI);
    cairo_fill(cr);
}


static void paint_mini_toggle(cairo_t *cr, double x, double y,
                              int on, int toggle_id)
{
    paint_switch(cr, x, y, on, HIT_TOGGLE, toggle_id);
}

static void page_startup(cairo_t *cr)
{
    section_label(cr, "AUTOSTART", CONTENT_X, 102.0);
    text_draw(cr, "Applications launched when the jtlabwc session starts",
              9.5, 400, C_DIM, 0.75, CONTENT_X, 117.0, ALIGN_LEFT, 0.1);

    double tx = CONTENT_X, ty = 142.0, tw = CONTENT_W;
    double by = WIN_H - 122.0;
    double th = (by - 12.0) - ty;
    paint_card(cr, tx, ty, tw, th);

    text_draw(cr, "APPLICATION", 8.0, 600, C_FAINT, 0.9,
              tx + 20.0, ty + 14.0, ALIGN_LEFT, 1.4);
    text_draw(cr, "ENABLED", 8.0, 600, C_FAINT, 0.9,
              tx + tw - 20.0, ty + 14.0, ALIGN_RIGHT, 1.4);
    paint_divider(cr, tx + 20.0, ty + 34.0, tw - 40.0);

    if (g_startup_count == 0) {
        text_draw(cr, "No startup applications", 14.0, 600, C_FG, 0.9,
                  tx + tw / 2.0, ty + th / 2.0 - 54.0,
                  ALIGN_CENTER, 0.15);
        text_draw(cr,
                  "Add an application and it will launch with your session.",
                  9.5, 400, C_DIM, 0.72, tx + tw / 2.0, ty + th / 2.0 - 28.0,
                  ALIGN_CENTER, 0.1);
    }

    for (int i = 0; i < g_startup_count; i++) {
        double ry = ty + 42.0 + i * 44.0;
        char name[64];
        startup_name(g_startups[i].command, name, sizeof name);

        paint_app_icon(cr, tx + 22.0, ry + 7.0, 28.0, name, C_ACCENT);

        text_draw(cr, name, 11.0, 600, C_FG, 0.95,
                  tx + 62.0, ry + 4.0, ALIGN_LEFT, 0.1);
        text_draw(cr, g_startups[i].command, 8.5, 400, C_DIM, 0.75,
                  tx + 62.0, ry + 22.0, ALIGN_LEFT, 0.1);

        paint_button(cr, tx + tw - 136.0, ry + 9.0, 66.0, 26.0,
                     "Remove", 0, BTN_START_REMOVE_FIRST + i);

        paint_switch(cr, tx + tw - 58.0, ry + 12.0,
                     g_startups[i].enabled, HIT_START_TOGGLE, i);

        if (i < g_startup_count - 1)
            paint_divider(cr, tx + 20.0, ry + 43.0, tw - 40.0);
    }

    double ay = ty + th - 40.0;
    if (g_ui.startup_adding) {
        double fx = tx + 20.0, fw = tw - 40.0, fh = 32.0;
        rgba(cr, C_TRACK, 0.95);
        rounded_rect(cr, fx, ay, fw, fh, 7.0);
        cairo_fill(cr);
        rgba(cr, C_ACCENT, 0.80);
        cairo_set_line_width(cr, 1.0);
        rounded_rect(cr, fx + 0.5, ay + 0.5, fw - 1.0, fh - 1.0, 6.5);
        cairo_stroke(cr);

        const char *input = g_ui.startup_input;
        if (*input) {
            text_draw(cr, input, 10.5, 500, C_FG, 0.95,
                      fx + 12.0, ay + 9.0, ALIGN_LEFT, 0.1);
        } else {
            text_draw(cr, "Type a command, e.g. foot --server",
                      10.5, 500, C_FAINT, 0.85,
                      fx + 12.0, ay + 9.0, ALIGN_LEFT, 0.1);
        }

        if ((int)(g_ui.t * 2.0) % 2 == 0) {
            double cx = fx + 12.0 +
                text_width(cr, input, 10.5, 500, 0.1) + 2.0;
            rgba(cr, C_ACCENT, 0.9);
            cairo_set_line_width(cr, 1.2);
            cairo_move_to(cr, cx, ay + 8.0);
            cairo_line_to(cr, cx, ay + 24.0);
            cairo_stroke(cr);
        }

        text_draw(cr, "Enter to add · Esc to cancel", 8.5, 500,
                  C_FAINT, 0.8, fx + fw - 12.0, ay + 10.0,
                  ALIGN_RIGHT, 0.2);
    } else {
        paint_dashed_button(cr, tx + 20.0, ay, tw - 40.0, 32.0,
                            "Add application", BTN_START_ADD);
    }

    double bh = 72.0;
    double col_w = (CONTENT_W - 20.0) / 2.0;
    double col2 = CONTENT_X + col_w + 20.0;
    paint_card(cr, CONTENT_X, by, col_w, bh);
    char v[32];
    paint_slider_row(cr, CONTENT_X + 18.0, by + 12.0, col_w - 36.0,
                     "Launch delay",
                     slider_text(SL_START_DELAY,
                                 g_ui.sliders[SL_START_DELAY], v, sizeof v),
                     g_ui.sliders[SL_START_DELAY], SL_START_DELAY);
    text_draw(cr, "Wait before the first application starts", 8.5, 400,
              C_FAINT, 0.8, CONTENT_X + 18.0, by + 50.0, ALIGN_LEFT, 0.1);

    paint_card(cr, col2, by, col_w, bh);
    text_draw(cr, "AUTOSTART FILE", 8.0, 600, C_FAINT, 0.9,
              col2 + 18.0, by + 14.0, ALIGN_LEFT, 1.4);
    text_draw(cr, "~/.config/jt/startup.conf", 10.0, 500, C_ACCENT, 0.95,
              col2 + 18.0, by + 32.0, ALIGN_LEFT, 0.2);
    text_draw(cr, "Changes are written immediately", 8.5, 400, C_DIM, 0.75,
              col2 + 18.0, by + 50.0, ALIGN_LEFT, 0.1);
}

/* ------------------------------------------------------------------ */
/*  Page: Default Apps                                                 */
/* ------------------------------------------------------------------ */

static void page_defaultapps(cairo_t *cr)
{
    section_label(cr, "DEFAULT APPS", CONTENT_X, 102.0);
    text_draw(cr, "Choose the applications jtlab and your desktop open by default",
              9.5, 400, C_DIM, 0.75, CONTENT_X, 117.0, ALIGN_LEFT, 0.1);

    /* Match the Keybindings page: leave breathing room under the header. */
    double y = 152.0;
    double row_h = 58.0;
    double gap = 6.0;
    double field_w = 292.0;
    double field_x = CONTENT_X + CONTENT_W - field_w - 20.0;

    for (int i = 0; i < APP_CAT_COUNT; i++) {
        double ry = y + i * (row_h + gap);
        paint_card(cr, CONTENT_X, ry, CONTENT_W, row_h);

        text_draw(cr, APP_CATEGORIES[i].label, 12.0, 600, C_FG, 0.96,
                  CONTENT_X + 20.0, ry + 12.0, ALIGN_LEFT, 0.1);
        text_draw(cr, APP_CATEGORIES[i].desc, 8.8, 400, C_DIM, 0.75,
                  CONTENT_X + 20.0, ry + 33.0, ALIGN_LEFT, 0.1);

        int idx = add_hit(HIT_APP_FIELD, i, 0,
                          field_x, ry + 11.0, field_w, 36.0);
        int hot = hit_hover(idx);

        rgba(cr, C_TRACK, 0.9);
        rounded_rect(cr, field_x, ry + 11.0, field_w, 36.0, 7.0);
        cairo_fill(cr);
        rgba(cr, hot ? C_ACCENT : C_BORDER, hot ? 0.75 : 0.5);
        cairo_set_line_width(cr, 1.0);
        rounded_rect(cr, field_x + 0.5, ry + 11.5, field_w - 1.0, 35.0, 6.5);
        cairo_stroke(cr);

        cairo_save(cr);
        cairo_rectangle(cr, field_x + 1.0, ry + 12.0,
                        field_w - 2.0, 34.0);
        cairo_clip(cr);
        if (i == 0 && g_ui.terminal_editing) {
            const char *input = g_ui.terminal_input;
            text_draw(cr, input, 10.5, 500, C_FG, 0.95,
                      field_x + 12.0, ry + 20.0, ALIGN_LEFT, 0.1);
            if ((int)(g_ui.t * 2.0) % 2 == 0) {
                double caret = field_x + 12.0 +
                    text_width(cr, input, 10.5, 500, 0.1) + 2.0;
                rgba(cr, C_ACCENT, 0.9);
                cairo_set_line_width(cr, 1.2);
                cairo_move_to(cr, caret, ry + 19.0);
                cairo_line_to(cr, caret, ry + 35.0);
                cairo_stroke(cr);
            }
        } else {
            char name[APP_NAME_MAX + APP_ID_MAX];
            defaultapps_selected_name(i, name, sizeof name);
            text_draw(cr, name, 10.5, 500, C_FG, 0.94,
                      field_x + 12.0, ry + 20.0, ALIGN_LEFT, 0.1);
        }
        cairo_restore(cr);

        rgba(cr, C_DIM, 0.85);
        double cx = field_x + field_w - 18.0;
        double cy = ry + 29.0;
        cairo_set_line_width(cr, 1.4);
        cairo_move_to(cr, cx - 4.0, cy - 2.5);
        cairo_line_to(cr, cx, cy + 2.0);
        cairo_line_to(cr, cx + 4.0, cy - 2.5);
        cairo_stroke(cr);
    }

    text_wrap(cr,
        "Defaults are applied through XDG and stored in "
        "~/.config/jt/defaultapps.conf. The terminal is also written to "
        "keybinds.conf for Super + Return.",
        8.8, 400, C_FAINT, 0.8, CONTENT_X,
        y + (APP_CAT_COUNT - 1) * (row_h + gap) + row_h + 8.0,
        CONTENT_W, 2);
}

/* ------------------------------------------------------------------ */
/*  Page: Window Rules                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    const char *desc;
    const char *match[2];
    int n_match;
    const char *action[2];
    int n_action;
    int toggle;
} RuleRow;

static const RuleRow RULES[] = {
    { "Picture-in-Picture",
      "Keep small video previews above everything else, without stealing "
      "keyboard focus.",
      { "app_id = firefox", "title ~ Picture-in-Picture" }, 2,
      { "Always on top" }, 1, TOG_RULE_PIP },
    { "Terminal",
      "Give foot a comfortable starting size and hide the server-side "
      "decorations.",
      { "app_id = foot", "role = toplevel" }, 2,
      { "960 × 620", "No decorations" }, 2, TOG_RULE_TERM },
    { "Browser",
      "Place Firefox on the second workspace and open it maximised.",
      { "app_id = firefox" }, 1,
      { "Workspace 2", "Maximise" }, 2, TOG_RULE_WEB },
};
#define RULE_COUNT ((int)(sizeof RULES / sizeof RULES[0]))

static void page_rules(cairo_t *cr)
{
    section_label(cr, "WINDOW RULES", CONTENT_X, 102.0);
    text_draw(cr, "Rules are matched top to bottom; the first match wins",
              9.5, 400, C_DIM, 0.75, CONTENT_X, 117.0, ALIGN_LEFT, 0.1);

    double btn_y = WIN_H - 180.0;
    double card_h = 116.0;
    double gap = 14.0 + (WIN_H - DESIGN_H) / (RULE_COUNT - 1);
    if (gap < 14.0)
        gap = 14.0;

    for (int i = 0; i < RULE_COUNT; i++) {
        double ry = 142.0 + i * (card_h + gap);
        int idx = add_hit(HIT_ROW, i, 0, CONTENT_X, ry, CONTENT_W, card_h);
        int sel = g_ui.selected[PAGE_RULES] == i;
        int hot = hit_hover(idx);

        paint_card(cr, CONTENT_X, ry, CONTENT_W, card_h);
        if (sel) {
            rgba(cr, C_ACCENT, 0.55);
            cairo_set_line_width(cr, 1.2);
            rounded_rect(cr, CONTENT_X + 0.6, ry + 0.6, CONTENT_W - 1.2,
                         card_h - 1.2, 9.4);
            cairo_stroke(cr);
        } else if (hot) {
            rgba(cr, C_FG, 0.20);
            cairo_set_line_width(cr, 1.0);
            rounded_rect(cr, CONTENT_X + 0.5, ry + 0.5, CONTENT_W - 1.0,
                         card_h - 1.0, 9.5);
            cairo_stroke(cr);
        }

        rgba(cr, C_ACCENT, sel ? 0.9 : 0.55);
        rounded_rect(cr, CONTENT_X, ry + 20.0, 3.0, 76.0, 1.5);
        cairo_fill(cr);

        text_draw(cr, RULES[i].name, 13.0, 600, C_FG, 0.96,
                  CONTENT_X + 20.0, ry + 16.0, ALIGN_LEFT, 0.1);
        text_wrap(cr, RULES[i].desc, 9.2, 400, C_DIM, 0.78,
                  CONTENT_X + 20.0, ry + 36.0, 520.0, 2);

        double mx = CONTENT_X + 20.0;
        for (int k = 0; k < RULES[i].n_match; k++)
            mx += paint_chip(cr, RULES[i].match[k], mx, ry + 74.0, 0) + 7.0;

        double ax = CONTENT_X + CONTENT_W - 20.0;
        for (int k = RULES[i].n_action - 1; k >= 0; k--) {
            double w = chip_width(cr, RULES[i].action[k]);
            ax -= w;
            paint_chip(cr, RULES[i].action[k], ax, ry + 74.0, 1);
            ax -= 7.0;
        }

        paint_mini_toggle(cr, CONTENT_X + CONTENT_W - 62.0, ry + 16.0,
                          g_ui.toggles[RULES[i].toggle], RULES[i].toggle);
    }

    paint_dashed_button(cr, CONTENT_X, btn_y, CONTENT_W, 52.0,
                        "New window rule", BTN_NEW_RULE);

    text_wrap(cr,
        "A finished app would read and write these rules in "
        "~/.config/labwc/rc.xml, then ask jtlabwc to reload. The preview "
        "keeps everything local.",
        8.8, 400, C_FAINT, 0.8, CONTENT_X, WIN_H - 108.0, CONTENT_W, 2);
}

/* ------------------------------------------------------------------ */
/*  Page: Input                                                        */
/* ------------------------------------------------------------------ */

static void page_input(cairo_t *cr)
{
    double col_w = (CONTENT_W - 20.0) / 2.0;
    double col2 = CONTENT_X + col_w + 20.0;
    double inner = col_w - 36.0;

    section_label(cr, "POINTER & TOUCHPAD", CONTENT_X, 102.0);
    section_label(cr, "KEYBOARD", col2, 102.0);

    char v[32];
    double cy = 122.0;
    double gy = WIN_H - 278.0;
    double ch = gy - 20.0 - cy;

    paint_card(cr, CONTENT_X, cy, col_w, ch);
    paint_card(cr, col2, cy, col_w, ch);

    double lx = CONTENT_X + 18.0;
    text_draw(cr, "Pointer", 12.0, 600, 0xE8E8E8, 0.95,
              lx, cy + 14.0, ALIGN_LEFT, 0.1);

    paint_slider_row(cr, lx, cy + 44.0, inner, "Pointer speed",
                     slider_text(SL_POINTER, g_ui.sliders[SL_POINTER],
                                 v, sizeof v),
                     g_ui.sliders[SL_POINTER], SL_POINTER);
    paint_divider(cr, lx, cy + 88.0, inner);

    paint_toggle_row(cr, lx, cy + 98.0, inner, "Natural scrolling",
                     "Content follows your fingers",
                     g_ui.toggles[TOG_NATURAL], TOG_NATURAL);
    paint_toggle_row(cr, lx, cy + 140.0, inner, "Tap to click",
                     "Tap the touchpad to press",
                     g_ui.toggles[TOG_TAP], TOG_TAP);
    paint_toggle_row(cr, lx, cy + 182.0, inner, "Tap and drag",
                     "Drag with one finger held down",
                     g_ui.toggles[TOG_DRAG], TOG_DRAG);
    paint_toggle_row(cr, lx, cy + 224.0, inner, "Disable while typing",
                     "Ignore taps for a short moment",
                     g_ui.toggles[TOG_DWT], TOG_DWT);
    paint_toggle_row(cr, lx, cy + 266.0, inner, "Left-handed",
                     "Swap the primary mouse button",
                     g_ui.toggles[TOG_LEFT], TOG_LEFT);

    double rx = col2 + 18.0;
    text_draw(cr, "Keyboard", 12.0, 600, 0xE8E8E8, 0.95,
              rx, cy + 14.0, ALIGN_LEFT, 0.1);

    text_draw(cr, "Layout", 11.5, 500, 0xD2D2D2, 0.94,
              rx, cy + 44.0, ALIGN_LEFT, 0.1);
    paint_field(cr, rx, cy + 62.0, inner, 32.0,
                "English (US) · intl", 0);

    paint_slider_row(cr, rx, cy + 108.0, inner, "Repeat rate",
                     slider_text(SL_REPEAT, g_ui.sliders[SL_REPEAT],
                                 v, sizeof v),
                     g_ui.sliders[SL_REPEAT], SL_REPEAT);
    paint_slider_row(cr, rx, cy + 156.0, inner, "Repeat delay",
                     slider_text(SL_DELAY, g_ui.sliders[SL_DELAY],
                                 v, sizeof v),
                     g_ui.sliders[SL_DELAY], SL_DELAY);

    paint_divider(cr, rx, cy + 200.0, inner);

    text_draw(cr, "Compose key", 11.5, 500, 0xD2D2D2, 0.94,
              rx, cy + 212.0, ALIGN_LEFT, 0.1);
    paint_field(cr, rx, cy + 230.0, inner, 32.0, "Disabled", 0);
    text_draw(cr, "Repeat settings apply to every keyboard", 8.5, 400,
              C_FAINT, 0.8, rx, cy + 272.0, ALIGN_LEFT, 0.1);

    paint_card(cr, CONTENT_X, gy, CONTENT_W, 104.0);
    text_draw(cr, "Pointer options", 12.0, 600, 0xE8E8E8, 0.95,
              CONTENT_X + 18.0, gy + 16.0, ALIGN_LEFT, 0.1);

    double bw = (CONTENT_W - 36.0 - 40.0) / 3.0;
    double bx1 = CONTENT_X + 18.0;
    double bx2 = bx1 + bw + 20.0;
    double bx3 = bx2 + bw + 20.0;

    text_draw(cr, "Scroll method", 9.5, 500, C_DIM, 0.85,
              bx1, gy + 42.0, ALIGN_LEFT, 0.2);
    static const char *const scrolls[] = { "2 finger", "Edge", "Button" };
    paint_segment(cr, bx1, gy + 58.0, bw, 30.0,
                  scrolls, 3, g_ui.segments[SEG_SCROLL], SEG_SCROLL);

    text_draw(cr, "Acceleration", 9.5, 500, C_DIM, 0.85,
              bx2, gy + 42.0, ALIGN_LEFT, 0.2);
    static const char *const accels[] = { "Adaptive", "Flat" };
    paint_segment(cr, bx2, gy + 58.0, bw, 30.0,
                  accels, 2, g_ui.segments[SEG_ACCEL], SEG_ACCEL);

    paint_toggle_row(cr, bx3, gy + 24.0, bw,
                     "Middle click",
                     "Press both buttons",
                     g_ui.toggles[TOG_MIDDLE], TOG_MIDDLE);

    text_wrap(cr,
        "Pointer, acceleration and keyboard repeat settings are saved to "
        "~/.config/jt/input.conf and applied by jtlabwc immediately. "
        "Layout and compose key are display-only for now.",
        8.8, 400, C_FAINT, 0.8, CONTENT_X, WIN_H - 156.0, CONTENT_W, 2);
}

/* ------------------------------------------------------------------ */
/*  Page: Displays                                                     */
/* ------------------------------------------------------------------ */

static void paint_display_preview(cairo_t *cr, double x, double y,
                                  double w, double h, int connected)
{
    rgba(cr, connected ? 0x111214 : 0x0C0C0D, 1.0);
    rounded_rect(cr, x, y, w, h, 6.0);
    cairo_fill(cr);
    rgba(cr, C_BORDER, connected ? 0.7 : 0.35);
    cairo_set_line_width(cr, 1.2);
    rounded_rect(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0, 5.5);
    cairo_stroke(cr);

    cairo_pattern_t *pg = cairo_pattern_create_linear(x, y, x + w, y + h);
    double a = connected ? 0.55 : 0.12;
    cairo_pattern_add_color_stop_rgba(pg, 0.0,
        R_(C_ACCENT), G_(C_ACCENT), B_(C_ACCENT), 0.18 * a);
    cairo_pattern_add_color_stop_rgba(pg, 0.55, 0.05, 0.06, 0.08, 0.9);
    cairo_pattern_add_color_stop_rgba(pg, 1.0, 0.02, 0.02, 0.03, 1.0);
    cairo_set_source(cr, pg);
    rounded_rect(cr, x + 8.0, y + 8.0, w - 16.0, h - 16.0, 3.0);
    cairo_fill(cr);
    cairo_pattern_destroy(pg);

    if (connected) {
        rgba(cr, C_ACCENT, 0.55);
        cairo_arc(cr, x + w * 0.72, y + h * 0.38, 13.0, 0.0, 2.0 * PI);
        cairo_fill(cr);
        rgba(cr, 0x0B0B0D, 0.55);
        rounded_rect(cr, x + 14.0, y + h - 34.0, w - 28.0, 18.0, 4.0);
        cairo_fill(cr);
        rgba(cr, C_ACCENT, 0.85);
        rounded_rect(cr, x + 18.0, y + h - 30.0, 22.0, 4.0, 2.0);
        cairo_fill(cr);
        rgba(cr, C_FG, 0.18);
        rounded_rect(cr, x + 46.0, y + h - 30.0, 34.0, 4.0, 2.0);
        cairo_fill(cr);
    } else {
        text_draw(cr, "NO SIGNAL", 8.0, 600, C_FAINT, 0.8,
                  x + w / 2.0, y + h / 2.0 - 5.0, ALIGN_CENTER, 1.8);
    }
}

static const char *head_description(const Head *h, char *buf, size_t n)
{
    if (h->description[0])
        return h->description;
    if (h->make[0] || h->model[0]) {
        snprintf(buf, n, "%s%s%s", h->make, h->make[0] && h->model[0] ? " " : "",
                 h->model);
        return buf;
    }
    return "Unknown display";
}

static void display_card(cairo_t *cr, int idx, double y)
{
    Head *h = &g_heads[idx];
    int connected = h->p_enabled;

    paint_card(cr, CONTENT_X, y, CONTENT_W, 206.0);
    paint_display_preview(cr, CONTENT_X + 18.0, y + 18.0, 196.0, 140.0,
                          connected);

    double rx = CONTENT_X + 240.0;
    char descbuf[192];
    const char *desc = head_description(h, descbuf, sizeof descbuf);

    text_draw(cr, h->name[0] ? h->name : "Output", 13.0, 600,
              connected ? C_FG : C_DIM, 0.96,
              rx, y + 16.0, ALIGN_LEFT, 0.1);
    text_wrap(cr, desc, 9.0, 400, C_DIM, 0.75,
              rx, y + 36.0, 500.0, 1);

    int32_t pw = h->cur_w, ph = h->cur_h;
    if (head_mode_valid(h)) {
        pw = h->modes[h->p_mode].width;
        ph = h->modes[h->p_mode].height;
    }

    text_draw(cr, "RESOLUTION", 7.8, 600, C_FAINT, 0.9,
              rx, y + 60.0, ALIGN_LEFT, 1.3);
    int ridx = add_hit(HIT_DISP_RES, idx, 0, rx, y + 76.0, 230.0, 30.0);
    char resbuf[48];
    if (pw > 0 && ph > 0)
        snprintf(resbuf, sizeof resbuf, "%d × %d", pw, ph);
    else
        snprintf(resbuf, sizeof resbuf, "—");
    paint_field(cr, rx, y + 76.0, 230.0, 30.0, resbuf, 1);
    if (hit_hover(ridx)) {
        rgba(cr, C_ACCENT, 0.55);
        cairo_set_line_width(cr, 1.0);
        rounded_rect(cr, rx + 0.5, y + 76.5, 229.0, 29.0, 6.5);
        cairo_stroke(cr);
    }

    text_draw(cr, "REFRESH RATE", 7.8, 600, C_FAINT, 0.9,
              rx + 254.0, y + 60.0, ALIGN_LEFT, 1.3);
    int32_t refreshes[8];
    int rn = head_refreshes(h, pw, ph, refreshes, 8);
    if (rn > 4)
        rn = 4;
    if (rn > 0) {
        char rbuf[4][24];
        const char *ritems[4];
        int rsel = 0;
        int32_t chosen = head_mode_valid(h) ? h->modes[h->p_mode].refresh
                                            : h->cur_refresh;
        for (int i = 0; i < rn; i++) {
            double hz = refreshes[i] / 1000.0;
            snprintf(rbuf[i], sizeof rbuf[i], "%.0f Hz", hz);
            ritems[i] = rbuf[i];
            if (refreshes[i] == chosen)
                rsel = i;
        }
        paint_segment_ex(cr, rx + 254.0, y + 76.0, 246.0, 30.0,
                         ritems, rn, rsel, HIT_DISP_REFRESH, idx);
    } else {
        paint_field(cr, rx + 254.0, y + 76.0, 246.0, 30.0, "—", 0);
    }

    text_draw(cr, "ORIENTATION", 7.8, 600, C_FAINT, 0.9,
              rx, y + 118.0, ALIGN_LEFT, 1.3);
    static const char *const orient[] = { "Landscape", "Portrait" };
    paint_segment_ex(cr, rx, y + 134.0, 230.0, 30.0,
                     orient, 2, head_orient_index(h),
                     HIT_DISP_ORIENT, idx);

    text_draw(cr, "SCALE", 7.8, 600, C_FAINT, 0.9,
              rx + 254.0, y + 118.0, ALIGN_LEFT, 1.3);
    paint_segment_ex(cr, rx + 254.0, y + 134.0, 246.0, 30.0,
                     DISP_SCALE_LABELS, DISP_SCALE_COUNT,
                     head_scale_index(h), HIT_DISP_SCALE, idx);

    text_draw(cr, "Enabled", 10.0, 500, connected ? 0xD2D2D2 : C_FAINT,
              0.9, rx, y + 176.0, ALIGN_LEFT, 0.1);
    paint_mini_toggle_ex(cr, rx + 58.0, y + 174.0, h->p_enabled,
                         HIT_DISP_ENABLE, idx);

    paint_button_ex(cr, rx + 380.0, y + 170.0, 110.0, 30.0,
                    head_is_primary(idx) ? "Primary" : "Make primary",
                    head_is_primary(idx), HIT_DISP_PRIMARY, idx, 0);

    char status[96];
    if (connected) {
        if (pw > 0 && ph > 0 && head_mode_valid(h))
            snprintf(status, sizeof status, "Connected · %d×%d @ %.0f Hz",
                     pw, ph, h->modes[h->p_mode].refresh / 1000.0);
        else
            snprintf(status, sizeof status, "Connected");
    } else {
        snprintf(status, sizeof status, "Disabled");
    }
    text_wrap(cr, status, 8.5, 400, connected ? C_GREEN : C_FAINT,
              connected ? 0.8 : 0.7, CONTENT_X + 18.0, y + 172.0, 196.0, 1);
}

static void page_displays(cairo_t *cr)
{
    section_label(cr, "OUTPUTS", CONTENT_X, 102.0);

    int n = head_count();
    int active = head_enabled_count();
    double btn_y = DISPLAY_BTN_Y;
    double view_top = DISPLAY_VIEW_TOP;
    double view_bottom = DISPLAY_VIEW_BOTTOM;
    int max_scroll = (int)(n * 220.0 - (view_bottom - view_top));
    if (max_scroll < 0)
        max_scroll = 0;
    if (g_ui.disp_scroll > max_scroll)
        g_ui.disp_scroll = max_scroll;
    if (g_ui.disp_scroll < 0)
        g_ui.disp_scroll = 0;

    char summary[96];
    if (n > 0)
        snprintf(summary, sizeof summary, "%d output%s · %d active",
                 n, n == 1 ? "" : "s", active);
    else
        snprintf(summary, sizeof summary, "no outputs detected");
    text_draw(cr, summary, 9.5, 400, C_DIM, 0.75,
              WIN_W - 36.0, 102.0, ALIGN_RIGHT, 0.2);

    if (n == 0) {
        paint_card(cr, CONTENT_X, 122.0, CONTENT_W, 206.0);
        text_draw(cr, g_output_manager_available
                       ? "No outputs detected"
                       : "Output management unavailable",
                  15.0, 600, C_FG, 0.95,
                  CONTENT_X + 24.0, 156.0, ALIGN_LEFT, 0.1);
        text_wrap(cr,
            g_output_manager_available
                ? "jtlabwc is not advertising any outputs. Check the "
                  "connection and make sure the compositor is running."
                : "jtlabwc did not advertise wlr-output-management. "
                  "Display detection needs a compositor that supports it.",
            9.5, 400, C_DIM, 0.85,
            CONTENT_X + 24.0, 186.0, CONTENT_W - 48.0, 2);
    } else {
        cairo_save(cr);
        cairo_rectangle(cr, CONTENT_X, view_top, CONTENT_W,
                            view_bottom - view_top);
        cairo_clip(cr);

        int ord = 0;
        for (int i = 0; i < MAX_HEADS; i++) {
            if (!g_heads[i].used)
                continue;
            double y = 122.0 + ord * 220.0 - g_ui.disp_scroll;
            ord++;
            if (y + 206.0 < view_top || y > view_bottom)
                continue;
            display_card(cr, i, y);
        }

        cairo_restore(cr);
    }

    paint_button(cr, CONTENT_X, btn_y, 140.0, 36.0,
                 "Arrange", 0, BTN_POSITION);
    paint_button(cr, CONTENT_X + 152.0, btn_y, 130.0, 36.0,
                 "Apply", 1, BTN_DISPLAYS_APPLY);
    paint_button(cr, CONTENT_X + 294.0, btn_y, 110.0, 36.0,
                 "Reset", 0, BTN_DISPLAYS_RESET);

    text_wrap(cr,
        "Changes are sent to jtlabwc through wlr-output-management and "
        "applied atomically. At least one display must remain enabled. "
        "Scroll to reach additional outputs.",
        8.8, 400, C_FAINT, 0.8, CONTENT_X, btn_y + 44.0, CONTENT_W, 2);
}


/*  Page: Session                                                      */
/* ------------------------------------------------------------------ */

static void page_session(cairo_t *cr)
{
    double col_w = (CONTENT_W - 20.0) / 2.0;
    double col2 = CONTENT_X + col_w + 20.0;
    double inner = col_w - 36.0;
    double row_w = col_w - 20.0;

    section_label(cr, "IDLE & LOCK", CONTENT_X, 102.0);
    section_label(cr, "POWER COMMANDS", col2, 102.0);

    char v[32];
    double cy = 122.0;
    double by = WIN_H - 344.0;
    double ch = by - 22.0 - cy;

    paint_card(cr, CONTENT_X, cy, col_w, ch);
    double lx = CONTENT_X + 18.0;
    text_draw(cr, "Away from the keyboard", 12.0, 600, 0xE8E8E8, 0.95,
              lx, cy + 14.0, ALIGN_LEFT, 0.1);

    paint_slider_row(cr, lx, cy + 44.0, inner, "Idle timeout",
                     slider_text(SL_IDLE, g_ui.sliders[SL_IDLE], v, sizeof v),
                     g_ui.sliders[SL_IDLE], SL_IDLE);
    paint_divider(cr, lx, cy + 88.0, inner);

    paint_toggle_row(cr, lx, cy + 98.0, inner, "Lock when idle",
                     "Require a password after the timeout",
                     g_ui.toggles[TOG_IDLE_LOCK], TOG_IDLE_LOCK);
    paint_toggle_row(cr, lx, cy + 140.0, inner, "Dim before locking",
                     "Fade the screen one minute early",
                     g_ui.toggles[TOG_DIM], TOG_DIM);
    paint_toggle_row(cr, lx, cy + 182.0, inner, "Lock on suspend",
                     "Never wake straight into the desktop",
                     g_ui.toggles[TOG_LOCK_SUSPEND], TOG_LOCK_SUSPEND);

    paint_card(cr, col2, cy, col_w, ch);
    text_draw(cr, "End the session", 12.0, 600, 0xE8E8E8, 0.95,
              col2 + 18.0, cy + 14.0, ALIGN_LEFT, 0.1);

    static const struct {
        const char *label;
        const char *desc;
        const char *key1;
        const char *key2;
        int button;
    } commands[] = {
        { "Lock screen", "jtlockscreen", "Super", "L", BTN_LOGOUT },
        { "Log out", "End the jtlabwc session", "Super", "Shift+E", BTN_LOGOUT },
        { "Reboot", "Restart the machine", "", "", BTN_REBOOT },
        { "Shut down", "Power off safely", "", "", BTN_SHUTDOWN },
    };

    for (int i = 0; i < 4; i++) {
        double ry = cy + 42.0 + i * 44.0;
        int idx = add_hit(HIT_BUTTON, commands[i].button, 0,
                          col2 + 10.0, ry, row_w, 40.0);
        int hot = hit_hover(idx);

        if (hot) {
            rgba(cr, C_FG, 0.04);
            rounded_rect(cr, col2 + 10.0, ry, row_w, 40.0, 7.0);
            cairo_fill(cr);
        }

        rgba(cr, i >= 2 ? C_RED : C_ACCENT, i >= 2 ? 0.75 : 0.85);
        cairo_arc(cr, col2 + 28.0, ry + 20.0, 2.6, 0.0, 2.0 * PI);
        cairo_fill(cr);

        text_draw(cr, commands[i].label, 11.0, 600, 0xDCDCDC, 0.95,
                  col2 + 42.0, ry + 6.0, ALIGN_LEFT, 0.1);
        text_draw(cr, commands[i].desc, 8.5, 400, C_DIM, 0.72,
                  col2 + 42.0, ry + 23.0, ALIGN_LEFT, 0.1);

        if (commands[i].key1[0]) {
            double kw = keycap_width(cr, commands[i].key1) + 5.0 +
                        keycap_width(cr, commands[i].key2);
            double kx = col2 + row_w - kw;
            kx += paint_keycap(cr, commands[i].key1, kx, ry + 10.0) + 5.0;
            paint_keycap(cr, commands[i].key2, kx, ry + 10.0);
        }
    }

    paint_card(cr, CONTENT_X, by, CONTENT_W, 196.0);
    text_draw(cr, "Session", 12.0, 600, 0xE8E8E8, 0.95,
              CONTENT_X + 18.0, by + 16.0, ALIGN_LEFT, 0.1);

    paint_toggle_row(cr, CONTENT_X + 18.0, by + 48.0, inner,
                     "Restore previous windows",
                     "Reopen apps from the last session",
                     g_ui.toggles[TOG_RESTORE], TOG_RESTORE);
    paint_toggle_row(cr, CONTENT_X + 18.0, by + 90.0, inner,
                     "Start on last workspace",
                     "Remember the active desktop",
                     g_ui.toggles[TOG_LAST_WS], TOG_LAST_WS);

    double rx2 = col2;
    text_draw(cr, "Startup workspace", 9.0, 500, C_DIM, 0.85,
              rx2, by + 46.0, ALIGN_LEFT, 0.2);
    paint_field(cr, rx2, by + 62.0, inner, 32.0, "Workspace 1", 1);

    text_draw(cr, "Environment file", 9.0, 500, C_DIM, 0.85,
              rx2, by + 106.0, ALIGN_LEFT, 0.2);
    paint_field(cr, rx2, by + 122.0, inner, 32.0,
                "~/.config/labwc/environment", 0);

    text_wrap(cr,
        "Power commands are drawn as real rows, but pressing one only shows "
        "a preview message. Nothing is executed.",
        8.8, 400, C_FAINT, 0.8, CONTENT_X, WIN_H - 128.0, CONTENT_W, 2);
}

static double
photo_zoom_value(void)
{
    double t = clampd(g_ui.sliders[SL_PHOTO_ZOOM], 0.0, 1.0);
    return PROFILE_ZOOM_MIN + t * (PROFILE_ZOOM_MAX - PROFILE_ZOOM_MIN);
}

/* Centre-cropped circular preview, using the same geometry JTLab uses for
 * the bar avatar so what you see here is what the bar will draw. */
static void
paint_photo_circle(cairo_t *cr, cairo_surface_t *sf,
                   double cx, double cy, double r, double zoom)
{
    rgba(cr, C_TRACK, 0.9);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, r, 0.0, 2.0 * PI);
    cairo_fill(cr);

    if (sf) {
        int iw = cairo_image_surface_get_width(sf);
        int ih = cairo_image_surface_get_height(sf);
        if (iw > 0 && ih > 0) {
            double mn = iw < ih ? iw : ih;
            if (zoom < PROFILE_ZOOM_MIN) zoom = PROFILE_ZOOM_MIN;
            if (zoom > PROFILE_ZOOM_MAX) zoom = PROFILE_ZOOM_MAX;
            double centre = mn / (2.0 * zoom);
            cairo_save(cr);
            cairo_arc(cr, cx, cy, r, 0.0, 2.0 * PI);
            cairo_clip(cr);
            cairo_translate(cr, cx - r, cy - r);
            cairo_scale(cr, 2.0 * r * zoom / mn, 2.0 * r * zoom / mn);
            cairo_set_source_surface(cr, sf,
                                     centre - iw / 2.0,
                                     centre - ih / 2.0);
            cairo_paint(cr);
            cairo_restore(cr);
        }
    } else {
        rgba(cr, C_FAINT, 0.55);
        cairo_new_path(cr);
        cairo_arc(cr, cx, cy - r * 0.22, r * 0.28, 0.0, 2.0 * PI);
        cairo_fill(cr);
        cairo_new_path(cr);
        cairo_arc(cr, cx, cy + r * 0.85, r * 0.62, PI, 2.0 * PI);
        cairo_fill(cr);
    }

    rgba(cr, C_BORDER, 0.85);
    cairo_set_line_width(cr, 1.0);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, r - 0.5, 0.0, 2.0 * PI);
    cairo_stroke(cr);
}

static void
paint_field_box(cairo_t *cr, double x, double y, double w, double h,
                const char *text, int editing, int hot)
{
    rgba(cr, C_TRACK, 0.9);
    rounded_rect(cr, x, y, w, h, 7.0);
    cairo_fill(cr);
    rgba(cr, hot ? C_ACCENT : C_BORDER, hot ? 0.75 : 0.5);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0, 6.5);
    cairo_stroke(cr);

    cairo_save(cr);
    cairo_rectangle(cr, x + 1.0, y + 1.0, w - 2.0, h - 2.0);
    cairo_clip(cr);
    text_draw(cr, text, 10.5, 500, editing ? C_FG : 0xD6D6D6, 0.94,
              x + 12.0, y + (h - 13.0) / 2.0, ALIGN_LEFT, 0.1);
    if (editing && (int)(g_ui.t * 2.0) % 2 == 0) {
        double cx = x + 12.0 +
                    text_width(cr, text, 10.5, 500, 0.1) + 2.0;
        rgba(cr, C_ACCENT, 0.9);
        cairo_set_line_width(cr, 1.2);
        cairo_move_to(cr, cx, y + 7.0);
        cairo_line_to(cr, cx, y + h - 7.0);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

/* ------------------------------------------------------------------ */
/*  Page: Profile                                                      */
/* ------------------------------------------------------------------ */

static void
page_profile(cairo_t *cr)
{
    section_label(cr, "WEATHER", CONTENT_X, 102.0);
    text_draw(cr, "Location and temperature unit shown in JTLab's status bar",
              9.5, 400, C_DIM, 0.75, CONTENT_X, 117.0, ALIGN_LEFT, 0.1);

    double cy = 132.0, ch = 250.0;
    paint_card(cr, CONTENT_X, cy, CONTENT_W, ch);

    double inner = CONTENT_W - 36.0;
    double lx = CONTENT_X + 18.0;
    double lw = inner * 0.60;
    double rx = lx + lw + 20.0;
    double rw = inner - lw - 20.0;

    text_draw(cr, "LOCATION", 8.0, 600, C_FAINT, 0.9,
              lx, cy + 14.0, ALIGN_LEFT, 1.4);
    int loc_hit = add_hit(HIT_WX_FIELD, 0, 0, lx, cy + 27.0, lw, 34.0);
    paint_field_box(cr, lx, cy + 27.0, lw, 34.0,
                    g_ui.wx_editing ? g_ui.wx_location_input
                                    : g_profile_cfg.location,
                    g_ui.wx_editing, hit_hover(loc_hit));

    text_draw(cr, "FIND A PLACE", 8.0, 600, C_FAINT, 0.9,
              lx, cy + 74.0, ALIGN_LEFT, 1.4);
    double qw = lw - 104.0;
    int q_hit = add_hit(HIT_WX_QUERY_FIELD, 0, 0, lx, cy + 87.0, qw, 32.0);
    paint_field_box(cr, lx, cy + 87.0, qw, 32.0,
                    g_ui.wx_query_input[0] ? g_ui.wx_query_input : "e.g. Cape Town",
                    g_ui.wx_query_editing, hit_hover(q_hit));
    paint_button(cr, lx + lw - 96.0, cy + 87.0, 96.0, 32.0,
                 g_wx_searching ? "Searching…" : "Search", 1,
                 BTN_WX_SEARCH);

    double row_y = cy + 126.0;
    if (g_wx_result_count > 0) {
        for (int i = 0; i < g_wx_result_count; i++) {
            double ry = row_y + i * 19.0;
            int idx = add_hit(HIT_WX_RESULT, i, 0, lx - 4.0, ry - 2.0,
                              lw + 8.0, 19.0);
            int hot = hit_hover(idx);
            if (hot) {
                rgba(cr, C_FG, 0.05);
                rounded_rect(cr, lx - 4.0, ry - 2.0, lw + 8.0, 19.0, 5.0);
                cairo_fill(cr);
            }
            rgba(cr, hot ? C_ACCENT : C_FAINT, 0.85);
            cairo_arc(cr, lx + 5.0, ry + 8.0, 2.0, 0.0, 2.0 * PI);
            cairo_fill(cr);
            text_draw(cr, g_wx_results[i].name, 9.5, 500,
                      hot ? C_FG : 0xD0D0D0, 0.92,
                      lx + 14.0, ry + 2.0, ALIGN_LEFT, 0.1);
        }
    } else if (!g_wx_searching) {
        text_wrap(cr,
                  "Type a city and press Search, or click the location field "
                  "and type a place name directly. Press Enter to save.",
                  9.0, 400, C_FAINT, 0.8, lx, row_y + 2.0, lw, 3);
    } else {
        text_draw(cr, "Looking up places…", 9.5, 400, C_DIM, 0.8,
                  lx, row_y + 3.0, ALIGN_LEFT, 0.1);
    }

    text_draw(cr, "TEMPERATURE UNIT", 8.0, 600, C_FAINT, 0.9,
              rx, cy + 14.0, ALIGN_LEFT, 1.4);
    static const char *const units[] = { "Celsius", "Fahrenheit" };
    paint_segment(cr, rx, cy + 28.0, rw, 30.0, units, 2,
                  g_ui.segments[SEG_WX_UNITS], SEG_WX_UNITS);
    text_wrap(cr,
              "Open-Meteo readings are converted when drawn, so switching "
              "units never needs a new fetch.",
              9.0, 400, C_DIM, 0.75, rx, cy + 78.0, rw, 3);
    paint_divider(cr, rx, cy + 146.0, rw);
    text_draw(cr, "DEVICE STATE", 8.0, 600, C_FAINT, 0.9,
              rx, cy + 160.0, ALIGN_LEFT, 1.4);
    char coords[96];
    if (g_profile_cfg.have_coords)
        snprintf(coords, sizeof coords, "%.3f, %.3f",
                 g_profile_cfg.lat, g_profile_cfg.lon);
    else
        snprintf(coords, sizeof coords, "Resolved by place name");
    text_draw(cr, coords, 10.0, 500, C_ACCENT, 0.92,
              rx, cy + 180.0, ALIGN_LEFT, 0.15);
    text_draw(cr, "~/.config/jt/profile.config", 8.5, 400, C_DIM, 0.75,
              rx, cy + 207.0, ALIGN_LEFT, 0.1);

    section_label(cr, "PROFILE PHOTO", CONTENT_X, 402.0);
    text_draw(cr, "The avatar drawn above the power menu in JTLab",
              9.5, 400, C_DIM, 0.75, CONTENT_X, 417.0, ALIGN_LEFT, 0.1);

    double py = 432.0, ph = 200.0;
    paint_card(cr, CONTENT_X, py, CONTENT_W, ph);
    double ax = CONTENT_X + 76.0;
    double ay = py + 92.0;
    paint_photo_circle(cr, g_photo_preview, ax, ay, 48.0, photo_zoom_value());

    double px = CONTENT_X + 152.0;
    double pw = CONTENT_W - 170.0;
    text_draw(cr, "JTLab avatar", 12.0, 600, 0xE8E8E8, 0.95,
              px, py + 16.0, ALIGN_LEFT, 0.1);
    text_draw(cr, "Choose an image with the file portal, then set its zoom.",
              9.0, 400, C_DIM, 0.78, px, py + 34.0, ALIGN_LEFT, 0.1);

    paint_button(cr, px, py + 54.0, 140.0, 32.0,
                 "Choose picture", 1, BTN_PHOTO_PICK);
    paint_button(cr, px + 150.0, py + 54.0, 96.0, 32.0,
                 "Remove", 0, BTN_PHOTO_CLEAR);

    char ztxt[32];
    snprintf(ztxt, sizeof ztxt, "%.2fx", photo_zoom_value());
    paint_slider_row(cr, px, py + 108.0, pw, "Zoom", ztxt,
                     g_ui.sliders[SL_PHOTO_ZOOM], SL_PHOTO_ZOOM);

    char shown[PROFILE_PATH_MAX + 16];
    if (g_profile_cfg.photo[0])
        snprintf(shown, sizeof shown, "Image: %s", g_profile_cfg.photo);
    else
        snprintf(shown, sizeof shown, "Image: using the built-in avatar");
    text_wrap(cr, shown, 8.5, 400, C_FAINT, 0.8,
              px, py + 168.0, pw, 1);

    text_wrap(cr,
              "The location, unit, picture and zoom are written to "
              "~/.config/jt/profile.config and applied to JTLab immediately.",
              8.8, 400, C_FAINT, 0.8, CONTENT_X, 650.0, CONTENT_W, 2);
}

/* A small confirmation step that appears right after the portal returns an
 * image: preview the centre crop and drag the slider before saving. */
static void
zoom_overlay_rect(double *x, double *y, double *w, double *h)
{
    *w = 440.0;
    *h = 372.0;
    *x = (WIN_W - *w) / 2.0;
    *y = (WIN_H - *h) / 2.0;
}

static void
paint_zoom_overlay(cairo_t *cr)
{
    rgba(cr, 0x000000, 0.74);
    cairo_rectangle(cr, 0, 0, WIN_W, WIN_H);
    cairo_fill(cr);
    add_hit(HIT_ZOOM_SCRIM, 0, 0, 0, 0, WIN_W, WIN_H);

    double x, y, w, h;
    zoom_overlay_rect(&x, &y, &w, &h);
    paint_card(cr, x, y, w, h);
    /* Swallow clicks on the dialog itself so only the dimmed area cancels. */
    add_hit(HIT_NONE, 0, 0, x, y, w, h);

    text_draw(cr, "Adjust profile photo", 16.0, 700, C_FG, 0.98,
              x + w / 2.0, y + 20.0, ALIGN_CENTER, 0.1);
    text_draw(cr, "Drag the slider to frame your picture",
              9.0, 400, C_DIM, 0.8, x + w / 2.0, y + 42.0,
              ALIGN_CENTER, 0.1);

    paint_photo_circle(cr, g_photo_preview, x + w / 2.0, y + 136.0,
                       68.0, photo_zoom_value());

    char ztxt[32];
    snprintf(ztxt, sizeof ztxt, "%.2fx", photo_zoom_value());
    paint_slider_row(cr, x + 34.0, y + 242.0, w - 68.0, "Zoom", ztxt,
                     g_ui.sliders[SL_PHOTO_ZOOM], SL_PHOTO_ZOOM);

    paint_button(cr, x + 34.0, y + 310.0, 120.0, 36.0,
                 "Cancel", 0, BTN_ZOOM_CANCEL);
    paint_button(cr, x + w - 154.0, y + 310.0, 120.0, 36.0,
                 "Use photo", 1, BTN_ZOOM_SAVE);

    text_draw(cr, "1.0x shows the whole image · zoom in to crop",
              8.5, 400, C_FAINT, 0.8, x + w / 2.0, y + 352.0,
              ALIGN_CENTER, 0.1);
}

/* Leaving the preview without saving restores the page's saved zoom so the
 * avatar preview below keeps matching what JTLab is drawing. */
static void
zoom_cancel(void)
{
    g_zoom_open = 0;
    g_pending_photo[0] = '\0';
    if (g_photo_preview) {
        cairo_surface_destroy(g_photo_preview);
        g_photo_preview = NULL;
        if (g_profile_cfg.photo[0])
            profile_preview_load(g_profile_cfg.photo);
    }
    g_ui.sliders[SL_PHOTO_ZOOM] =
        (g_profile_cfg.zoom - PROFILE_ZOOM_MIN) /
        (PROFILE_ZOOM_MAX - PROFILE_ZOOM_MIN);
}

static void
zoom_accept(void)
{
    if (!g_pending_photo[0])
        return;
    snprintf(g_profile_cfg.photo, sizeof g_profile_cfg.photo,
             "%s", g_pending_photo);
    g_profile_cfg.zoom = photo_zoom_value();
    g_pending_photo[0] = '\0';
    g_zoom_open = 0;
    profile_save();
    ui_toast("Profile photo saved");
}

/* ------------------------------------------------------------------ */
/*  Page: About                                                        */
/* ------------------------------------------------------------------ */

static void page_about(cairo_t *cr)
{
    /* Center on the content column, not the whole window: the sidebar is
     * outside this column and was pulling these elements left. */
    double cx = CONTENT_X + CONTENT_W / 2.0;

    rgba(cr, C_ACCENT, 0.10);
    cairo_arc(cr, cx, 150.0, 54.0, 0.0, 2.0 * PI);
    cairo_fill(cr);
    rgba(cr, C_ACCENT, 0.95);
    rounded_rect(cr, cx - 30.0, 120.0, 60.0, 60.0, 16.0);
    cairo_fill(cr);
    rgba(cr, C_ON_ACCENT, 0.92);
    for (int i = 0; i < 4; i++) {
        double h = 13.0 + i * 7.0;
        rounded_rect(cr, cx - 19.0 + i * 10.0, 150.0 - h / 2.0,
                     5.0, h, 2.5);
        cairo_fill(cr);
    }

    text_draw(cr, "JTLab Settings", 27.0, 700, C_FG, 0.98,
              cx, 192.0, ALIGN_CENTER, 0.0);
    text_draw(cr, "A calm place to shape your desktop", 11.0, 400,
              C_DIM, 0.82, cx, 231.0, ALIGN_CENTER, 0.3);

    double pw = 164.0;
    rgba(cr, C_ACCENT, 0.12);
    rounded_rect(cr, cx - pw / 2.0, 256.0, pw, 26.0, 13.0);
    cairo_fill(cr);
    rgba(cr, C_ACCENT, 0.5);
    cairo_set_line_width(cr, 1.0);
    rounded_rect(cr, cx - pw / 2.0 + 0.5, 256.5, pw - 1.0, 25.0, 12.5);
    cairo_stroke(cr);
    text_draw(cr, "0.1.0  ·  preview build", 9.0, 600, C_ACCENT, 0.95,
              cx, 263.0, ALIGN_CENTER, 0.6);

    double gx = CONTENT_X, gy = 304.0, gw = CONTENT_W;
    double btn_y = WIN_H - 172.0;
    double gh = (btn_y - 30.0) - gy;
    paint_card(cr, gx, gy, gw, gh);

    static const char *const keys[] = {
        "Compositor", "Toolkit", "Palette", "Interface font",
        "Control socket", "Configuration",
    };
    static const char *const vals[] = {
        "jtlabwc · Wayland", "cairo + Pango · no GTK",
        "#030303 · amber #E8A03C", "Liberation Sans · 10 pt",
        "$XDG_RUNTIME_DIR/jtsettings-ctl.sock", "~/jt",
    };

    for (int i = 0; i < 6; i++) {
        int col = i % 2;
        int row = i / 2;
        double kx = gx + 24.0 + col * (gw / 2.0);
        double ky = gy + 24.0 + row * 62.0;

        text_draw(cr, keys[i], 8.0, 600, C_FAINT, 0.9,
                  kx, ky, ALIGN_LEFT, 1.4);
        text_draw(cr, vals[i], 10.5, 500, 0xDCDCDC, 0.92,
                  kx, ky + 18.0, ALIGN_LEFT, 0.1);

        if (col == 0 && i + 2 < 6)
            paint_divider(cr, gx + 24.0, ky + 48.0, gw - 48.0);
    }

    paint_button(cr, cx - 182.0, btn_y, 170.0, 40.0,
                 "Documentation", 0, BTN_DOCS);
    paint_button(cr, cx + 12.0, btn_y, 170.0, 40.0,
                 "Check for updates", 1, BTN_UPDATE);

    text_draw(cr, "Built for the JTLab desktop · made with care",
              9.0, 400, C_FAINT, 0.8, cx, WIN_H - 106.0,
              ALIGN_CENTER, 0.3);
    text_draw(cr, "Source: ~/jtsettings/jtsettings.c",
              8.5, 400, C_FAINT, 0.7, cx, WIN_H - 86.0,
              ALIGN_CENTER, 0.2);
}

/* ------------------------------------------------------------------ */
/*  Frame                                                              */
/* ------------------------------------------------------------------ */

static void paint_content(cairo_t *cr)
{
    g_hit_count = 0;

    cairo_pattern_t *bg = cairo_pattern_create_linear(0.0, 0.0, 0.0, WIN_H);
    cairo_pattern_add_color_stop_rgba(bg, 0.0,
        R_(C_BG_TOP), G_(C_BG_TOP), B_(C_BG_TOP), 1.0);
    cairo_pattern_add_color_stop_rgba(bg, 1.0,
        R_(C_BG), G_(C_BG), B_(C_BG), 1.0);
    cairo_set_source(cr, bg);
    cairo_paint(cr);
    cairo_pattern_destroy(bg);

    glow_ensure();
    glow_paint(cr, g_ambient_glow,
               CONTENT_X + CONTENT_W * 0.62, 120.0, 520.0);

    paint_sidebar(cr, g_ui.t);
    paint_header(cr);

    switch (g_ui.page) {
    case PAGE_KEYBINDINGS: page_keybindings(cr); break;
    case PAGE_STARTUP: page_startup(cr); break;
    case PAGE_DEFAULT_APPS: page_defaultapps(cr); break;
    case PAGE_RULES: page_rules(cr); break;
    case PAGE_INPUT: page_input(cr); break;
    case PAGE_DISPLAYS: page_displays(cr); break;
    case PAGE_SESSION: page_session(cr); break;
    case PAGE_PROFILE: page_profile(cr); break;
    case PAGE_ABOUT: page_about(cr); break;
    default: break;
    }

    paint_footer(cr);
    paint_position_editor(cr);
    paint_mode_picker(cr);
    paint_app_picker(cr);

    /* Painted last so its hits sit above the page's hits in hit_at(). */
    if (g_zoom_open)
        paint_zoom_overlay(cr);
}

/* ------------------------------------------------------------------ */
/*  Wayland backend                                                    */
/* ------------------------------------------------------------------ */

#define NBUFS 4
#define MAX_OUTPUTS 8
#define BTN_LEFT 0x110

typedef struct App App;

typedef struct {
    struct wl_buffer *buffer;
    void *data;
    size_t size;
    int width, height;
    int busy;
    int retired;
} ShmBuf;

typedef struct {
    struct wl_output *output;
    uint32_t registry_name;
    int32_t scale;
    int used;
    int entered;
    App *app;
} Output;

/* Companion windows we can raise through zwlr_foreign_toplevel_manager_v1,
 * ported from JTLab's focus_app_window() helper. */
#define MAX_FOREIGN_TOPLEVELS 128

typedef struct {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    char *app_id;
} ForeignToplevel;

static ForeignToplevel g_toplevels[MAX_FOREIGN_TOPLEVELS];
static int g_toplevel_count;

struct App {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct zxdg_decoration_manager_v1 *deco_manager;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    uint32_t pointer_serial;
    struct wl_keyboard *keyboard;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct zxdg_toplevel_decoration_v1 *decoration;
    struct xdg_toplevel_icon_manager_v1 *icon_manager;
    struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
    struct wp_cursor_shape_device_v1 *cursor_shape_device;
    uint32_t cursor_shape;

    int configured;
    int dirty;

    /* One outstanding wl_surface.frame callback.  Painting is paced by the
     * compositor: we draw at most once per refresh and request no callbacks
     * while the UI is idle. */
    struct wl_callback *frame_cb;

    /* Window geometry remembered across runs.  Wayland leaves placement to
     * the compositor, so only size and maximized state are persisted. */
    int state_w;
    int state_h;
    int state_maximized;

    int scale;
    int fixed_scale;
    int has_argb;
    ShmBuf bufs[NBUFS];

    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;

    Output outputs[MAX_OUTPUTS];
    int output_count;

    struct zwlr_output_manager_v1 *output_manager;
    struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager;
    uint32_t output_serial;
    struct zwlr_output_configuration_v1 *pending_config;

    int quit;
    double t0;

    int ctl_fd;
    int ctl_client_fd;
    /* Filesystem identity of our bound socket path.  fstat() on an AF_UNIX
     * fd returns a sockfs inode on Linux, so it cannot be compared with the
     * path's lstat() result. */
    dev_t ctl_dev;
    ino_t ctl_ino;
    char ctl_buf[256];
    size_t ctl_len;
};

static void defaultapps_mark_dirty(void)
{
    if (g_app)
        g_app->dirty = 1;
}

/* Hits and several UI fields store a g_heads[] slot, not a stable output ID.
 * When a head goes away, invalidate every reference before the slot can be
 * reused by a different monitor. */
static void head_ui_invalidate_slot(int slot)
{
    if (slot < 0 || slot >= MAX_HEADS)
        return;

    if (g_ui.mode_picker_head == slot) {
        g_ui.mode_picker_open = 0;
        g_ui.mode_picker_head = -1;
    }
    if (g_ui.disp_drag == slot) {
        g_ui.disp_drag = -1;
        g_ui.disp_drag_ox = 0.0;
        g_ui.disp_drag_oy = 0.0;
    }
    g_ui.pos_layout_valid = 0;
    g_ui.hover = -1;
    g_ui.pressed = -1;
    g_hit_count = 0;
    if (g_app)
        g_app->dirty = 1;
}

/* Clear head state only; callers remain responsible for destroying any live
 * Wayland objects (head_finished handles its own, app_cleanup handles the
 * leftover objects on shutdown). */
static void heads_clear(void)
{
    for (int i = 0; i < MAX_HEADS; i++) {
        head_ui_invalidate_slot(i);
        memset(&g_heads[i], 0, sizeof g_heads[i]);
    }
}

/* Raise the first open window whose app_id matches, as JTLab does. */
static int
focus_app_window(App *a, const char *app_id)
{
    if (!a || !a->seat || !app_id)
        return 0;
    for (int i = 0; i < g_toplevel_count; i++) {
        const char *id = g_toplevels[i].app_id;
        if (id && g_toplevels[i].handle && !strcasecmp(id, app_id)) {
            zwlr_foreign_toplevel_handle_v1_activate(
                g_toplevels[i].handle, a->seat);
            return 1;
        }
    }
    return 0;
}

static void
launch_jtupdate(void)
{
    char cmd[256];
    snprintf(cmd, sizeof cmd,
             "jtupdate '#%06X' >/dev/null 2>&1 &", accent_rgb & 0xFFFFFF);
    if (system(cmd) != 0) { }
}

/* ------------------------------------------------------------------ */
/*  Remembered window state                                            */
/* ------------------------------------------------------------------ */

#define STATE_APP "jtsettings"

static void
state_path(char *out, size_t n)
{
    const char *base = getenv("XDG_STATE_HOME");
    if (base && *base) {
        snprintf(out, n, "%s/jt/%s.conf", base, STATE_APP);
        return;
    }
    const char *home = getenv("HOME");
    if (!home || !*home)
        home = ".";
    snprintf(out, n, "%s/.local/state/jt/%s.conf", home, STATE_APP);
}

static void
state_mkdir_p(const char *path)
{
    char tmp[512];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof tmp)
        return;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        mkdir(tmp, 0755);
        *p = '/';
    }
    mkdir(tmp, 0755);
}

static void
state_load(App *a)
{
    a->state_w = (int)DESIGN_W;
    a->state_h = (int)DESIGN_H;
    a->state_maximized = 0;

    char path[512];
    state_path(path, sizeof path);

    FILE *f = fopen(path, "r");
    if (!f)
        return;

    char line[256];
    while (fgets(line, sizeof line, f)) {
        char key[64];
        int value;
        if (sscanf(line, " %63[^= \t] = %d", key, &value) != 2)
            continue;
        if (!strcmp(key, "width") && value > 0)
            a->state_w = value;
        else if (!strcmp(key, "height") && value > 0)
            a->state_h = value;
        else if (!strcmp(key, "maximized"))
            a->state_maximized = value != 0;
    }
    fclose(f);
}

static void
state_save(App *a)
{
    if (a->state_w <= 0 || a->state_h <= 0)
        return;

    char path[512];
    state_path(path, sizeof path);

    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        state_mkdir_p(dir);
    }

    FILE *f = fopen(path, "w");
    if (!f)
        return;

    fprintf(f, "width=%d\nheight=%d\nmaximized=%d\n",
            a->state_w, a->state_h, a->state_maximized);
    fclose(f);
}

static int create_anon_file(size_t size)
{
    int fd = memfd_create("jtsettings", MFD_CLOEXEC);
    if (fd >= 0) {
        if (ftruncate(fd, (off_t)size) == 0)
            return fd;
        close(fd);
    }

    const char *dir = getenv("XDG_RUNTIME_DIR");
    char path[512];
    snprintf(path, sizeof path, "%s/jtsettings-XXXXXX",
             (dir && *dir) ? dir : "/tmp");
    fd = mkstemp(path);
    if (fd < 0)
        return -1;
    unlink(path);
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void shm_buffer_destroy(ShmBuf *b)
{
    if (b->buffer) {
        wl_buffer_destroy(b->buffer);
        b->buffer = NULL;
    }
    if (b->data) {
        munmap(b->data, b->size);
        b->data = NULL;
    }
    b->size = 0;
    b->width = b->height = 0;
    b->busy = b->retired = 0;
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    (void)buffer;
    ShmBuf *b = data;
    b->busy = 0;
    if (b->retired)
        shm_buffer_destroy(b);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static int shm_buffer_create(App *a, ShmBuf *b, int w, int h)
{
    int stride = w * 4;
    size_t size = (size_t)stride * (size_t)h;
    int fd = create_anon_file(size);
    if (fd < 0)
        return -1;

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return -1;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(a->shm, fd, (int32_t)size);
    if (!pool) {
        munmap(data, size);
        close(fd);
        return -1;
    }

    b->buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                          WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (!b->buffer) {
        munmap(data, size);
        return -1;
    }

    b->data = data;
    b->size = size;
    b->width = w;
    b->height = h;
    b->busy = 0;
    b->retired = 0;
    wl_buffer_add_listener(b->buffer, &buffer_listener, b);
    return 0;
}

static ShmBuf *next_buffer(App *a, int w, int h)
{
    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (b->buffer && !b->busy && !b->retired &&
            b->width == w && b->height == h)
            return b;
    }

    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (!b->buffer && shm_buffer_create(a, b, w, h) == 0)
            return b;
    }

    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (b->buffer && !b->busy && !b->retired) {
            shm_buffer_destroy(b);
            if (shm_buffer_create(a, b, w, h) == 0)
                return b;
        }
    }

    return NULL;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    (void)time;
    App *a = data;
    wl_callback_destroy(cb);
    a->frame_cb = NULL;
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

static void request_frame(App *a)
{
    if (!a->surface || a->frame_cb)
        return;
    a->frame_cb = wl_surface_frame(a->surface);
    wl_callback_add_listener(a->frame_cb, &frame_listener, a);
}

static void app_draw(App *a)
{
    if (!a->configured || !a->surface || !a->shm)
        return;

    int w = (int)lround(WIN_W * a->scale);
    int h = (int)lround(WIN_H * a->scale);
    if (w < 1 || h < 1)
        return;

    ShmBuf *b = next_buffer(a, w, h);
    if (!b)
        return;

    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        b->data, CAIRO_FORMAT_ARGB32, w, h, w * 4);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return;
    }

    cairo_t *cr = cairo_create(surf);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_scale(cr, a->scale, a->scale);

    paint_content(cr);

    cairo_destroy(cr);
    cairo_surface_flush(surf);
    cairo_surface_destroy(surf);

    if (wl_surface_get_version(a->surface) >= 3)
        wl_surface_set_buffer_scale(a->surface, a->scale);
    wl_surface_attach(a->surface, b->buffer, 0, 0);
    wl_surface_damage(a->surface, 0, 0, (int)WIN_W, (int)WIN_H);
    request_frame(a);
    wl_surface_commit(a->surface);
    b->busy = 1;
}

static void recalc_scale(App *a)
{
    if (a->fixed_scale)
        return;

    int s = 0;
    for (int i = 0; i < a->output_count; i++) {
        if (!a->outputs[i].used)
            continue;
        if (a->outputs[i].entered && a->outputs[i].scale > s)
            s = a->outputs[i].scale;
    }
    if (s < 1)
        s = 1;
    if (s > 3)
        s = 3;
    if (s == a->scale)
        return;

    a->scale = s;
    a->dirty = 1;
    if (a->surface && wl_surface_get_version(a->surface) >= 3)
        wl_surface_set_buffer_scale(a->surface, s);

    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (!b->buffer)
            continue;
        if (b->busy)
            b->retired = 1;
        else
            shm_buffer_destroy(b);
    }
}

/* ---- protocol listeners ------------------------------------------ */

static void shm_format(void *data, struct wl_shm *shm, uint32_t format)
{
    (void)shm;
    App *a = data;
    if (format == WL_SHM_FORMAT_ARGB8888)
        a->has_argb = 1;
}

static const struct wl_shm_listener shm_listener = {
    .format = shm_format,
};

static void wm_base_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xs,
                                  uint32_t serial)
{
    App *a = data;
    xdg_surface_ack_configure(xs, serial);
    a->configured = 1;
    a->dirty = 1;
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *t,
                               int32_t width, int32_t height,
                               struct wl_array *states)
{
    App *a = data;
    (void)t;

    int maximized = 0;
    int fullscreen = 0;
    uint32_t *state;
    wl_array_for_each(state, states) {
        if (*state == XDG_TOPLEVEL_STATE_MAXIMIZED)
            maximized = 1;
        else if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN)
            fullscreen = 1;
    }

    a->state_maximized = maximized;

    if (width > 0 && height > 0) {
        WIN_W = width;
        WIN_H = height;
        if (!maximized && !fullscreen) {
            a->state_w = width;
            a->state_h = height;
        }
    }
    a->dirty = 1;
}

static void toplevel_close(void *data, struct xdg_toplevel *t)
{
    (void)t;
    App *a = data;
    a->quit = 1;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static void decoration_configure(void *data,
                                 struct zxdg_toplevel_decoration_v1 *dec,
                                 uint32_t mode)
{
    (void)data;
    (void)dec;
    (void)mode;
}

static const struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
    .configure = decoration_configure,
};

static void output_geometry(void *data, struct wl_output *output,
                            int32_t x, int32_t y, int32_t pw, int32_t ph,
                            int32_t subpixel, const char *make,
                            const char *model, int32_t transform)
{
    (void)data; (void)output; (void)x; (void)y; (void)pw; (void)ph;
    (void)subpixel; (void)make; (void)model; (void)transform;
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh)
{
    (void)data; (void)output; (void)flags;
    (void)width; (void)height; (void)refresh;
}

static void output_done(void *data, struct wl_output *output)
{
    (void)data;
    (void)output;
}

static void output_scale(void *data, struct wl_output *output, int32_t factor)
{
    (void)output;
    Output *o = data;
    if (factor < 1)
        factor = 1;
    o->scale = factor;
    if (o->entered)
        recalc_scale(o->app);
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
};

static void surface_enter(void *data, struct wl_surface *surface,
                          struct wl_output *output)
{
    (void)surface;
    App *a = data;
    for (int i = 0; i < a->output_count; i++) {
        if (!a->outputs[i].used)
            continue;
        if (a->outputs[i].output == output) {
            a->outputs[i].entered = 1;
            break;
        }
    }
    recalc_scale(a);
}

static void surface_leave(void *data, struct wl_surface *surface,
                          struct wl_output *output)
{
    (void)surface;
    App *a = data;
    for (int i = 0; i < a->output_count; i++) {
        if (!a->outputs[i].used)
            continue;
        if (a->outputs[i].output == output) {
            a->outputs[i].entered = 0;
            break;
        }
    }
    recalc_scale(a);
}

static const struct wl_surface_listener surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
};

/* ---- output management ------------------------------------------- */

static void head_pick_default_mode(Head *h)
{
    if (head_mode_valid(h))
        return;
    int pick = -1;
    for (int i = 0; i < h->mode_count; i++) {
        const HeadMode *m = &h->modes[i];
        if (!m->available || m->width <= 0)
            continue;
        if (m->preferred) {
            pick = i;
            break;
        }
        if (pick < 0)
            pick = i;
    }
    if (pick >= 0)
        h->p_mode = pick;
}

static HeadMode *head_mode_from_obj(Head *h, struct zwlr_output_mode_v1 *obj)
{
    for (int i = 0; i < h->mode_count; i++)
        if (h->modes[i].obj == obj)
            return &h->modes[i];
    return NULL;
}

static void mode_size(void *data, struct zwlr_output_mode_v1 *mode,
                      int32_t width, int32_t height)
{
    (void)mode;
    HeadMode *m = data;
    m->width = width;
    m->height = height;
}

static void mode_refresh(void *data, struct zwlr_output_mode_v1 *mode,
                         int32_t refresh)
{
    (void)mode;
    HeadMode *m = data;
    m->refresh = refresh;
}

static void mode_preferred(void *data, struct zwlr_output_mode_v1 *mode)
{
    (void)mode;
    HeadMode *m = data;
    m->preferred = 1;
}

static void mode_finished(void *data, struct zwlr_output_mode_v1 *mode)
{
    HeadMode *m = data;
    m->available = 0;
    m->obj = NULL;
    zwlr_output_mode_v1_destroy(mode);
}

static const struct zwlr_output_mode_v1_listener mode_listener = {
    .size = mode_size,
    .refresh = mode_refresh,
    .preferred = mode_preferred,
    .finished = mode_finished,
};

static void head_name(void *data, struct zwlr_output_head_v1 *head,
                      const char *name)
{
    (void)head;
    Head *h = data;
    snprintf(h->name, sizeof h->name, "%s", name ? name : "");
}

static void head_description_event(void *data,
                                   struct zwlr_output_head_v1 *head,
                                   const char *description)
{
    (void)head;
    Head *h = data;
    snprintf(h->description, sizeof h->description, "%s",
             description ? description : "");
}

static void head_physical_size(void *data, struct zwlr_output_head_v1 *head,
                               int32_t width, int32_t height)
{
    (void)head;
    Head *h = data;
    h->phys_w = width;
    h->phys_h = height;
}

static void head_mode_event(void *data, struct zwlr_output_head_v1 *head,
                            struct zwlr_output_mode_v1 *mode)
{
    (void)head;
    Head *h = data;
    if (h->mode_count >= MAX_MODES) {
        zwlr_output_mode_v1_destroy(mode);
        return;
    }
    HeadMode *m = &h->modes[h->mode_count++];
    memset(m, 0, sizeof *m);
    m->obj = mode;
    m->available = 1;
    zwlr_output_mode_v1_add_listener(mode, &mode_listener, m);
}

static void head_enabled(void *data, struct zwlr_output_head_v1 *head,
                         int32_t enabled)
{
    (void)head;
    Head *h = data;
    h->enabled = enabled != 0;
}

static void head_current_mode(void *data, struct zwlr_output_head_v1 *head,
                              struct zwlr_output_mode_v1 *mode)
{
    (void)head;
    Head *h = data;
    h->current_mode = mode;
    HeadMode *m = head_mode_from_obj(h, mode);
    if (m) {
        h->cur_w = m->width;
        h->cur_h = m->height;
        h->cur_refresh = m->refresh;
    }
}

static void head_position(void *data, struct zwlr_output_head_v1 *head,
                          int32_t x, int32_t y)
{
    (void)head;
    Head *h = data;
    h->x = x;
    h->y = y;
}

static void head_transform(void *data, struct zwlr_output_head_v1 *head,
                           int32_t transform)
{
    (void)head;
    Head *h = data;
    h->transform = transform;
}

static void head_scale(void *data, struct zwlr_output_head_v1 *head,
                       wl_fixed_t scale)
{
    (void)head;
    Head *h = data;
    double s = wl_fixed_to_double(scale);
    h->scale = s > 0.0 ? s : 1.0;
}

static void head_finished(void *data, struct zwlr_output_head_v1 *head)
{
    Head *h = data;
    head_ui_invalidate_slot((int)(h - g_heads));
    h->used = 0;
    h->head = NULL;
    h->current_mode = NULL;
    for (int i = 0; i < h->mode_count; i++) {
        h->modes[i].available = 0;
        h->modes[i].obj = NULL;
    }
    zwlr_output_head_v1_destroy(head);
}

static void head_make(void *data, struct zwlr_output_head_v1 *head,
                      const char *make)
{
    (void)head;
    Head *h = data;
    snprintf(h->make, sizeof h->make, "%s", make ? make : "");
}

static void head_model(void *data, struct zwlr_output_head_v1 *head,
                       const char *model)
{
    (void)head;
    Head *h = data;
    snprintf(h->model, sizeof h->model, "%s", model ? model : "");
}

static void head_serial_number(void *data, struct zwlr_output_head_v1 *head,
                               const char *serial)
{
    (void)data;
    (void)head;
    (void)serial;
}

static void head_adaptive_sync(void *data, struct zwlr_output_head_v1 *head,
                               uint32_t state)
{
    (void)data;
    (void)head;
    (void)state;
}

static const struct zwlr_output_head_v1_listener head_listener = {
    .name = head_name,
    .description = head_description_event,
    .physical_size = head_physical_size,
    .mode = head_mode_event,
    .enabled = head_enabled,
    .current_mode = head_current_mode,
    .position = head_position,
    .transform = head_transform,
    .scale = head_scale,
    .finished = head_finished,
    .make = head_make,
    .model = head_model,
    .serial_number = head_serial_number,
    .adaptive_sync = head_adaptive_sync,
};

static void manager_head(void *data, struct zwlr_output_manager_v1 *manager,
                         struct zwlr_output_head_v1 *head)
{
    (void)manager;
    App *a = data;

    int slot = -1;
    for (int i = 0; i < MAX_HEADS; i++) {
        if (!g_heads[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        zwlr_output_head_v1_destroy(head);
        return;
    }

    Head *h = &g_heads[slot];
    memset(h, 0, sizeof *h);
    h->used = 1;
    h->head = head;
    h->scale = 1.0;
    h->p_scale = 1.0;
    h->p_mode = -1;
    h->pending_ready = 0;
    snprintf(h->name, sizeof h->name, "Output %d", slot + 1);
    zwlr_output_head_v1_add_listener(head, &head_listener, h);
    a->dirty = 1;
}

static void manager_done(void *data, struct zwlr_output_manager_v1 *manager,
                         uint32_t serial)
{
    (void)manager;
    App *a = data;
    a->output_serial = serial;

    for (int i = 0; i < MAX_HEADS; i++) {
        Head *h = &g_heads[i];
        if (!h->used)
            continue;
        if (!h->pending_ready || !h->dirty)
            displays_sync_pending(h);
    }
    a->dirty = 1;
}

static void manager_finished(void *data,
                             struct zwlr_output_manager_v1 *manager)
{
    App *a = data;
    if (a->output_manager == manager)
        a->output_manager = NULL;
    g_output_manager_available = 0;
    /* heads_clear() only drops our bookkeeping; release any protocol objects
     * that still outlived their own finished events. */
    for (int i = 0; i < MAX_HEADS; i++) {
        Head *h = &g_heads[i];
        for (int j = 0; j < h->mode_count; j++) {
            if (h->modes[j].obj) {
                zwlr_output_mode_v1_destroy(h->modes[j].obj);
                h->modes[j].obj = NULL;
            }
        }
        if (h->head) {
            zwlr_output_head_v1_destroy(h->head);
            h->head = NULL;
        }
    }
    heads_clear();
    zwlr_output_manager_v1_destroy(manager);
    a->dirty = 1;
}

static const struct zwlr_output_manager_v1_listener manager_listener = {
    .head = manager_head,
    .done = manager_done,
    .finished = manager_finished,
};

static void config_succeeded(void *data,
                             struct zwlr_output_configuration_v1 *config)
{
    App *a = data;
    if (a->pending_config == config)
        a->pending_config = NULL;
    zwlr_output_configuration_v1_destroy(config);
    for (int i = 0; i < MAX_HEADS; i++)
        if (g_heads[i].used)
            g_heads[i].dirty = 0;
    displays_commit();
    ui_toast("Display configuration applied");
    a->dirty = 1;
}

static void config_failed(void *data,
                          struct zwlr_output_configuration_v1 *config)
{
    App *a = data;
    if (a->pending_config == config)
        a->pending_config = NULL;
    zwlr_output_configuration_v1_destroy(config);
    ui_toast("Display configuration rejected");
    a->dirty = 1;
}

static void config_cancelled(void *data,
                             struct zwlr_output_configuration_v1 *config)
{
    App *a = data;
    if (a->pending_config == config)
        a->pending_config = NULL;
    zwlr_output_configuration_v1_destroy(config);
    ui_toast("Display configuration cancelled");
    a->dirty = 1;
}

static const struct zwlr_output_configuration_v1_listener config_listener = {
    .succeeded = config_succeeded,
    .failed = config_failed,
    .cancelled = config_cancelled,
};

static void displays_apply(void)
{
    App *a = g_app;
    if (!a || !a->output_manager) {
        ui_toast("Output management is not available");
        return;
    }
    if (a->pending_config) {
        ui_toast("Waiting for the compositor");
        return;
    }
    if (head_enabled_count() <= 0) {
        ui_toast("At least one display must stay enabled");
        return;
    }

    struct zwlr_output_configuration_v1 *config =
        zwlr_output_manager_v1_create_configuration(a->output_manager,
                                                    a->output_serial);
    if (!config) {
        ui_toast("Could not create a display configuration");
        return;
    }

    for (int i = 0; i < MAX_HEADS; i++) {
        Head *h = &g_heads[i];
        if (!h->used || !h->head)
            continue;
        if (!h->p_enabled) {
            zwlr_output_configuration_v1_disable_head(config, h->head);
            continue;
        }

        head_pick_default_mode(h);
        struct zwlr_output_configuration_head_v1 *ch =
            zwlr_output_configuration_v1_enable_head(config, h->head);
        if (!ch)
            continue;

        if (head_mode_valid(h))
            zwlr_output_configuration_head_v1_set_mode(
                ch, h->modes[h->p_mode].obj);
        zwlr_output_configuration_head_v1_set_scale(
            ch, wl_fixed_from_double(h->p_scale));
        zwlr_output_configuration_head_v1_set_transform(ch, h->p_transform);
        zwlr_output_configuration_head_v1_set_position(ch, h->p_x, h->p_y);
    }

    a->pending_config = config;
    zwlr_output_configuration_v1_add_listener(config, &config_listener, a);
    zwlr_output_configuration_v1_apply(config);
    ui_toast("Applying display configuration…");
}


/* ---- interaction ------------------------------------------------- */

static void slider_set_from_x(int idx, double x)
{
    if (idx < 0 || idx >= g_hit_count)
        return;
    Hit *h = &g_hits[idx];
    if (h->kind != HIT_SLIDER || h->a < 0 || h->a >= SL_COUNT)
        return;
    double t = (x - h->x) / (h->w > 0.0 ? h->w : 1.0);
    g_ui.sliders[h->a] = clampd(t, 0.0, 1.0);
}

static void ui_activate(App *a, int idx)
{
    (void)a;
    if (idx < 0 || idx >= g_hit_count)
        return;

    Hit *h = &g_hits[idx];
    switch (h->kind) {
    case HIT_NAV:
        if (h->a >= 0 && h->a < PAGE_COUNT)
            ui_set_page(h->a);
        break;
    case HIT_TOGGLE:
        if (h->a >= 0 && h->a < TOG_COUNT) {
            g_ui.toggles[h->a] = !g_ui.toggles[h->a];
            if (input_toggle(h->a))
                input_commit();
        }
        break;
    case HIT_START_TOGGLE:
        if (h->a >= 0 && h->a < g_startup_count) {
            g_startups[h->a].enabled = !g_startups[h->a].enabled;
            startup_save();
            ui_toast(g_startups[h->a].enabled
                     ? "Startup application enabled"
                     : "Startup application disabled");
        }
        break;
    case HIT_SEGMENT:
        if (h->a >= 0 && h->a < SEG_COUNT) {
            g_ui.segments[h->a] = h->b;
            if (h->a == SEG_KB_FILTER) {
                g_ui.kb_scroll = 0.0;
                g_ui.hover = -1;
            } else if (h->a == SEG_SCROLL || h->a == SEG_ACCEL) {
                input_commit();
            } else if (h->a == SEG_WX_UNITS) {
                g_profile_cfg.fahrenheit = h->b;
                profile_save();
                ui_toast(h->b ? "Temperature set to Fahrenheit"
                              : "Temperature set to Celsius");
            }
        }
        break;
    case HIT_ROW:
        if (g_ui.page >= 0 && g_ui.page < PAGE_COUNT)
            g_ui.selected[g_ui.page] = h->a;
        break;
    case HIT_DISP_RES:
        g_ui.mode_picker_open = 1;
        g_ui.mode_picker_head = h->a;
        g_ui.pos_editor_open = 0;
        g_ui.disp_drag = -1;
        break;
    case HIT_DISP_REFRESH: {
        Head *hh = head_get(h->a);
        if (!hh)
            break;
        int32_t pw = hh->cur_w, ph = hh->cur_h;
        if (head_mode_valid(hh)) {
            pw = hh->modes[hh->p_mode].width;
            ph = hh->modes[hh->p_mode].height;
        }
        int32_t refreshes[8];
        int rn = head_refreshes(hh, pw, ph, refreshes, 8);
        if (h->b >= 0 && h->b < rn)
            head_choose_refresh(hh, pw, ph, refreshes[h->b]);
        break;
    }
    case HIT_DISP_SCALE: {
        Head *hh = head_get(h->a);
        if (hh && h->b >= 0 && h->b < DISP_SCALE_COUNT) {
            hh->p_scale = DISP_SCALES[h->b];
            hh->dirty = 1;
        }
        break;
    }
    case HIT_DISP_ORIENT: {
        Head *hh = head_get(h->a);
        if (hh) {
            if (h->b == 0)
                hh->p_transform = 0;
            else
                hh->p_transform = hh->p_transform == 3 ? 3 : 1;
            hh->dirty = 1;
        }
        break;
    }
    case HIT_DISP_ENABLE:
        displays_toggle_enabled(h->a);
        break;
    case HIT_DISP_PRIMARY:
        displays_make_primary(h->a);
        break;
    case HIT_DISP_MODE: {
        Head *hh = head_get(h->a);
        if (hh) {
            Resolution res[MAX_RESOLUTIONS];
            int rn = head_resolutions(hh, res, MAX_RESOLUTIONS);
            if (h->b >= 0 && h->b < rn)
                head_choose_resolution(hh, res[h->b].w, res[h->b].h);
        }
        g_ui.mode_picker_open = 0;
        g_ui.mode_picker_head = -1;
        break;
    }

    case HIT_APP_FIELD:
        g_ui.mode_picker_open = 0;
        g_ui.pos_editor_open = 0;
        g_ui.app_picker_cat = h->a;
        g_ui.app_picker_scroll = 0;
        g_ui.app_picker_open = 1;
        g_hit_count = 0;
        g_ui.hover = -1;
        g_ui.pressed = -1;
        break;
    case HIT_APP_PICK:
        defaultapps_choose(h->a, h->b);
        break;
    case HIT_APP_PICKER_CLOSE:
        g_ui.app_picker_open = 0;
        break;

    case HIT_WX_FIELD:
        g_ui.mode_picker_open = 0;
        g_ui.pos_editor_open = 0;
        g_ui.app_picker_open = 0;
        g_ui.wx_query_editing = 0;
        g_ui.wx_editing = 1;
        snprintf(g_ui.wx_location_input, sizeof g_ui.wx_location_input,
                 "%s", g_profile_cfg.location);
        g_ui.hover = -1;
        g_ui.pressed = -1;
        break;
    case HIT_WX_QUERY_FIELD:
        g_ui.wx_editing = 0;
        g_ui.wx_query_editing = 1;
        g_ui.hover = -1;
        g_ui.pressed = -1;
        break;
    case HIT_WX_RESULT:
        if (h->a >= 0 && h->a < g_wx_result_count) {
            WxResult *r = &g_wx_results[h->a];
            snprintf(g_profile_cfg.location, sizeof g_profile_cfg.location,
                     "%s", r->name);
            g_profile_cfg.lat = r->lat;
            g_profile_cfg.lon = r->lon;
            g_profile_cfg.have_coords = 1;
            g_wx_result_count = 0;
            g_ui.wx_query_editing = 0;
            g_ui.wx_query_input[0] = '\0';
            profile_save();
            ui_toast("Weather location saved");
        }
        break;
    case HIT_ZOOM_SCRIM:
        zoom_cancel();
        break;

    case HIT_BUTTON:
        if (h->a == BTN_START_ADD) {
            if (g_startup_count >= STARTUP_MAX) {
                ui_toast("Startup list is full");
            } else {
                g_ui.startup_adding = 1;
                g_ui.startup_input[0] = '\0';
            }
            break;
        }
        if (h->a >= BTN_START_REMOVE_FIRST &&
            h->a <= BTN_START_REMOVE_LAST &&
            h->a < BTN_START_REMOVE_FIRST + g_startup_count) {
            startup_remove(h->a - BTN_START_REMOVE_FIRST);
            ui_toast("Startup application removed");
            break;
        }
        switch (h->a) {
        case BTN_NEW_RULE:
            ui_toast("Preview only — window rules are not saved");
            break;
        case BTN_DOCS:
            ui_toast("Preview only — docs would open in a browser");
            break;
        case BTN_UPDATE:
            if (focus_app_window(a, "jtupdate"))
                ui_toast("Focused jtupdate");
            else {
                launch_jtupdate();
                ui_toast("Opening jtupdate…");
            }
            break;
        case BTN_POSITION:
            g_ui.pos_editor_open = 1;
            g_ui.mode_picker_open = 0;
            g_ui.mode_picker_head = -1;
            g_ui.pos_layout_valid = 0;
            g_ui.disp_drag = -1;
            g_ui.hover = -1;
            g_ui.pressed = -1;
            ui_toast("Drag displays to arrange them");
            break;
        case BTN_POS_DONE:
            g_ui.pos_editor_open = 0;
            g_ui.disp_drag = -1;
            ui_toast("Arrangement updated — press Apply to use it");
            break;
        case BTN_DISPLAYS_APPLY:
            displays_apply();
            break;
        case BTN_DISPLAYS_RESET:
            displays_sync_all();
            g_ui.mode_picker_open = 0;
            g_ui.mode_picker_head = -1;
            g_ui.pos_editor_open = 0;
            g_ui.disp_drag = -1;
            ui_toast("Display changes reset");
            break;
        case BTN_MODE_CLOSE:
            g_ui.mode_picker_open = 0;
            g_ui.mode_picker_head = -1;
            break;
        case BTN_WX_SEARCH:
            wx_search_start();
            break;
        case BTN_PHOTO_PICK:
            profile_open_picker();
            break;
        case BTN_PHOTO_CLEAR:
            g_profile_cfg.photo[0] = '\0';
            g_profile_cfg.zoom = 1.0;
            g_ui.sliders[SL_PHOTO_ZOOM] = 0.0;
            if (g_photo_preview) {
                cairo_surface_destroy(g_photo_preview);
                g_photo_preview = NULL;
            }
            profile_save();
            ui_toast("Profile photo removed");
            break;
        case BTN_ZOOM_CANCEL:
            zoom_cancel();
            break;
        case BTN_ZOOM_SAVE:
            zoom_accept();
            break;
        default:
            ui_toast("Preview only — no command was run");
            break;
        }
        break;
    default:
        break;
    }
}

static void update_cursor(App *a)
{
    if (!a->cursor_shape_manager || !a->pointer)
        return;
    if (!a->cursor_shape_device) {
        a->cursor_shape_device = wp_cursor_shape_manager_v1_get_pointer(
            a->cursor_shape_manager, a->pointer);
        a->cursor_shape = 0;
    }

    uint32_t shape = g_ui.hover >= 0 ?
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER :
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;

    if (shape != a->cursor_shape) {
        wp_cursor_shape_device_v1_set_shape(a->cursor_shape_device,
                                            a->pointer_serial, shape);
        a->cursor_shape = shape;
    }
}

static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t sx, wl_fixed_t sy)
{
    (void)pointer; (void)surface;
    App *a = data;
    a->pointer_serial = serial;
    g_ui.pointer_x = wl_fixed_to_double(sx);
    g_ui.pointer_y = wl_fixed_to_double(sy);
    g_ui.hover = hit_at(g_ui.pointer_x, g_ui.pointer_y);
    a->cursor_shape = 0;
    a->dirty = 1;
    update_cursor(a);
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface)
{
    (void)pointer; (void)serial; (void)surface;
    App *a = data;
    g_ui.pointer_x = -1;
    g_ui.pointer_y = -1;
    g_ui.hover = -1;
    g_ui.pressed = -1;
    g_ui.kb_drag = 0;
    a->dirty = 1;
    update_cursor(a);
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    (void)pointer; (void)time;
    App *a = data;
    g_ui.pointer_x = wl_fixed_to_double(sx);
    g_ui.pointer_y = wl_fixed_to_double(sy);

    if (g_ui.mode_picker_open) {
        g_ui.hover = hit_at(g_ui.pointer_x, g_ui.pointer_y);
        a->dirty = 1;
        update_cursor(a);
        return;
    }

    if (g_ui.app_picker_open) {
        g_ui.hover = hit_at(g_ui.pointer_x, g_ui.pointer_y);
        a->dirty = 1;
        update_cursor(a);
        return;
    }

    if (g_ui.pos_editor_open) {
        if (g_ui.disp_drag >= 0 && g_ui.pos_scale > 0.0) {
            Head *h = head_get(g_ui.disp_drag);
            if (h) {
                double nx = (g_ui.pointer_x - g_ui.disp_drag_ox -
                             g_ui.pos_offx) / g_ui.pos_scale;
                double ny = (g_ui.pointer_y - g_ui.disp_drag_oy -
                             g_ui.pos_offy) / g_ui.pos_scale;
                h->p_x = (int32_t)llround(nx);
                h->p_y = (int32_t)llround(ny);
                h->dirty = 1;
            }
        }
        g_ui.hover = -1;
        a->dirty = 1;
        update_cursor(a);
        return;
    }

    if (g_ui.kb_drag) {
        double delta = g_ui.kb_drag_y - g_ui.pointer_y;
        if (fabs(delta) > 3.0)
            g_ui.kb_drag_moved = 1;
        g_ui.kb_scroll = clampd(g_ui.kb_drag_scroll + delta,
                                0.0, kb_max_scroll());
        g_ui.hover = -1;
        a->dirty = 1;
        update_cursor(a);
        return;
    }

    g_ui.hover = hit_at(g_ui.pointer_x, g_ui.pointer_y);

    if (g_ui.pressed >= 0 && g_ui.pressed < g_hit_count &&
        g_hits[g_ui.pressed].kind == HIT_SLIDER)
        slider_set_from_x(g_ui.pressed, g_ui.pointer_x);

    a->dirty = 1;
    update_cursor(a);
}

static void pointer_button(void *data, struct wl_pointer *pointer,
                           uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state)
{
    (void)pointer;
    (void)serial;
    (void)time;
    App *a = data;

    if (button != BTN_LEFT)
        return;

    double x = g_ui.pointer_x;
    double y = g_ui.pointer_y;

    if (g_ui.mode_picker_open) {
        if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
            int idx = hit_at(x, y);
            int handled = 0;
            if (idx >= 0) {
                Hit *hh = &g_hits[idx];
                if (hh->kind == HIT_DISP_MODE ||
                    (hh->kind == HIT_BUTTON && hh->a == BTN_MODE_CLOSE)) {
                    ui_activate(a, idx);
                    handled = 1;
                }
            }
            if (!handled) {
                Head *h = head_get(g_ui.mode_picker_head);
                double px = 0.0, py = 0.0, pw = 0.0, ph = 0.0;
                if (h)
                    mode_picker_rect(h, &px, &py, &pw, &ph);
                if (!h || x < px || x > px + pw ||
                    y < py || y > py + ph) {
                    g_ui.mode_picker_open = 0;
                    g_ui.mode_picker_head = -1;
                }
            }
        }
        a->dirty = 1;
        update_cursor(a);
        return;
    }

    if (g_ui.app_picker_open) {
        if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
            int idx = hit_at(x, y);
            int handled = 0;
            if (idx >= 0) {
                Hit *hh = &g_hits[idx];
                if (hh->kind == HIT_APP_PICK ||
                    hh->kind == HIT_APP_PICKER_CLOSE) {
                    ui_activate(a, idx);
                    handled = 1;
                }
            }
            if (!handled) {
                double px = 0.0, py = 0.0, pw = 0.0, ph = 0.0;
                app_picker_rect(&px, &py, &pw, &ph);
                if (x < px || x > px + pw || y < py || y > py + ph)
                    g_ui.app_picker_open = 0;
            }
        }
        a->dirty = 1;
        update_cursor(a);
        return;
    }

    int idx = hit_at(x, y);

    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (g_ui.pos_editor_open) {
            double pw = 700.0, ph = 420.0;
            double px = (WIN_W - pw) / 2.0;
            double py = (WIN_H - ph) / 2.0;
            double dbx = px + pw - 24.0 - 120.0;
            double dby = py + ph - 24.0 - 36.0;
            if (x >= dbx && x <= dbx + 120.0 &&
                y >= dby && y <= dby + 36.0) {
                g_ui.pos_editor_open = 0;
                g_ui.disp_drag = -1;
                ui_toast("Arrangement updated — press Apply to use it");
                a->dirty = 1;
                update_cursor(a);
                return;
            }
            /* Pick the topmost display rectangle. */
            for (int i = MAX_HEADS - 1; i >= 0; i--) {
                if (!head_in_arrangement(i))
                    continue;
                Head *h = &g_heads[i];
                if (!h->p_enabled)
                    continue;
                double lw, lh;
                head_logical_size(h, &lw, &lh);
                double rx = g_ui.pos_offx + h->p_x * g_ui.pos_scale;
                double ry = g_ui.pos_offy + h->p_y * g_ui.pos_scale;
                double rw = lw * g_ui.pos_scale;
                double rh = lh * g_ui.pos_scale;
                if (rw < 28.0) rw = 28.0;
                if (rh < 20.0) rh = 20.0;
                if (x >= rx && x <= rx + rw && y >= ry && y <= ry + rh) {
                    g_ui.disp_drag = i;
                    g_ui.disp_drag_ox = x - rx;
                    g_ui.disp_drag_oy = y - ry;
                    a->dirty = 1;
                    update_cursor(a);
                    return;
                }
            }
            a->dirty = 1;
            update_cursor(a);
            return;
        }
        if (g_ui.page == PAGE_KEYBINDINGS) {
            double ty, th, view_h;
            kb_geom(&ty, &th, &view_h);
            (void)view_h;
            if (x >= CONTENT_X && x <= CONTENT_X + CONTENT_W &&
                y >= ty && y <= ty + th) {
                g_ui.kb_drag = 1;
                g_ui.kb_drag_moved = 0;
                g_ui.kb_drag_y = y;
                g_ui.kb_drag_scroll = g_ui.kb_scroll;
            }
        }
        g_ui.pressed = idx;
        g_ui.hover = idx;
        if (idx >= 0 && g_hits[idx].kind == HIT_SLIDER)
            slider_set_from_x(idx, x);
    } else {
        if (g_ui.pos_editor_open) {
            g_ui.disp_drag = -1;
            a->dirty = 1;
            update_cursor(a);
            return;
        }
        int kind = (idx >= 0) ? g_hits[idx].kind : HIT_NONE;
        int pressed = g_ui.pressed;
        int was_drag = g_ui.kb_drag && g_ui.kb_drag_moved;
        g_ui.kb_drag = 0;
        if (idx >= 0 && idx == pressed && kind != HIT_SLIDER && !was_drag)
            ui_activate(a, idx);
        if (pressed >= 0 && pressed < g_hit_count &&
            g_hits[pressed].kind == HIT_SLIDER) {
            if (g_hits[pressed].a == SL_START_DELAY) {
                startup_save();
                ui_toast("Autostart delay saved");
            } else if (g_hits[pressed].a == SL_POINTER ||
                       g_hits[pressed].a == SL_REPEAT ||
                       g_hits[pressed].a == SL_DELAY) {
                input_commit();
            } else if (g_hits[pressed].a == SL_PHOTO_ZOOM && !g_zoom_open) {
                g_profile_cfg.zoom = photo_zoom_value();
                profile_save();
                ui_toast("Profile zoom saved");
            }
        }
        g_ui.pressed = -1;
        g_ui.hover = (kind == HIT_NAV) ? -1 : hit_at(x, y);
    }
    a->dirty = 1;
    update_cursor(a);
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
                         uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)pointer; (void)time;
    App *a = data;

    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
        return;

    if (g_ui.app_picker_open) {
        int max = app_picker_scroll_max();
        int next = g_ui.app_picker_scroll +
                   (int)lround(wl_fixed_to_double(value));
        if (next < 0)
            next = 0;
        if (next > max)
            next = max;
        if (next != g_ui.app_picker_scroll) {
            g_ui.app_picker_scroll = next;
            g_hit_count = 0;
            g_ui.hover = -1;
            g_ui.pressed = -1;
            a->dirty = 1;
        }
        return;
    }

    if (g_ui.page == PAGE_DISPLAYS && !g_ui.pos_editor_open &&
        !g_ui.mode_picker_open) {
        int max_scroll = head_count() * 220 -
                         (int)(DISPLAY_VIEW_BOTTOM - DISPLAY_VIEW_TOP);
        if (max_scroll < 0)
            max_scroll = 0;
        g_ui.disp_scroll += (int)lround(wl_fixed_to_double(value));
        if (g_ui.disp_scroll < 0)
            g_ui.disp_scroll = 0;
        if (g_ui.disp_scroll > max_scroll)
            g_ui.disp_scroll = max_scroll;
        g_ui.hover = -1;
        a->dirty = 1;
        return;
    }

    if (g_ui.page != PAGE_KEYBINDINGS)
        return;

    double ty, th, view_h;
    kb_geom(&ty, &th, &view_h);
    (void)view_h;
    if (g_ui.pointer_x < CONTENT_X ||
        g_ui.pointer_x > CONTENT_X + CONTENT_W ||
        g_ui.pointer_y < ty || g_ui.pointer_y > ty + th)
        return;

    double max_scroll = kb_max_scroll();
    if (max_scroll <= 0.0)
        return;

    double next = g_ui.kb_scroll + wl_fixed_to_double(value) * 2.0;
    g_ui.kb_scroll = clampd(next, 0.0, max_scroll);
    g_ui.hover = -1;
    a->dirty = 1;
}

static void pointer_frame(void *data, struct wl_pointer *pointer)
{
    (void)data;
    (void)pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer,
                                uint32_t source)
{
    (void)data; (void)pointer; (void)source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer,
                              uint32_t time, uint32_t axis)
{
    (void)data; (void)pointer; (void)time; (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer,
                                  uint32_t axis, int32_t discrete)
{
    (void)data; (void)pointer; (void)axis; (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int32_t fd, uint32_t size)
{
    (void)keyboard;
    App *a = data;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return;

    struct xkb_keymap *km = xkb_keymap_new_from_string(
        a->xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!km)
        return;

    struct xkb_state *st = xkb_state_new(km);
    if (!st) {
        xkb_keymap_unref(km);
        return;
    }

    xkb_state_unref(a->xkb_state);
    xkb_keymap_unref(a->xkb_keymap);
    a->xkb_keymap = km;
    a->xkb_state = st;
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys)
{
    (void)data; (void)keyboard; (void)serial; (void)surface; (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface)
{
    (void)data; (void)keyboard; (void)serial; (void)surface;
}

static size_t
utf8_seq_len(unsigned char c)
{
    if (c < 0x80)
        return 1;
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 0;
}

/* Shared single-line text editing.  Returns 0 if the key was not consumed,
 * 1 if it edited the buffer, 2 for Enter and 3 for Escape. */
static int
text_input_key(App *a, uint32_t key, xkb_keysym_t sym, char *buf, size_t n)
{
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)
        return 2;
    if (sym == XKB_KEY_Escape)
        return 3;
    if (sym == XKB_KEY_BackSpace) {
        size_t len = strlen(buf);
        while (len > 0 && ((unsigned char)buf[len - 1] & 0xC0) == 0x80)
            len--;
        if (len > 0)
            len--;
        buf[len] = '\0';
        return 1;
    }
    if (sym == XKB_KEY_Delete) {
        buf[0] = '\0';
        return 1;
    }

    char tmp[16];
    int got = xkb_state_key_get_utf8(a->xkb_state, key + 8, tmp, sizeof tmp);
    if (got > 0) {
        size_t len = strlen(buf);
        size_t room = n > 0 ? n - len - 1 : 0;
        for (int i = 0; i < got && room > 0; ) {
            unsigned char c = (unsigned char)tmp[i];
            size_t clen = utf8_seq_len(c);
            if (clen == 0 || (size_t)(got - i) < clen) {
                i++;
                continue;
            }
            int valid = 1;
            for (size_t j = 1; j < clen; j++) {
                if (((unsigned char)tmp[i + j] & 0xC0) != 0x80) {
                    valid = 0;
                    break;
                }
            }
            if (!valid || (clen == 1 && (c < 0x20 || c == 0x7F))) {
                i++;
                continue;
            }
            if (clen > room)
                break;
            memcpy(buf + len, tmp + i, clen);
            len += clen;
            room -= clen;
            i += (int)clen;
        }
        buf[len] = '\0';
    }
    return 1;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                         uint32_t serial, uint32_t time,
                         uint32_t key, uint32_t state)
{
    (void)keyboard; (void)serial; (void)time;
    App *a = data;

    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !a->xkb_state)
        return;

    a->dirty = 1;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(a->xkb_state, key + 8);

    /* A modal owns the keyboard.  Never let a page shortcut or quit key act
     * behind one: pointer input is already gated the same way. */
    if (g_ui.mode_picker_open) {
        if (sym == XKB_KEY_Escape) {
            g_ui.mode_picker_open = 0;
            g_ui.mode_picker_head = -1;
        }
        return;
    }
    if (g_ui.pos_editor_open) {
        if (sym == XKB_KEY_Escape) {
            g_ui.pos_editor_open = 0;
            g_ui.pos_layout_valid = 0;
            g_ui.disp_drag = -1;
        }
        return;
    }

    /* The zoom confirmation owns the keyboard until it is confirmed. */
    if (g_zoom_open) {
        if (sym == XKB_KEY_Escape)
            zoom_cancel();
        else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)
            zoom_accept();
        return;
    }

    if (g_ui.app_picker_open) {
        if (sym == XKB_KEY_Escape) {
            g_ui.app_picker_open = 0;
        } else if (sym == XKB_KEY_Down || sym == XKB_KEY_Page_Down ||
                   sym == XKB_KEY_Up || sym == XKB_KEY_Page_Up) {
            int max = app_picker_scroll_max();
            int step = 1;
            if (sym == XKB_KEY_Page_Down || sym == XKB_KEY_Page_Up) {
                int cat = g_ui.app_picker_cat;
                int ids[MAX_DESKTOP_APPS];
                int count = (cat >= 0 && cat < APP_CAT_COUNT)
                    ? defaultapps_candidates(cat, ids, MAX_DESKTOP_APPS) : 0;
                step = app_picker_visible_rows(count, cat == 0);
            }
            int next = g_ui.app_picker_scroll;
            if (sym == XKB_KEY_Down || sym == XKB_KEY_Page_Down)
                next += step;
            else
                next -= step;
            if (next < 0)
                next = 0;
            if (next > max)
                next = max;
            if (next != g_ui.app_picker_scroll) {
                g_ui.app_picker_scroll = next;
                g_hit_count = 0;
                g_ui.hover = -1;
                g_ui.pressed = -1;
            }
        }
        return;
    }

    /* The weather location / search fields own the keyboard while editing. */
    if (g_ui.page == PAGE_PROFILE &&
        (g_ui.wx_editing || g_ui.wx_query_editing)) {
        int typed = g_ui.wx_editing;
        char *buf = typed ? g_ui.wx_location_input : g_ui.wx_query_input;
        size_t n = typed ? sizeof g_ui.wx_location_input
                         : sizeof g_ui.wx_query_input;
        int r = text_input_key(a, key, sym, buf, n);
        if (r == 2) {
            if (typed) {
                snprintf(g_profile_cfg.location,
                         sizeof g_profile_cfg.location, "%s", buf);
                /* A hand-typed name has no coordinates; the bar will resolve
                 * it through Open-Meteo's geocoder as it did before. */
                g_profile_cfg.have_coords = 0;
                g_profile_cfg.lat = 0.0;
                g_profile_cfg.lon = 0.0;
                g_ui.wx_editing = 0;
                profile_save();
                ui_toast("Weather location saved");
            } else {
                g_ui.wx_query_editing = 0;
                wx_search_start();
            }
        } else if (r == 3) {
            g_ui.wx_editing = 0;
            g_ui.wx_query_editing = 0;
        }
        return;
    }

    /* The Add application field owns the keyboard while it is open. */
    if (g_ui.page == PAGE_STARTUP && g_ui.startup_adding) {
        int r = text_input_key(a, key, sym, g_ui.startup_input,
                               sizeof g_ui.startup_input);
        if (r == 2)
            startup_input_commit();
        else if (r == 3)
            startup_input_cancel();
        return;
    }

    /* The default terminal field owns the keyboard while it is open. */
    if (g_ui.page == PAGE_DEFAULT_APPS && g_ui.terminal_editing) {
        int r = text_input_key(a, key, sym, g_ui.terminal_input,
                               sizeof g_ui.terminal_input);
        if (r == 2)
            terminal_input_commit();
        else if (r == 3)
            terminal_input_cancel();
        return;
    }

    if (sym == XKB_KEY_Escape || sym == XKB_KEY_q || sym == XKB_KEY_x) {
        a->quit = 1;
        return;
    }
    if (sym == XKB_KEY_Down || sym == XKB_KEY_Right ||
        sym == XKB_KEY_Page_Down) {
        ui_set_page((g_ui.page + 1) % PAGE_COUNT);
        return;
    }
    if (sym == XKB_KEY_Up || sym == XKB_KEY_Left ||
        sym == XKB_KEY_Page_Up) {
        ui_set_page((g_ui.page + PAGE_COUNT - 1) % PAGE_COUNT);
        return;
    }
    if (sym >= XKB_KEY_1 && sym <= XKB_KEY_9) {
        int p = (int)(sym - XKB_KEY_1);
        if (p < PAGE_COUNT)
            ui_set_page(p);
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, uint32_t dep,
                               uint32_t latched, uint32_t locked,
                               uint32_t group)
{
    (void)keyboard; (void)serial;
    App *a = data;
    if (a->xkb_state)
        xkb_state_update_mask(a->xkb_state, dep, latched, locked, 0, 0, group);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                 int32_t rate, int32_t delay)
{
    (void)data; (void)keyboard; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    App *a = data;

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !a->pointer) {
        a->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(a->pointer, &pointer_listener, a);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && a->pointer) {
        if (a->cursor_shape_device) {
            wp_cursor_shape_device_v1_destroy(a->cursor_shape_device);
            a->cursor_shape_device = NULL;
            a->cursor_shape = 0;
        }
        wl_pointer_destroy(a->pointer);
        a->pointer = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !a->keyboard) {
        a->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(a->keyboard, &keyboard_listener, a);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && a->keyboard) {
        wl_keyboard_destroy(a->keyboard);
        a->keyboard = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

/* ---- Foreign toplevels: raise a companion app if it is already open ---- */

static int
foreign_toplevel_index(struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    for (int i = 0; i < g_toplevel_count; i++)
        if (g_toplevels[i].handle == handle)
            return i;
    return -1;
}

static void
foreign_toplevel_set_app_id(void *data,
                            struct zwlr_foreign_toplevel_handle_v1 *handle,
                            const char *app_id)
{
    (void)data;
    int idx = foreign_toplevel_index(handle);
    if (idx < 0)
        return;
    free(g_toplevels[idx].app_id);
    g_toplevels[idx].app_id = app_id ? strdup(app_id) : NULL;
}

static void
foreign_toplevel_set_title(void *data,
                           struct zwlr_foreign_toplevel_handle_v1 *handle,
                           const char *title)
{
    (void)data; (void)handle; (void)title;
}

static void
foreign_toplevel_set_state(void *data,
                           struct zwlr_foreign_toplevel_handle_v1 *handle,
                           struct wl_array *state)
{
    (void)data; (void)handle; (void)state;
}

static void
foreign_toplevel_done(void *data,
                      struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    (void)data; (void)handle;
}

static void
foreign_toplevel_closed(void *data,
                        struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    (void)data;
    int idx = foreign_toplevel_index(handle);
    if (idx < 0)
        return;
    free(g_toplevels[idx].app_id);
    g_toplevels[idx] = g_toplevels[--g_toplevel_count];
    memset(&g_toplevels[g_toplevel_count], 0,
           sizeof g_toplevels[g_toplevel_count]);
    zwlr_foreign_toplevel_handle_v1_destroy(handle);
}

static void
foreign_toplevel_parent(void *data,
                        struct zwlr_foreign_toplevel_handle_v1 *handle,
                        struct zwlr_foreign_toplevel_handle_v1 *parent)
{
    (void)data; (void)handle; (void)parent;
}

static void
foreign_toplevel_output_enter(void *data,
                              struct zwlr_foreign_toplevel_handle_v1 *handle,
                              struct wl_output *output)
{
    (void)data; (void)handle; (void)output;
}

static void
foreign_toplevel_output_leave(void *data,
                              struct zwlr_foreign_toplevel_handle_v1 *handle,
                              struct wl_output *output)
{
    (void)data; (void)handle; (void)output;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener
foreign_toplevel_listener = {
    .title = foreign_toplevel_set_title,
    .app_id = foreign_toplevel_set_app_id,
    .output_enter = foreign_toplevel_output_enter,
    .output_leave = foreign_toplevel_output_leave,
    .state = foreign_toplevel_set_state,
    .done = foreign_toplevel_done,
    .closed = foreign_toplevel_closed,
    .parent = foreign_toplevel_parent,
};

static void
foreign_toplevel_clear(void)
{
    for (int i = 0; i < g_toplevel_count; i++) {
        if (g_toplevels[i].handle)
            zwlr_foreign_toplevel_handle_v1_destroy(g_toplevels[i].handle);
        free(g_toplevels[i].app_id);
        memset(&g_toplevels[i], 0, sizeof g_toplevels[i]);
    }
    g_toplevel_count = 0;
}

static void
foreign_toplevel_new(void *data,
                     struct zwlr_foreign_toplevel_manager_v1 *manager,
                     struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    (void)data; (void)manager;
    if (g_toplevel_count >= MAX_FOREIGN_TOPLEVELS) {
        zwlr_foreign_toplevel_handle_v1_destroy(handle);
        return;
    }
    ForeignToplevel *t = &g_toplevels[g_toplevel_count++];
    memset(t, 0, sizeof *t);
    t->handle = handle;
    zwlr_foreign_toplevel_handle_v1_add_listener(
        handle, &foreign_toplevel_listener, NULL);
}

static void
foreign_toplevel_manager_finished(void *data,
                                  struct zwlr_foreign_toplevel_manager_v1 *manager)
{
    App *a = data;
    if (a && a->toplevel_manager == manager)
        a->toplevel_manager = NULL;
    zwlr_foreign_toplevel_manager_v1_destroy(manager);
    foreign_toplevel_clear();
}

static const struct zwlr_foreign_toplevel_manager_v1_listener
foreign_toplevel_manager_listener = {
    .toplevel = foreign_toplevel_new,
    .finished = foreign_toplevel_manager_finished,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    App *a = data;

    if (!strcmp(interface, wl_compositor_interface.name)) {
        a->compositor = wl_registry_bind(registry, name,
                                         &wl_compositor_interface,
                                         version < 4 ? version : 4);
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        a->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
        wl_shm_add_listener(a->shm, &shm_listener, a);
    } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
        a->wm_base = wl_registry_bind(registry, name,
                                      &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(a->wm_base, &wm_base_listener, a);
    } else if (!strcmp(interface,
                       zxdg_decoration_manager_v1_interface.name)) {
        a->deco_manager = wl_registry_bind(
            registry, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (!strcmp(interface,
                       xdg_toplevel_icon_manager_v1_interface.name)) {
        a->icon_manager = wl_registry_bind(
            registry, name, &xdg_toplevel_icon_manager_v1_interface, 1);
    } else if (!strcmp(interface,
                       wp_cursor_shape_manager_v1_interface.name)) {
        a->cursor_shape_manager = wl_registry_bind(
            registry, name, &wp_cursor_shape_manager_v1_interface, 1);
    } else if (!strcmp(interface, wl_seat_interface.name)) {
        a->seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                   version < 5 ? version : 5);
        wl_seat_add_listener(a->seat, &seat_listener, a);
    } else if (!strcmp(interface,
                       zwlr_output_manager_v1_interface.name)) {
        uint32_t v = version < 4 ? version : 4;
        a->output_manager = wl_registry_bind(
            registry, name, &zwlr_output_manager_v1_interface, v);
        if (a->output_manager) {
            g_output_manager_available = 1;
            zwlr_output_manager_v1_add_listener(
                a->output_manager, &manager_listener, a);
        }
    } else if (!strcmp(interface,
                       zwlr_foreign_toplevel_manager_v1_interface.name)) {
        uint32_t v = version < 3 ? version : 3;
        a->toplevel_manager = wl_registry_bind(
            registry, name, &zwlr_foreign_toplevel_manager_v1_interface, v);
        if (a->toplevel_manager) {
            zwlr_foreign_toplevel_manager_v1_add_listener(
                a->toplevel_manager, &foreign_toplevel_manager_listener, a);
        }
    } else if (!strcmp(interface, wl_output_interface.name)) {
        /* Keep slots stable: live wl_output listeners point at &outputs[i]. */
        int slot = -1;
        for (int i = 0; i < MAX_OUTPUTS; i++) {
            if (!a->outputs[i].used) {
                slot = i;
                break;
            }
        }
        if (slot >= 0) {
            struct wl_output *out = wl_registry_bind(registry, name,
                                                     &wl_output_interface,
                                                     version < 2 ? version : 2);
            if (out) {
                Output *o = &a->outputs[slot];
                o->output = out;
                o->registry_name = name;
                o->scale = 1;
                o->used = 1;
                o->entered = 0;
                o->app = a;
                wl_output_add_listener(out, &output_listener, o);
                if (slot + 1 > a->output_count)
                    a->output_count = slot + 1;
            }
        }
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
    (void)registry;
    App *a = data;
    if (!a)
        return;

    for (int i = 0; i < a->output_count; i++) {
        Output *o = &a->outputs[i];
        if (!o->used || o->registry_name != name)
            continue;
        if (o->output)
            wl_output_destroy(o->output);
        memset(o, 0, sizeof *o);
        recalc_scale(a);
        a->dirty = 1;
        return;
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static int wayland_start(App *a)
{
    a->display = wl_display_connect(NULL);
    if (!a->display) {
        fprintf(stderr, "jtsettings: cannot connect to a Wayland display\n");
        return 0;
    }

    a->registry = wl_display_get_registry(a->display);
    wl_registry_add_listener(a->registry, &registry_listener, a);
    if (wl_display_roundtrip(a->display) < 0 ||
        wl_display_roundtrip(a->display) < 0) {
        fprintf(stderr, "jtsettings: Wayland roundtrip failed\n");
        return 0;
    }

    if (!a->compositor || !a->shm || !a->wm_base) {
        fprintf(stderr, "jtsettings: missing wl_compositor, wl_shm "
                        "or xdg_wm_base\n");
        return 0;
    }
    if (!a->has_argb) {
        fprintf(stderr, "jtsettings: compositor has no ARGB8888 shm format\n");
        return 0;
    }

    a->surface = wl_compositor_create_surface(a->compositor);
    if (!a->surface) {
        fprintf(stderr, "jtsettings: cannot create wl_surface\n");
        return 0;
    }
    wl_surface_add_listener(a->surface, &surface_listener, a);

    if (wl_surface_get_version(a->surface) < 3) {
        a->scale = 1;
        a->fixed_scale = 1;
    }

    a->xdg_surface = xdg_wm_base_get_xdg_surface(a->wm_base, a->surface);
    xdg_surface_add_listener(a->xdg_surface, &xdg_surface_listener, a);
    a->toplevel = xdg_surface_get_toplevel(a->xdg_surface);
    xdg_toplevel_add_listener(a->toplevel, &toplevel_listener, a);
    xdg_toplevel_set_title(a->toplevel, "JTLab Settings");
    xdg_toplevel_set_app_id(a->toplevel, "jtsettings");
    xdg_toplevel_set_min_size(a->toplevel, (int)DESIGN_W, (int)DESIGN_H);

    if (a->state_w > 0 && a->state_h > 0) {
        if (a->state_w < (int)DESIGN_W)
            a->state_w = (int)DESIGN_W;
        if (a->state_h < (int)DESIGN_H)
            a->state_h = (int)DESIGN_H;
        WIN_W = a->state_w;
        WIN_H = a->state_h;
    }
    if (a->state_maximized)
        xdg_toplevel_set_maximized(a->toplevel);

    if (a->deco_manager) {
        a->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
            a->deco_manager, a->toplevel);
        zxdg_toplevel_decoration_v1_add_listener(
            a->decoration, &decoration_listener, a);
        /* Ask jtlabwc to draw its own titlebar and borders. */
        zxdg_toplevel_decoration_v1_set_mode(
            a->decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }

    if (a->icon_manager) {
        struct xdg_toplevel_icon_v1 *icon =
            xdg_toplevel_icon_manager_v1_create_icon(a->icon_manager);
        if (icon) {
            xdg_toplevel_icon_v1_set_name(icon, "jtsettings");
            xdg_toplevel_icon_manager_v1_set_icon(a->icon_manager,
                                                  a->toplevel, icon);
            xdg_toplevel_icon_v1_destroy(icon);
        }
    }

    if (wl_surface_get_version(a->surface) >= 3)
        wl_surface_set_buffer_scale(a->surface, a->scale);
    wl_surface_commit(a->surface);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Live control socket                                                */
/* ------------------------------------------------------------------ */

#define CTL_MAX_ACCEPTS 16
#define CTL_MAX_READS   64

static void ctl_path(char *buf, size_t n)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    int len;
    if (rt && rt[0])
        len = snprintf(buf, n, "%s/jtsettings-ctl.sock", rt);
    else
        len = snprintf(buf, n, "/tmp/jtsettings-ctl-%u.sock",
                       (unsigned)getuid());
    if (len < 0 || (size_t)len >= n)
        buf[0] = '\0';
}

/* Remove only the socket path we actually created, never another instance's
 * replacement. */
static void ctl_unlink_owned(const char *path, dev_t dev, ino_t ino)
{
    if (!path || !*path || (dev == 0 && ino == 0))
        return;
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISSOCK(st.st_mode) &&
        st.st_dev == dev && st.st_ino == ino)
        unlink(path);
}

static int page_from_name(const char *s)
{
    if (!s || !*s)
        return -1;

    if (!strcasecmp(s, "keybindings") || !strcasecmp(s, "keys") ||
        !strcasecmp(s, "shortcuts"))
        return PAGE_KEYBINDINGS;
    if (!strcasecmp(s, "startup") || !strcasecmp(s, "startup apps") ||
        !strcasecmp(s, "autostart"))
        return PAGE_STARTUP;
    if (!strcasecmp(s, "apps") || !strcasecmp(s, "default apps") ||
        !strcasecmp(s, "defaults") || !strcasecmp(s, "default"))
        return PAGE_DEFAULT_APPS;
    if (!strcasecmp(s, "rules") || !strcasecmp(s, "window rules"))
        return PAGE_RULES;
    if (!strcasecmp(s, "input") || !strcasecmp(s, "mouse") ||
        !strcasecmp(s, "keyboard"))
        return PAGE_INPUT;
    if (!strcasecmp(s, "displays") || !strcasecmp(s, "display") ||
        !strcasecmp(s, "outputs"))
        return PAGE_DISPLAYS;
    if (!strcasecmp(s, "session") || !strcasecmp(s, "power"))
        return PAGE_SESSION;
    if (!strcasecmp(s, "profile") || !strcasecmp(s, "weather") ||
        !strcasecmp(s, "avatar"))
        return PAGE_PROFILE;
    if (!strcasecmp(s, "about") || !strcasecmp(s, "info"))
        return PAGE_ABOUT;

    if (s[0] >= '1' && s[0] <= '9' && s[1] == '\0')
        return s[0] - '1';
    return -1;
}

static void ctl_handle_line(App *a, char *line)
{
    while (*line == ' ' || *line == '\t')
        line++;

    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t' ||
                     line[n - 1] == '\r' || line[n - 1] == '\n'))
        line[--n] = '\0';

    if (!strcmp(line, "quit")) {
        a->quit = 1;
        return;
    }

    if (!strcmp(line, "apply")) {
        displays_apply();
        a->dirty = 1;
        return;
    }

    if (!strcmp(line, "reset")) {
        displays_sync_all();
        g_ui.mode_picker_open = 0;
        g_ui.mode_picker_head = -1;
        g_ui.pos_editor_open = 0;
        g_ui.pos_layout_valid = 0;
        g_ui.disp_drag = -1;
        ui_toast("Display changes reset");
        a->dirty = 1;
        return;
    }

    if (!strcmp(line, "dump")) {
        fprintf(stderr,
                "ui: page=%d pos_editor=%d mode_picker=%d drag=%d "
                "scale=%.6f offx=%.3f offy=%.3f ox=%.3f oy=%.3f scroll=%d\n",
                g_ui.page, g_ui.pos_editor_open, g_ui.mode_picker_open,
                g_ui.disp_drag, g_ui.pos_scale, g_ui.pos_offx,
                g_ui.pos_offy, g_ui.disp_drag_ox, g_ui.disp_drag_oy,
                g_ui.disp_scroll);

        for (int i = 0; i < MAX_HEADS; i++) {
            Head *h = &g_heads[i];
            if (!h->used)
                continue;
            fprintf(stderr,
                    "head %d: %s | %s | enabled=%d pos=%d,%d scale=%.2f "
                    "transform=%d mode=%d/%d cur=%dx%d@%d mHz modes=%d "
                    "pending=%d mode=%d scale=%.2f transform=%d pos=%d,%d "
                    "dirty=%d\n",
                    i, h->name, h->description, h->enabled, h->x, h->y,
                    h->scale, h->transform, h->p_mode, h->mode_count,
                    h->cur_w, h->cur_h, h->cur_refresh, h->mode_count,
                    h->p_enabled, h->p_mode, h->p_scale, h->p_transform,
                    h->p_x, h->p_y, h->dirty);
        }
        return;
    }

    if (strncmp(line, "accent", 6) == 0 &&
        (line[6] == ' ' || line[6] == '\t')) {
        const char *arg = line + 6;
        while (*arg == ' ' || *arg == '\t')
            arg++;
        unsigned rgb;
        if (accent_parse(arg, &rgb)) {
            accent_set(rgb);
            a->dirty = 1;
        }
        return;
    }

    if (strncmp(line, "page", 4) == 0 &&
        (line[4] == ' ' || line[4] == '\t')) {
        const char *arg = line + 4;
        while (*arg == ' ' || *arg == '\t')
            arg++;
        int page = page_from_name(arg);
        if (page >= 0 && page < PAGE_COUNT) {
            ui_set_page(page);
            a->dirty = 1;
        }
    }
}

static void ctl_close_client(App *a)
{
    if (a->ctl_client_fd >= 0) {
        close(a->ctl_client_fd);
        a->ctl_client_fd = -1;
    }
    a->ctl_len = 0;
}

static void ctl_process(App *a, int flush)
{
    size_t start = 0;
    for (size_t i = 0; i < a->ctl_len; i++) {
        if (a->ctl_buf[i] == '\n') {
            a->ctl_buf[i] = '\0';
            ctl_handle_line(a, a->ctl_buf + start);
            start = i + 1;
        }
    }
    if (start > 0) {
        memmove(a->ctl_buf, a->ctl_buf + start, a->ctl_len - start);
        a->ctl_len -= start;
    }
    if (flush && a->ctl_len > 0) {
        a->ctl_buf[a->ctl_len] = '\0';
        ctl_handle_line(a, a->ctl_buf);
        a->ctl_len = 0;
    }
}

static void ctl_read_client(App *a)
{
    char buf[128];
    /* Bound one poll cycle so a client streaming data cannot starve Wayland
     * input.  Anything left over is picked up by the next POLLIN. */
    for (int reads = 0; reads < CTL_MAX_READS; reads++) {
        ssize_t n = read(a->ctl_client_fd, buf, sizeof(buf));
        if (n > 0) {
            if (a->ctl_len >= sizeof(a->ctl_buf) - 1) {
                ctl_close_client(a);
                return;
            }
            size_t room = sizeof(a->ctl_buf) - 1 - a->ctl_len;
            if ((size_t)n > room) {
                ctl_close_client(a);
                return;
            }
            memcpy(a->ctl_buf + a->ctl_len, buf, (size_t)n);
            a->ctl_len += (size_t)n;
            ctl_process(a, 0);
            continue;
        }
        if (n == 0) {
            ctl_process(a, 1);
            ctl_close_client(a);
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ctl_process(a, 1);
            ctl_close_client(a);
            return;
        }
        return;
    }
}

static void ctl_accept(App *a)
{
    for (int accepted = 0; accepted < CTL_MAX_ACCEPTS; accepted++) {
        int fd = accept4(a->ctl_fd, NULL, NULL,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (a->ctl_client_fd >= 0) {
            close(fd);
            continue;
        }
        a->ctl_client_fd = fd;
        a->ctl_len = 0;
    }
}

static int ctl_start(App *a)
{
    char path[256];
    ctl_path(path, sizeof(path));

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "jtsettings: control socket path too long: %s\n",
                path);
        return 0;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        perror("jtsettings: control socket");
        return 0;
    }
    /* A Unix socket's mode is fixed at bind() time.  Without this, a missing
     * XDG_RUNTIME_DIR leaves a world-connectable control socket in /tmp. */
    mode_t old_umask = umask(0077);
    int bound = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    umask(old_umask);
    if (bound < 0) {
        fprintf(stderr, "jtsettings: cannot bind %s: %s\n",
                path, strerror(errno));
        close(fd);
        return 0;
    }

    /* Remember the path's real filesystem identity for cleanup. */
    struct stat bound_st;
    if (lstat(path, &bound_st) != 0) {
        fprintf(stderr, "jtsettings: cannot stat bound socket %s: %s\n",
                path, strerror(errno));
        close(fd);
        return 0;
    }
    if (!S_ISSOCK(bound_st.st_mode)) {
        fprintf(stderr, "jtsettings: bound path is not a socket: %s\n", path);
        close(fd);
        return 0;
    }
    a->ctl_dev = bound_st.st_dev;
    a->ctl_ino = bound_st.st_ino;

    if (listen(fd, 4) < 0) {
        fprintf(stderr, "jtsettings: cannot listen on %s: %s\n",
                path, strerror(errno));
        close(fd);
        ctl_unlink_owned(path, a->ctl_dev, a->ctl_ino);
        return 0;
    }

    a->ctl_fd = fd;
    return 1;
}

static void ctl_cleanup(App *a)
{
    ctl_close_client(a);
    if (a->ctl_fd >= 0) {
        /* Another instance may have rebound our path after it was removed.
         * Only unlink when it still refers to the socket we bound. */
        char path[256];
        ctl_path(path, sizeof(path));
        ctl_unlink_owned(path, a->ctl_dev, a->ctl_ino);
        close(a->ctl_fd);
        a->ctl_fd = -1;
    }
}

/* The XDG portal and the geocoding request live in a GLib main context.  We
 * still own the Wayland event loop, so borrow GLib's poll fds for one poll()
 * and dispatch its sources alongside Wayland/control-socket events. */
static int g_gl_max_priority;
static GPollFD *g_gl_pfds;
static int g_gl_n_pfds, g_gl_pfds_cap, g_gl_have_prepare;

static void
glib_pump_prepare(int *timeout_ms)
{
    int gl_timeout = -1;
    g_gl_have_prepare = 0;
    if (!g_glctx || !g_main_context_acquire(g_glctx))
        return;

    g_main_context_prepare(g_glctx, &g_gl_max_priority);
    int need = g_main_context_query(g_glctx, g_gl_max_priority, &gl_timeout,
                                    NULL, 0);
    if (need > g_gl_pfds_cap) {
        g_gl_pfds = g_renew(GPollFD, g_gl_pfds, (gsize)need);
        g_gl_pfds_cap = need;
    }
    g_gl_n_pfds = g_main_context_query(g_glctx, g_gl_max_priority, &gl_timeout,
                                       g_gl_pfds, g_gl_pfds_cap);
    if (gl_timeout >= 0 && (*timeout_ms < 0 || gl_timeout < *timeout_ms))
        *timeout_ms = gl_timeout;
    g_gl_have_prepare = 1;
}

static void
glib_pump_dispatch(void)
{
    if (!g_gl_have_prepare)
        return;
    g_main_context_check(g_glctx, g_gl_max_priority, g_gl_pfds, g_gl_n_pfds);
    g_main_context_dispatch(g_glctx);
    g_main_context_release(g_glctx);
    g_gl_have_prepare = 0;
}

/* Finish an acquired prepare cycle without running callbacks.  Used on fatal
 * exits before dispatch so the context is not left locked by this thread. */
static void
glib_pump_cancel(void)
{
    if (!g_glctx || !g_gl_have_prepare)
        return;
    for (int i = 0; i < g_gl_n_pfds; i++)
        g_gl_pfds[i].revents = 0;
    g_main_context_check(g_glctx, g_gl_max_priority, g_gl_pfds, g_gl_n_pfds);
    g_main_context_release(g_glctx);
    g_gl_have_prepare = 0;
}

/* Returns < 0 on a fatal display error, 0 otherwise. */
static int pump_events(App *a, int timeout_ms)
{
    if (wl_display_dispatch_pending(a->display) < 0)
        return -1;

    /* GLib may replace an infinite wait with one of its own timeouts; keep
     * the caller's original intent for the dirty-repaint shortcut. */
    int block_forever = timeout_ms < 0;
    glib_pump_prepare(&timeout_ms);

    /* Dispatching may have applied a configure that marked us dirty.  When
     * the caller asked to block indefinitely, return immediately so the
     * resize repaint is not held until some unrelated event arrives. */
    if (block_forever && a->dirty) {
        glib_pump_dispatch();
        return 0;
    }

    int want_write = 0;
    if (wl_display_flush(a->display) < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            glib_pump_cancel();
            return -1;
        }
        want_write = 1;
    }

#define MAX_POLL_FDS 32
    struct pollfd pfd[MAX_POLL_FDS];
    int gl_map[MAX_POLL_FDS];
    for (int i = 0; i < MAX_POLL_FDS; i++)
        gl_map[i] = -1;

    pfd[0].fd = wl_display_get_fd(a->display);
    pfd[0].events = POLLIN | (want_write ? POLLOUT : 0);
    pfd[0].revents = 0;

    int nfds = 1;
    int ctl_idx = -1;
    int ctl_client_idx = -1;
    if (a->ctl_fd >= 0) {
        ctl_idx = nfds;
        pfd[nfds].fd = a->ctl_fd;
        pfd[nfds].events = POLLIN;
        pfd[nfds].revents = 0;
        nfds++;
    }
    if (a->ctl_client_fd >= 0) {
        ctl_client_idx = nfds;
        pfd[nfds].fd = a->ctl_client_fd;
        pfd[nfds].events = POLLIN;
        pfd[nfds].revents = 0;
        nfds++;
    }

    for (int i = 0; i < g_gl_n_pfds && nfds < MAX_POLL_FDS; i++) {
        if (g_gl_pfds[i].fd < 0)
            continue;
        gl_map[nfds] = i;
        pfd[nfds].fd = g_gl_pfds[i].fd;
        pfd[nfds].events = (short)g_gl_pfds[i].events;
        pfd[nfds].revents = 0;
        nfds++;
    }

    int pr = poll(pfd, (nfds_t)nfds, timeout_ms);
    for (int i = 0; i < nfds; i++)
        if (gl_map[i] >= 0)
            g_gl_pfds[gl_map[i]].revents = (gushort)pfd[i].revents;
    glib_pump_dispatch();
    if (pr < 0)
        return (errno == EINTR) ? 0 : -1;
    if (pr == 0)
        return 0;

    if (pfd[0].revents & POLLOUT) {
        if (wl_display_flush(a->display) < 0 && errno != EAGAIN)
            return -1;
    }
    if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL))
        return -1;
    if (pfd[0].revents & POLLIN) {
        if (wl_display_dispatch(a->display) < 0)
            return -1;
    }
    if (ctl_idx >= 0 && (pfd[ctl_idx].revents & POLLIN))
        ctl_accept(a);
    if (ctl_client_idx >= 0 && pfd[ctl_client_idx].revents)
        ctl_read_client(a);
    return 0;
}

static volatile sig_atomic_t g_signal_quit;

static void on_signal(int sig)
{
    (void)sig;
    g_signal_quit = 1;
}

static void app_cleanup(App *a)
{
    ctl_cleanup(a);

    if (a->pending_config) {
        zwlr_output_configuration_v1_destroy(a->pending_config);
        a->pending_config = NULL;
    }
    if (a->output_manager) {
        for (int i = 0; i < MAX_HEADS; i++) {
            Head *h = &g_heads[i];
            for (int j = 0; j < h->mode_count; j++)
                if (h->modes[j].obj)
                    zwlr_output_mode_v1_destroy(h->modes[j].obj);
            if (h->head)
                zwlr_output_head_v1_destroy(h->head);
        }
        heads_clear();
        zwlr_output_manager_v1_destroy(a->output_manager);
        a->output_manager = NULL;
    }

    foreign_toplevel_clear();
    if (a->toplevel_manager) {
        zwlr_foreign_toplevel_manager_v1_destroy(a->toplevel_manager);
        a->toplevel_manager = NULL;
    }

    if (a->frame_cb) {
        wl_callback_destroy(a->frame_cb);
        a->frame_cb = NULL;
    }
    if (a->keyboard)
        wl_keyboard_destroy(a->keyboard);
    if (a->cursor_shape_device) {
        wp_cursor_shape_device_v1_destroy(a->cursor_shape_device);
        a->cursor_shape_device = NULL;
    }
    if (a->pointer)
        wl_pointer_destroy(a->pointer);
    if (a->cursor_shape_manager)
        wp_cursor_shape_manager_v1_destroy(a->cursor_shape_manager);
    if (a->decoration)
        zxdg_toplevel_decoration_v1_destroy(a->decoration);
    if (a->toplevel)
        xdg_toplevel_destroy(a->toplevel);
    if (a->xdg_surface)
        xdg_surface_destroy(a->xdg_surface);
    if (a->surface)
        wl_surface_destroy(a->surface);
    if (a->seat)
        wl_seat_destroy(a->seat);
    if (a->deco_manager)
        zxdg_decoration_manager_v1_destroy(a->deco_manager);
    if (a->icon_manager)
        xdg_toplevel_icon_manager_v1_destroy(a->icon_manager);
    if (a->wm_base)
        xdg_wm_base_destroy(a->wm_base);
    if (a->shm)
        wl_shm_destroy(a->shm);
    if (a->compositor)
        wl_compositor_destroy(a->compositor);

    for (int i = 0; i < a->output_count; i++) {
        if (a->outputs[i].output)
            wl_output_destroy(a->outputs[i].output);
    }
    if (a->registry)
        wl_registry_destroy(a->registry);

    if (a->xkb_state)
        xkb_state_unref(a->xkb_state);
    if (a->xkb_keymap)
        xkb_keymap_unref(a->xkb_keymap);
    if (a->xkb_ctx)
        xkb_context_unref(a->xkb_ctx);

    if (a->display) {
        for (int i = 0; i < NBUFS; i++) {
            ShmBuf *b = &a->bufs[i];
            if (b->buffer && !b->busy) {
                wl_buffer_destroy(b->buffer);
                b->buffer = NULL;
            }
        }
        wl_display_flush(a->display);
        wl_display_disconnect(a->display);
        a->display = NULL;
    }

    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (b->data) {
            munmap(b->data, b->size);
            b->data = NULL;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Screenshot mode (also handy as a preview)                          */
/* ------------------------------------------------------------------ */

static int write_screenshot(const char *path, int page)
{
    cairo_surface_t *surf = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, (int)WIN_W, (int)WIN_H);
    cairo_t *cr = cairo_create(surf);

    g_ui.page = page;
    g_ui.t = 1.2;
    paint_content(cr);

    cairo_destroy(cr);
    cairo_status_t st = cairo_surface_write_to_png(surf, path);
    cairo_surface_destroy(surf);

    if (st != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "jtsettings: cannot write %s: %s\n", path,
                cairo_status_to_string(st));
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [#RRGGBB] [--accent #RRGGBB] [--page NAME]\n"
        "       %s --screenshot FILE [--page NAME]\n"
        "\n"
        "Pages: keybindings, startup, apps, rules, input,\n"
        "       displays, session, profile, about\n",
        argv0, argv0);
}

int main(int argc, char **argv)
{
    /* ui_init() calls xdg-mime via popen(), and glib_services_init() may
     * spawn dbus-daemon.  Reset an inherited SIG_IGN before either runs. */
    signal(SIGCHLD, SIG_DFL);

    if (!accent_cli(&argc, argv)) {
        usage(argv[0]);
        return 2;
    }

    int page = PAGE_KEYBINDINGS;
    const char *screenshot = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--page") && i + 1 < argc) {
            int p = page_from_name(argv[++i]);
            if (p < 0) {
                fprintf(stderr, "%s: unknown page '%s'\n", argv[0], argv[i]);
                return 2;
            }
            page = p;
        } else if (!strncmp(argv[i], "--page=", 7)) {
            int p = page_from_name(argv[i] + 7);
            if (p < 0) {
                fprintf(stderr, "%s: unknown page '%s'\n", argv[0],
                        argv[i] + 7);
                return 2;
            }
            page = p;
        } else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) {
            screenshot = argv[++i];
        } else if (!strncmp(argv[i], "--screenshot=", 13)) {
            screenshot = argv[i] + 13;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "%s: unknown argument '%s'\n", argv[0], argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    ui_init();
    g_ui.page = page;

    if (screenshot)
        return write_screenshot(screenshot, page) ? 0 : 1;

    glib_services_init();

    App a;
    memset(&a, 0, sizeof a);
    g_app = &a;
    g_output_manager_available = 0;
    memset(g_heads, 0, sizeof g_heads);
    a.scale = 1;
    a.ctl_fd = -1;
    a.ctl_client_fd = -1;
    state_load(&a);
    a.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    if (!wayland_start(&a)) {
        app_cleanup(&a);
        glib_services_cleanup();
        return 1;
    }

    ctl_start(&a);

    struct sigaction sa = { 0 };
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    a.t0 = mono_sec();

    while (!a.quit && !g_signal_quit) {
        double now = mono_sec();
        g_ui.t = now - a.t0;
        if (g_profile_dirty) {
            a.dirty = 1;
            g_profile_dirty = 0;
        }

        int animating = g_ui.startup_adding || g_ui.terminal_editing ||
                        g_ui.wx_editing || g_ui.wx_query_editing;
        if (!animating && g_ui.toast[0]) {
            double age = now - g_ui.toast_t;
            if (age >= 0.0 && age < 3.0)
                animating = 1;
        }

        if (a.configured && (a.dirty || animating) && !a.frame_cb) {
            app_draw(&a);
            a.dirty = 0;
        }

        /* A pending frame callback wakes us at the next refresh.  While
         * animating we still cap the wait so a stalled compositor cannot
         * freeze the UI; when truly idle we block until input or a control
         * command arrives. */
        int timeout;
        if (!a.configured || animating || a.dirty)
            timeout = 100;
        else
            timeout = -1;

        if (pump_events(&a, timeout) < 0)
            break;
    }

    if (a.configured)
        state_save(&a);
    app_cleanup(&a);
    glib_services_cleanup();
    return 0;
}
