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
 *
 * This is a GUI preview.  Every control is drawn and reacts locally, but
 * nothing is written to jtlabwc's configuration yet.  The left panel mirrors
 * the areas a real settings app would cover: keybindings, startup apps,
 * window rules, input, displays, session behaviour and an about page.
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

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-client-protocol.h"
#include "xdg-toplevel-icon-client-protocol.h"
#include "cursor-shape-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
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

#define WIN_W 1080.0
#define WIN_H 720.0

#define SIDEBAR_W 252.0
#define HEADER_H  84.0

#define CONTENT_X (SIDEBAR_W + 36.0)
#define CONTENT_W (WIN_W - CONTENT_X - 36.0)

#define FONT_FAMILY "Liberation"

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
    PAGE_RULES,
    PAGE_INPUT,
    PAGE_DISPLAYS,
    PAGE_SESSION,
    PAGE_ABOUT,
    PAGE_COUNT
};

static const char *PAGE_NAMES[PAGE_COUNT] = {
    "Keybindings", "Startup Apps", "Window Rules", "Input",
    "Displays", "Session", "About",
};

static const char *PAGE_SUBS[PAGE_COUNT] = {
    "Every shortcut jtlabwc listens for",
    "Programs launched together with your session",
    "Match windows and shape how they appear",
    "Pointer, touchpad and keyboard behaviour",
    "Outputs, resolution, refresh rate and scale",
    "Idle, lock and power commands",
    "Version and build information",
};

enum {
    TOG_NATURAL,
    TOG_TAP,
    TOG_DRAG,
    TOG_DWT,
    TOG_LEFT,
    TOG_APP_JTLAB,
    TOG_APP_FOOT,
    TOG_APP_WALL,
    TOG_APP_NOTIFY,
    TOG_APP_CLIP,
    TOG_APP_KEYRING,
    TOG_RULE_PIP,
    TOG_RULE_TERM,
    TOG_RULE_WEB,
    TOG_MOMENTUM,
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
    SL_COUNT
};

enum {
    SEG_SCROLL,
    SEG_REFRESH_A,
    SEG_SCALE_A,
    SEG_ORIENT_A,
    SEG_REFRESH_B,
    SEG_SCALE_B,
    SEG_ORIENT_B,
    SEG_COUNT
};

enum {
    HIT_NONE,
    HIT_NAV,
    HIT_TOGGLE,
    HIT_SLIDER,
    HIT_SEGMENT,
    HIT_ROW,
    HIT_BUTTON
};

enum {
    BTN_ADD_BINDING,
    BTN_ADD_START,
    BTN_NEW_RULE,
    BTN_DOCS,
    BTN_UPDATE,
    BTN_LOGOUT,
    BTN_REBOOT,
    BTN_SHUTDOWN
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
    int page;
    double t;
    int toggles[TOG_COUNT];
    double sliders[SL_COUNT];
    int segments[SEG_COUNT];
    int selected[PAGE_COUNT];
    int hover;
    int pressed;
    double pointer_x, pointer_y;
    char toast[96];
    double toast_t;
} Ui;

static Ui g_ui;

static void ui_toast(const char *msg)
{
    snprintf(g_ui.toast, sizeof g_ui.toast, "%s", msg);
    g_ui.toast_t = mono_sec();
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

    g_ui.toggles[TOG_NATURAL] = 1;
    g_ui.toggles[TOG_TAP] = 1;
    g_ui.toggles[TOG_DRAG] = 1;
    g_ui.toggles[TOG_DWT] = 1;
    g_ui.toggles[TOG_LEFT] = 0;
    g_ui.toggles[TOG_APP_JTLAB] = 1;
    g_ui.toggles[TOG_APP_FOOT] = 1;
    g_ui.toggles[TOG_APP_WALL] = 1;
    g_ui.toggles[TOG_APP_NOTIFY] = 1;
    g_ui.toggles[TOG_APP_CLIP] = 0;
    g_ui.toggles[TOG_APP_KEYRING] = 1;
    g_ui.toggles[TOG_RULE_PIP] = 1;
    g_ui.toggles[TOG_RULE_TERM] = 1;
    g_ui.toggles[TOG_RULE_WEB] = 0;
    g_ui.toggles[TOG_MOMENTUM] = 1;
    g_ui.toggles[TOG_IDLE_LOCK] = 1;
    g_ui.toggles[TOG_DIM] = 1;
    g_ui.toggles[TOG_LOCK_SUSPEND] = 1;
    g_ui.toggles[TOG_RESTORE] = 1;
    g_ui.toggles[TOG_LAST_WS] = 0;
    g_ui.toggles[TOG_PRIMARY_A] = 1;
    g_ui.toggles[TOG_PRIMARY_B] = 0;

    g_ui.sliders[SL_POINTER] = 0.52;
    g_ui.sliders[SL_REPEAT] = 0.58;
    g_ui.sliders[SL_DELAY] = 0.35;
    g_ui.sliders[SL_START_DELAY] = 0.20;
    g_ui.sliders[SL_IDLE] = 0.45;

    g_ui.segments[SEG_SCROLL] = 0;
    g_ui.segments[SEG_REFRESH_A] = 1;
    g_ui.segments[SEG_SCALE_A] = 0;
    g_ui.segments[SEG_ORIENT_A] = 0;
    g_ui.segments[SEG_REFRESH_B] = 0;
    g_ui.segments[SEG_SCALE_B] = 0;
    g_ui.segments[SEG_ORIENT_B] = 0;

    for (int i = 0; i < PAGE_COUNT; i++)
        g_ui.selected[i] = 0;
}

static int add_hit(int kind, int a, int b,
                   double x, double y, double w, double h)
{
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

static void paint_sidebar(cairo_t *cr, double now)
{
    (void)now;
    rgba(cr, C_SIDEBAR, 1.0);
    cairo_rectangle(cr, 0, 0, SIDEBAR_W, WIN_H);
    cairo_fill(cr);

    cairo_pattern_t *glow = cairo_pattern_create_radial(
        36.0, 30.0, 8.0, 36.0, 30.0, 260.0);
    cairo_pattern_add_color_stop_rgba(glow, 0.0,
        R_(C_ACCENT), G_(C_ACCENT), B_(C_ACCENT), 0.075);
    cairo_pattern_add_color_stop_rgba(glow, 0.55,
        R_(C_ACCENT), G_(C_ACCENT), B_(C_ACCENT), 0.018);
    cairo_pattern_add_color_stop_rgba(glow, 1.0,
        R_(C_ACCENT), G_(C_ACCENT), B_(C_ACCENT), 0.0);
    cairo_set_source(cr, glow);
    cairo_paint(cr);
    cairo_pattern_destroy(glow);

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

    paint_divider(cr, 22.0, 592.0, SIDEBAR_W - 44.0);

    text_draw(cr, "PREVIEW BUILD", 8.0, 600, C_FAINT, 0.9,
              24.0, 610.0, ALIGN_LEFT, 1.7);
    text_draw(cr, "jtsettings 0.1.0", 10.5, 600, 0xD6D6D6, 0.92,
              24.0, 628.0, ALIGN_LEFT, 0.1);

    rgba(cr, C_ACCENT, 0.95);
    cairo_arc(cr, 29.0, 658.0, 4.0, 0.0, 2.0 * PI);
    cairo_fill(cr);
    char hex[16];
    snprintf(hex, sizeof hex, "#%06X", C_ACCENT);
    text_draw(cr, hex, 9.5, 500, C_DIM, 0.9,
              40.0, 652.0, ALIGN_LEFT, 0.6);

    text_draw(cr, "↑ ↓  switch pages", 8.5, 400, C_FAINT, 0.8,
              24.0, 678.0, ALIGN_LEFT, 0.2);

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

static void paint_footer(cairo_t *cr)
{
    paint_divider(cr, SIDEBAR_W, 690.0, WIN_W - SIDEBAR_W);

    double age = mono_sec() - g_ui.toast_t;
    const char *msg = "PREVIEW · changes are not saved";
    unsigned col = C_FAINT;
    double alpha = 0.9;

    if (g_ui.toast[0] && age >= 0.0 && age < 3.0) {
        msg = g_ui.toast;
        col = C_ACCENT;
        alpha = age < 0.25 ? age / 0.25 : (age > 2.4 ? (3.0 - age) / 0.6 : 1.0);
        alpha = clampd(alpha, 0.0, 1.0) * 0.95;
    }

    rgba(cr, col, alpha);
    cairo_arc(cr, CONTENT_X + 3.0, 704.0, 2.5, 0.0, 2.0 * PI);
    cairo_fill(cr);
    text_draw(cr, msg, 9.0, 500, col, alpha,
              CONTENT_X + 12.0, 698.0, ALIGN_LEFT, 0.35);

    char counter[32];
    snprintf(counter, sizeof counter, "%02d / %02d", g_ui.page + 1,
             PAGE_COUNT);
    text_draw(cr, counter, 9.0, 600, C_FAINT, 0.85,
              WIN_W - 36.0, 698.0, ALIGN_RIGHT, 0.8);
    text_draw(cr, "↑ ↓ · Esc to quit", 9.0, 400, C_FAINT, 0.7,
              WIN_W - 96.0, 698.0, ALIGN_RIGHT, 0.2);
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

static const KeybindRow KEYBINDS[] = {
    { "Open terminal",        "Session",   { "Super", "Return" }, 2 },
    { "Close window",         "Window",    { "Super", "Q" }, 2 },
    { "Toggle maximize",      "Window",    { "Super", "M" }, 2 },
    { "Move window left",     "Window",    { "Super", "Ctrl", "Left" }, 3 },
    { "Workspace 1",          "Workspace", { "Super", "1" }, 2 },
    { "Next workspace",       "Workspace", { "Super", "Right" }, 2 },
    { "Application launcher", "Session",   { "Super", "Space" }, 2 },
    { "Lock screen",          "Session",   { "Super", "L" }, 2 },
    { "Screenshot region",    "Media",     { "Print" }, 1 },
};
#define KEYBIND_COUNT ((int)(sizeof KEYBINDS / sizeof KEYBINDS[0]))

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

    static const char *const filters[] = { "All", "Window", "Workspace", "Media" };
    double total = 0.0;
    for (int i = 0; i < 4; i++) {
        total += chip_width(cr, filters[i]);
        if (i < 3) total += 8.0;
    }
    double cx = WIN_W - 36.0 - total;
    for (int i = 0; i < 4; i++) {
        cx += paint_chip(cr, filters[i], cx, 110.0, i == 0) + 8.0;
    }

    double tx = CONTENT_X, ty = 152.0, tw = CONTENT_W, th = 466.0;
    paint_card(cr, tx, ty, tw, th);

    text_draw(cr, "ACTION", 8.0, 600, C_FAINT, 0.9,
              tx + 20.0, ty + 14.0, ALIGN_LEFT, 1.4);
    text_draw(cr, "CONTEXT", 8.0, 600, C_FAINT, 0.9,
              tx + 330.0, ty + 14.0, ALIGN_LEFT, 1.4);
    text_draw(cr, "SHORTCUT", 8.0, 600, C_FAINT, 0.9,
              tx + tw - 20.0, ty + 14.0, ALIGN_RIGHT, 1.4);
    paint_divider(cr, tx + 20.0, ty + 36.0, tw - 40.0);

    for (int i = 0; i < KEYBIND_COUNT; i++) {
        double ry = ty + 44.0 + i * 43.0;
        int idx = add_hit(HIT_ROW, i, 0, tx + 8.0, ry, tw - 16.0, 40.0);
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

        if (i < KEYBIND_COUNT - 1)
            paint_divider(cr, tx + 20.0, ry + 41.0, tw - 40.0);
    }

    paint_button(cr, CONTENT_X, 634.0, 150.0, 36.0,
                 "Add keybinding", 1, BTN_ADD_BINDING);
    paint_button(cr, CONTENT_X + 162.0, 634.0, 158.0, 36.0,
                 "Reset to defaults", 0, BTN_ADD_BINDING);
    text_draw(cr, "Click a row to select it", 9.0, 400, C_FAINT, 0.8,
              WIN_W - 36.0, 645.0, ALIGN_RIGHT, 0.2);
}

/* ------------------------------------------------------------------ */
/*  Page: Startup Apps                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    const char *command;
    const char *note;
    unsigned tint;
    int toggle;
} StartupRow;

static const StartupRow STARTUPS[] = {
    { "JTLab", "jtlab", "Desktop bar, popups and taskbar", 0xE8A03C,
      TOG_APP_JTLAB },
    { "foot server", "foot --server", "Shared terminal daemon", 0x36B4EB,
      TOG_APP_FOOT },
    { "jtwallpaperd", "jtwallpaperd --daemon", "Wallpaper and accent source",
      0x66B366, TOG_APP_WALL },
    { "jtnotify", "jtnotify --daemon", "Notification centre", 0xB07CE6,
      TOG_APP_NOTIFY },
    { "wl-clipboard", "wl-paste --watch cliphist store",
      "Clipboard history", 0xE06C9F, TOG_APP_CLIP },
    { "gnome-keyring", "gnome-keyring-daemon --start", "Secret storage",
      0xC9B458, TOG_APP_KEYRING },
};
#define STARTUP_COUNT ((int)(sizeof STARTUPS / sizeof STARTUPS[0]))

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

static void paint_mini_toggle(cairo_t *cr, double x, double y,
                              int on, int toggle_id)
{
    double tw = 38.0, th = 20.0;
    int idx = add_hit(HIT_TOGGLE, toggle_id, 0, x - 4.0, y - 4.0,
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

static void page_startup(cairo_t *cr)
{
    section_label(cr, "AUTOSTART", CONTENT_X, 102.0);
    text_draw(cr, "Applications launched when the jtlabwc session starts",
              9.5, 400, C_DIM, 0.75, CONTENT_X, 117.0, ALIGN_LEFT, 0.1);

    double tx = CONTENT_X, ty = 142.0, tw = CONTENT_W, th = 416.0;
    paint_card(cr, tx, ty, tw, th);

    text_draw(cr, "APPLICATION", 8.0, 600, C_FAINT, 0.9,
              tx + 20.0, ty + 14.0, ALIGN_LEFT, 1.4);
    text_draw(cr, "COMMAND", 8.0, 600, C_FAINT, 0.9,
              tx + 300.0, ty + 14.0, ALIGN_LEFT, 1.4);
    text_draw(cr, "ENABLED", 8.0, 600, C_FAINT, 0.9,
              tx + tw - 24.0, ty + 14.0, ALIGN_RIGHT, 1.4);
    paint_divider(cr, tx + 20.0, ty + 34.0, tw - 40.0);

    for (int i = 0; i < STARTUP_COUNT; i++) {
        double ry = ty + 42.0 + i * 61.0;
        int idx = add_hit(HIT_ROW, i, 0, tx + 8.0, ry, tw - 16.0, 58.0);
        int sel = g_ui.selected[PAGE_STARTUP] == i;
        int hot = hit_hover(idx);

        if (sel || hot) {
            rgba(cr, sel ? C_ACCENT : C_FG, sel ? 0.070 : 0.030);
            rounded_rect(cr, tx + 8.0, ry, tw - 16.0, 56.0, 7.0);
            cairo_fill(cr);
        }
        if (sel) {
            rgba(cr, C_ACCENT, 0.9);
            rounded_rect(cr, tx + 8.0, ry + 15.0, 3.0, 26.0, 1.5);
            cairo_fill(cr);
        }

        paint_app_icon(cr, tx + 24.0, ry + 12.0, 32.0,
                       STARTUPS[i].name, STARTUPS[i].tint);

        text_draw(cr, STARTUPS[i].name, 11.5, 600, C_FG, 0.95,
                  tx + 68.0, ry + 11.0, ALIGN_LEFT, 0.1);
        text_draw(cr, STARTUPS[i].note, 8.5, 400, C_DIM, 0.72,
                  tx + 68.0, ry + 29.0, ALIGN_LEFT, 0.1);
        text_draw(cr, STARTUPS[i].command, 9.0, 500, C_DIM, 0.85,
                  tx + 300.0, ry + 20.0, ALIGN_LEFT, 0.15);

        paint_mini_toggle(cr, tx + tw - 62.0, ry + 18.0,
                          g_ui.toggles[STARTUPS[i].toggle],
                          STARTUPS[i].toggle);

        if (i < STARTUP_COUNT - 1)
            paint_divider(cr, tx + 20.0, ry + 59.0, tw - 40.0);
    }

    double by = 580.0, bh = 84.0;
    paint_card(cr, CONTENT_X, by, 368.0, bh);
    char v[32];
    paint_slider_row(cr, CONTENT_X + 18.0, by + 14.0, 332.0,
                     "Launch delay",
                     slider_text(SL_START_DELAY,
                                 g_ui.sliders[SL_START_DELAY], v, sizeof v),
                     g_ui.sliders[SL_START_DELAY], SL_START_DELAY);
    text_draw(cr, "Wait before the first application starts", 8.5, 400,
              C_FAINT, 0.8, CONTENT_X + 18.0, by + 56.0, ALIGN_LEFT, 0.1);

    paint_dashed_button(cr, CONTENT_X + 388.0, by + 18.0, 368.0, 48.0,
                        "Add application", BTN_ADD_START);
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

    for (int i = 0; i < RULE_COUNT; i++) {
        double ry = 142.0 + i * 130.0;
        int idx = add_hit(HIT_ROW, i, 0, CONTENT_X, ry, CONTENT_W, 116.0);
        int sel = g_ui.selected[PAGE_RULES] == i;
        int hot = hit_hover(idx);

        paint_card(cr, CONTENT_X, ry, CONTENT_W, 116.0);
        if (sel) {
            rgba(cr, C_ACCENT, 0.55);
            cairo_set_line_width(cr, 1.2);
            rounded_rect(cr, CONTENT_X + 0.6, ry + 0.6, CONTENT_W - 1.2,
                         114.8, 9.4);
            cairo_stroke(cr);
        } else if (hot) {
            rgba(cr, C_FG, 0.20);
            cairo_set_line_width(cr, 1.0);
            rounded_rect(cr, CONTENT_X + 0.5, ry + 0.5, CONTENT_W - 1.0,
                         115.0, 9.5);
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

    paint_dashed_button(cr, CONTENT_X, 540.0, CONTENT_W, 52.0,
                        "New window rule", BTN_NEW_RULE);

    text_wrap(cr,
        "A finished app would read and write these rules in "
        "~/.config/labwc/rc.xml, then ask jtlabwc to reload. The preview "
        "keeps everything local.",
        8.8, 400, C_FAINT, 0.8, CONTENT_X, 612.0, CONTENT_W, 2);
}

/* ------------------------------------------------------------------ */
/*  Page: Input                                                        */
/* ------------------------------------------------------------------ */

static void page_input(cairo_t *cr)
{
    section_label(cr, "POINTER & TOUCHPAD", CONTENT_X, 102.0);
    section_label(cr, "KEYBOARD", CONTENT_X + 388.0, 102.0);

    char v[32];
    double cy = 122.0, ch = 300.0;

    paint_card(cr, CONTENT_X, cy, 368.0, ch);
    paint_card(cr, CONTENT_X + 388.0, cy, 368.0, ch);

    double lx = CONTENT_X + 18.0;
    text_draw(cr, "Pointer", 12.0, 600, 0xE8E8E8, 0.95,
              lx, cy + 14.0, ALIGN_LEFT, 0.1);

    paint_slider_row(cr, lx, cy + 44.0, 332.0, "Pointer speed",
                     slider_text(SL_POINTER, g_ui.sliders[SL_POINTER],
                                 v, sizeof v),
                     g_ui.sliders[SL_POINTER], SL_POINTER);
    paint_divider(cr, lx, cy + 88.0, 332.0);

    paint_toggle_row(cr, lx, cy + 98.0, 332.0, "Natural scrolling",
                     "Content follows your fingers",
                     g_ui.toggles[TOG_NATURAL], TOG_NATURAL);
    paint_toggle_row(cr, lx, cy + 140.0, 332.0, "Tap to click",
                     "Tap the touchpad to press",
                     g_ui.toggles[TOG_TAP], TOG_TAP);
    paint_toggle_row(cr, lx, cy + 182.0, 332.0, "Tap and drag",
                     "Drag with one finger held down",
                     g_ui.toggles[TOG_DRAG], TOG_DRAG);
    paint_toggle_row(cr, lx, cy + 224.0, 332.0, "Disable while typing",
                     "Ignore taps for a short moment",
                     g_ui.toggles[TOG_LOCK_SUSPEND], TOG_LOCK_SUSPEND);
    paint_toggle_row(cr, lx, cy + 266.0, 332.0, "Left-handed",
                     "Swap the primary mouse button",
                     g_ui.toggles[TOG_LEFT], TOG_LEFT);

    double rx = CONTENT_X + 388.0 + 18.0;
    text_draw(cr, "Keyboard", 12.0, 600, 0xE8E8E8, 0.95,
              rx, cy + 14.0, ALIGN_LEFT, 0.1);

    text_draw(cr, "Layout", 11.5, 500, 0xD2D2D2, 0.94,
              rx, cy + 44.0, ALIGN_LEFT, 0.1);
    paint_field(cr, rx, cy + 62.0, 332.0, 32.0,
                "English (US) · intl", 1);

    paint_slider_row(cr, rx, cy + 108.0, 332.0, "Repeat rate",
                     slider_text(SL_REPEAT, g_ui.sliders[SL_REPEAT],
                                 v, sizeof v),
                     g_ui.sliders[SL_REPEAT], SL_REPEAT);
    paint_slider_row(cr, rx, cy + 156.0, 332.0, "Repeat delay",
                     slider_text(SL_DELAY, g_ui.sliders[SL_DELAY],
                                 v, sizeof v),
                     g_ui.sliders[SL_DELAY], SL_DELAY);

    paint_divider(cr, rx, cy + 200.0, 332.0);

    text_draw(cr, "Compose key", 11.5, 500, 0xD2D2D2, 0.94,
              rx, cy + 212.0, ALIGN_LEFT, 0.1);
    paint_field(cr, rx, cy + 230.0, 332.0, 32.0, "Disabled", 1);
    text_draw(cr, "Layout changes apply to all keyboards", 8.5, 400,
              C_FAINT, 0.8, rx, cy + 272.0, ALIGN_LEFT, 0.1);

    double gy = 442.0;
    paint_card(cr, CONTENT_X, gy, CONTENT_W, 104.0);
    text_draw(cr, "Touchpad gestures", 12.0, 600, 0xE8E8E8, 0.95,
              CONTENT_X + 18.0, gy + 16.0, ALIGN_LEFT, 0.1);
    text_draw(cr, "Scroll method", 9.5, 500, C_DIM, 0.85,
              CONTENT_X + 18.0, gy + 42.0, ALIGN_LEFT, 0.2);

    static const char *const scrolls[] = { "Two finger", "Edge", "Button" };
    paint_segment(cr, CONTENT_X + 18.0, gy + 58.0, 332.0, 30.0,
                  scrolls, 3, g_ui.segments[SEG_SCROLL], SEG_SCROLL);

    paint_toggle_row(cr, CONTENT_X + 388.0, gy + 24.0, 332.0,
                     "Scroll momentum",
                     "Keep gliding after the fingers lift",
                     g_ui.toggles[TOG_MOMENTUM], TOG_MOMENTUM);

    text_wrap(cr,
        "Input settings are a preview. A finished app would talk to the "
        "compositor's input configuration and store the result in "
        "~/.config/labwc/rc.xml.",
        8.8, 400, C_FAINT, 0.8, CONTENT_X, 564.0, CONTENT_W, 2);
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

static void display_card(cairo_t *cr, double y, const char *name,
                         const char *model, int connected, int refresh_seg,
                         int scale_seg, int orient_seg, int primary_toggle)
{
    paint_card(cr, CONTENT_X, y, CONTENT_W, 206.0);
    paint_display_preview(cr, CONTENT_X + 18.0, y + 18.0, 196.0, 140.0,
                          connected);

    double rx = CONTENT_X + 240.0;
    text_draw(cr, name, 13.0, 600, connected ? C_FG : C_DIM, 0.96,
              rx, y + 16.0, ALIGN_LEFT, 0.1);
    text_draw(cr, model, 9.0, 400, C_DIM, 0.75,
              rx, y + 36.0, ALIGN_LEFT, 0.1);

    text_draw(cr, "RESOLUTION", 7.8, 600, C_FAINT, 0.9,
              rx, y + 60.0, ALIGN_LEFT, 1.3);
    paint_field(cr, rx, y + 76.0, 230.0, 30.0,
                connected ? "2560 × 1440" : "—", 1);

    text_draw(cr, "REFRESH RATE", 7.8, 600, C_FAINT, 0.9,
              rx + 254.0, y + 60.0, ALIGN_LEFT, 1.3);
    static const char *const refresh[] = { "60 Hz", "120 Hz", "144 Hz" };
    paint_segment(cr, rx + 254.0, y + 76.0, 246.0, 30.0,
                  refresh, 3, g_ui.segments[refresh_seg], refresh_seg);

    text_draw(cr, "ORIENTATION", 7.8, 600, C_FAINT, 0.9,
              rx, y + 118.0, ALIGN_LEFT, 1.3);
    static const char *const orient[] = { "Landscape", "Portrait" };
    paint_segment(cr, rx, y + 134.0, 230.0, 30.0,
                  orient, 2, g_ui.segments[orient_seg], orient_seg);

    text_draw(cr, "SCALE", 7.8, 600, C_FAINT, 0.9,
              rx + 254.0, y + 118.0, ALIGN_LEFT, 1.3);
    static const char *const scales[] = { "100%", "125%", "150%" };
    paint_segment(cr, rx + 254.0, y + 134.0, 246.0, 30.0,
                  scales, 3, g_ui.segments[scale_seg], scale_seg);

    text_draw(cr, "Primary display", 10.0, 500, connected ? 0xD2D2D2 : C_FAINT,
              0.9, rx, y + 176.0, ALIGN_LEFT, 0.1);
    paint_mini_toggle(cr, rx + 230.0, y + 174.0,
                      g_ui.toggles[primary_toggle], primary_toggle);

    text_draw(cr, connected ? "Connected · DisplayPort" : "Cable unplugged",
              8.5, 400, connected ? C_GREEN : C_FAINT,
              connected ? 0.8 : 0.7, CONTENT_X + 18.0, y + 172.0,
              ALIGN_LEFT, 0.2);
}

static void page_displays(cairo_t *cr)
{
    section_label(cr, "OUTPUTS", CONTENT_X, 102.0);
    text_draw(cr, "2 outputs · one active", 9.5, 400, C_DIM, 0.75,
              WIN_W - 36.0, 102.0, ALIGN_RIGHT, 0.2);

    display_card(cr, 122.0, "HDMI-A-1", "Dell U2720Q · 27\"",
                 1, SEG_REFRESH_A, SEG_SCALE_A, SEG_ORIENT_A, TOG_PRIMARY_A);
    display_card(cr, 342.0, "DP-1", "Unknown display",
                 0, SEG_REFRESH_B, SEG_SCALE_B, SEG_ORIENT_B, TOG_PRIMARY_B);

    text_wrap(cr,
        "Output changes are shown locally. Writing them for real would mean "
        "updating the compositor's output configuration and asking jtlabwc "
        "to apply it.",
        8.8, 400, C_FAINT, 0.8, CONTENT_X, 566.0, CONTENT_W, 2);
}

/* ------------------------------------------------------------------ */
/*  Page: Session                                                      */
/* ------------------------------------------------------------------ */

static void page_session(cairo_t *cr)
{
    section_label(cr, "IDLE & LOCK", CONTENT_X, 102.0);
    section_label(cr, "POWER COMMANDS", CONTENT_X + 388.0, 102.0);

    char v[32];
    double cy = 122.0, ch = 232.0;

    paint_card(cr, CONTENT_X, cy, 368.0, ch);
    double lx = CONTENT_X + 18.0;
    text_draw(cr, "Away from the keyboard", 12.0, 600, 0xE8E8E8, 0.95,
              lx, cy + 14.0, ALIGN_LEFT, 0.1);

    paint_slider_row(cr, lx, cy + 44.0, 332.0, "Idle timeout",
                     slider_text(SL_IDLE, g_ui.sliders[SL_IDLE], v, sizeof v),
                     g_ui.sliders[SL_IDLE], SL_IDLE);
    paint_divider(cr, lx, cy + 88.0, 332.0);

    paint_toggle_row(cr, lx, cy + 98.0, 332.0, "Lock when idle",
                     "Require a password after the timeout",
                     g_ui.toggles[TOG_IDLE_LOCK], TOG_IDLE_LOCK);
    paint_toggle_row(cr, lx, cy + 140.0, 332.0, "Dim before locking",
                     "Fade the screen one minute early",
                     g_ui.toggles[TOG_DIM], TOG_DIM);
    paint_toggle_row(cr, lx, cy + 182.0, 332.0, "Lock on suspend",
                     "Never wake straight into the desktop",
                     g_ui.toggles[TOG_LOCK_SUSPEND], TOG_LOCK_SUSPEND);

    double rx = CONTENT_X + 388.0;
    paint_card(cr, rx, cy, 368.0, ch);
    text_draw(cr, "End the session", 12.0, 600, 0xE8E8E8, 0.95,
              rx + 18.0, cy + 14.0, ALIGN_LEFT, 0.1);

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
                          rx + 10.0, ry, 348.0, 40.0);
        int hot = hit_hover(idx);

        if (hot) {
            rgba(cr, C_FG, 0.04);
            rounded_rect(cr, rx + 10.0, ry, 348.0, 40.0, 7.0);
            cairo_fill(cr);
        }

        rgba(cr, i >= 2 ? C_RED : C_ACCENT, i >= 2 ? 0.75 : 0.85);
        cairo_arc(cr, rx + 28.0, ry + 20.0, 2.6, 0.0, 2.0 * PI);
        cairo_fill(cr);

        text_draw(cr, commands[i].label, 11.0, 600, 0xDCDCDC, 0.95,
                  rx + 42.0, ry + 6.0, ALIGN_LEFT, 0.1);
        text_draw(cr, commands[i].desc, 8.5, 400, C_DIM, 0.72,
                  rx + 42.0, ry + 23.0, ALIGN_LEFT, 0.1);

        if (commands[i].key1[0]) {
            double kw = keycap_width(cr, commands[i].key1) + 5.0 +
                        keycap_width(cr, commands[i].key2);
            double kx = rx + 348.0 - kw;
            kx += paint_keycap(cr, commands[i].key1, kx, ry + 10.0) + 5.0;
            paint_keycap(cr, commands[i].key2, kx, ry + 10.0);
        }
    }

    double by = 376.0;
    paint_card(cr, CONTENT_X, by, CONTENT_W, 196.0);
    text_draw(cr, "Session", 12.0, 600, 0xE8E8E8, 0.95,
              CONTENT_X + 18.0, by + 16.0, ALIGN_LEFT, 0.1);

    paint_toggle_row(cr, CONTENT_X + 18.0, by + 48.0, 332.0,
                     "Restore previous windows",
                     "Reopen apps from the last session",
                     g_ui.toggles[TOG_RESTORE], TOG_RESTORE);
    paint_toggle_row(cr, CONTENT_X + 18.0, by + 90.0, 332.0,
                     "Start on last workspace",
                     "Remember the active desktop",
                     g_ui.toggles[TOG_LAST_WS], TOG_LAST_WS);

    double rx2 = CONTENT_X + 388.0;
    text_draw(cr, "Startup workspace", 9.0, 500, C_DIM, 0.85,
              rx2, by + 46.0, ALIGN_LEFT, 0.2);
    paint_field(cr, rx2, by + 62.0, 332.0, 32.0, "Workspace 1", 1);

    text_draw(cr, "Environment file", 9.0, 500, C_DIM, 0.85,
              rx2, by + 106.0, ALIGN_LEFT, 0.2);
    paint_field(cr, rx2, by + 122.0, 332.0, 32.0,
                "~/.config/labwc/environment", 0);

    text_wrap(cr,
        "Power commands are drawn as real rows, but pressing one only shows "
        "a preview message. Nothing is executed.",
        8.8, 400, C_FAINT, 0.8, CONTENT_X, 592.0, CONTENT_W, 2);
}

/* ------------------------------------------------------------------ */
/*  Page: About                                                        */
/* ------------------------------------------------------------------ */

static void page_about(cairo_t *cr)
{
    double cx = WIN_W / 2.0;

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

    double gx = CONTENT_X, gy = 304.0, gw = CONTENT_W, gh = 214.0;
    paint_card(cr, gx, gy, gw, gh);

    static const char *const keys[] = {
        "Compositor", "Toolkit", "Palette", "Interface font",
        "Control socket", "Configuration",
    };
    static const char *const vals[] = {
        "jtlabwc · Wayland", "cairo + Pango · no GTK",
        "#030303 · amber #E8A03C", "Liberation Sans · 10 pt",
        "$XDG_RUNTIME_DIR/jtsettings-ctl.sock", "~/.config/labwc",
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

    paint_button(cr, cx - 182.0, 548.0, 170.0, 40.0,
                 "Documentation", 0, BTN_DOCS);
    paint_button(cr, cx + 12.0, 548.0, 170.0, 40.0,
                 "Check for updates", 1, BTN_UPDATE);

    text_draw(cr, "Built for the JTLab desktop · made with care",
              9.0, 400, C_FAINT, 0.8, cx, 614.0, ALIGN_CENTER, 0.3);
    text_draw(cr, "Source: ~/jtsettings/jtsettings.c",
              8.5, 400, C_FAINT, 0.7, cx, 634.0, ALIGN_CENTER, 0.2);
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

    cairo_pattern_t *ambient = cairo_pattern_create_radial(
        CONTENT_X + CONTENT_W * 0.62, 120.0, 30.0,
        CONTENT_X + CONTENT_W * 0.62, 120.0, 520.0);
    cairo_pattern_add_color_stop_rgba(ambient, 0.0,
        R_(C_ACCENT), G_(C_ACCENT), B_(C_ACCENT), 0.035);
    cairo_pattern_add_color_stop_rgba(ambient, 1.0,
        R_(C_ACCENT), G_(C_ACCENT), B_(C_ACCENT), 0.0);
    cairo_set_source(cr, ambient);
    cairo_paint(cr);
    cairo_pattern_destroy(ambient);

    paint_sidebar(cr, g_ui.t);
    paint_header(cr);

    switch (g_ui.page) {
    case PAGE_KEYBINDINGS: page_keybindings(cr); break;
    case PAGE_STARTUP: page_startup(cr); break;
    case PAGE_RULES: page_rules(cr); break;
    case PAGE_INPUT: page_input(cr); break;
    case PAGE_DISPLAYS: page_displays(cr); break;
    case PAGE_SESSION: page_session(cr); break;
    case PAGE_ABOUT: page_about(cr); break;
    default: break;
    }

    paint_footer(cr);
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
    int32_t scale;
    int entered;
    App *app;
} Output;

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
    int scale;
    int fixed_scale;
    int has_argb;
    ShmBuf bufs[NBUFS];

    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;

    Output outputs[MAX_OUTPUTS];
    int output_count;

    int quit;
    double t0;
    double last_draw;

    int ctl_fd;
    int ctl_client_fd;
    char ctl_buf[256];
    size_t ctl_len;
};

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
    wl_surface_commit(a->surface);
    b->busy = 1;
}

static void recalc_scale(App *a)
{
    if (a->fixed_scale)
        return;

    int s = 0;
    for (int i = 0; i < a->output_count; i++) {
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
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *t,
                               int32_t width, int32_t height,
                               struct wl_array *states)
{
    (void)data;
    (void)t;
    (void)width;
    (void)height;
    (void)states;
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
        if (h->a >= 0 && h->a < PAGE_COUNT) {
            g_ui.page = h->a;
            g_ui.hover = -1;
            g_ui.pressed = -1;
        }
        break;
    case HIT_TOGGLE:
        if (h->a >= 0 && h->a < TOG_COUNT)
            g_ui.toggles[h->a] = !g_ui.toggles[h->a];
        break;
    case HIT_SEGMENT:
        if (h->a >= 0 && h->a < SEG_COUNT)
            g_ui.segments[h->a] = h->b;
        break;
    case HIT_ROW:
        if (g_ui.page >= 0 && g_ui.page < PAGE_COUNT)
            g_ui.selected[g_ui.page] = h->a;
        break;
    case HIT_BUTTON:
        switch (h->a) {
        case BTN_ADD_BINDING:
            ui_toast("Preview only — keybindings are not saved");
            break;
        case BTN_ADD_START:
            ui_toast("Preview only — autostart is not changed");
            break;
        case BTN_NEW_RULE:
            ui_toast("Preview only — window rules are not saved");
            break;
        case BTN_DOCS:
            ui_toast("Preview only — docs would open in a browser");
            break;
        case BTN_UPDATE:
            ui_toast("Preview only — update check is not wired up");
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
    update_cursor(a);
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    (void)pointer; (void)time;
    App *a = data;
    g_ui.pointer_x = wl_fixed_to_double(sx);
    g_ui.pointer_y = wl_fixed_to_double(sy);
    g_ui.hover = hit_at(g_ui.pointer_x, g_ui.pointer_y);

    if (g_ui.pressed >= 0 && g_ui.pressed < g_hit_count &&
        g_hits[g_ui.pressed].kind == HIT_SLIDER)
        slider_set_from_x(g_ui.pressed, g_ui.pointer_x);

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
    int idx = hit_at(x, y);

    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        g_ui.pressed = idx;
        g_ui.hover = idx;
        if (idx >= 0 && g_hits[idx].kind == HIT_SLIDER)
            slider_set_from_x(idx, x);
    } else {
        int kind = (idx >= 0) ? g_hits[idx].kind : HIT_NONE;
        if (idx >= 0 && idx == g_ui.pressed && kind != HIT_SLIDER)
            ui_activate(a, idx);
        g_ui.pressed = -1;
        g_ui.hover = (kind == HIT_NAV) ? -1 : hit_at(x, y);
    }
    update_cursor(a);
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
                         uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)data; (void)pointer; (void)time; (void)axis; (void)value;
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

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                         uint32_t serial, uint32_t time,
                         uint32_t key, uint32_t state)
{
    (void)keyboard; (void)serial; (void)time;
    App *a = data;

    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !a->xkb_state)
        return;

    xkb_keysym_t sym = xkb_state_key_get_one_sym(a->xkb_state, key + 8);

    if (sym == XKB_KEY_Escape || sym == XKB_KEY_q || sym == XKB_KEY_x) {
        a->quit = 1;
        return;
    }
    if (sym == XKB_KEY_Down || sym == XKB_KEY_Right ||
        sym == XKB_KEY_Page_Down) {
        g_ui.page = (g_ui.page + 1) % PAGE_COUNT;
        g_ui.hover = -1;
        g_ui.pressed = -1;
        return;
    }
    if (sym == XKB_KEY_Up || sym == XKB_KEY_Left ||
        sym == XKB_KEY_Page_Up) {
        g_ui.page = (g_ui.page + PAGE_COUNT - 1) % PAGE_COUNT;
        g_ui.hover = -1;
        g_ui.pressed = -1;
        return;
    }
    if (sym >= XKB_KEY_1 && sym <= XKB_KEY_9) {
        int p = (int)(sym - XKB_KEY_1);
        if (p < PAGE_COUNT) {
            g_ui.page = p;
            g_ui.hover = -1;
            g_ui.pressed = -1;
        }
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
    } else if (!strcmp(interface, wl_output_interface.name)) {
        if (a->output_count < MAX_OUTPUTS) {
            Output *o = &a->outputs[a->output_count];
            o->output = wl_registry_bind(registry, name,
                                         &wl_output_interface,
                                         version < 2 ? version : 2);
            o->scale = 1;
            o->entered = 0;
            o->app = a;
            wl_output_add_listener(o->output, &output_listener, o);
            a->output_count++;
        }
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
    (void)data; (void)registry; (void)name;
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
    xdg_toplevel_set_min_size(a->toplevel, (int)WIN_W, (int)WIN_H);
    xdg_toplevel_set_max_size(a->toplevel, (int)WIN_W, (int)WIN_H);

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

static void ctl_path(char *buf, size_t n)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !rt[0]) rt = "/tmp";
    snprintf(buf, n, "%s/jtsettings-ctl.sock", rt);
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

    if (strncmp(line, "accent", 6) == 0 &&
        (line[6] == ' ' || line[6] == '\t')) {
        const char *arg = line + 6;
        while (*arg == ' ' || *arg == '\t')
            arg++;
        unsigned rgb;
        if (accent_parse(arg, &rgb)) {
            accent_set(rgb);
                    a->last_draw = -1.0;
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
            g_ui.page = page;
            g_ui.hover = -1;
            g_ui.pressed = -1;
            a->last_draw = -1.0;
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
    for (;;) {
        ssize_t n = read(a->ctl_client_fd, buf, sizeof(buf));
        if (n > 0) {
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
    for (;;) {
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
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "jtsettings: cannot bind %s: %s\n",
                path, strerror(errno));
        close(fd);
        return 0;
    }
    if (listen(fd, 4) < 0) {
        fprintf(stderr, "jtsettings: cannot listen on %s: %s\n",
                path, strerror(errno));
        close(fd);
        unlink(path);
        return 0;
    }

    a->ctl_fd = fd;
    return 1;
}

static void ctl_cleanup(App *a)
{
    ctl_close_client(a);
    if (a->ctl_fd >= 0) {
        close(a->ctl_fd);
        a->ctl_fd = -1;
        char path[256];
        ctl_path(path, sizeof(path));
        unlink(path);
    }
}

/* Returns < 0 on a fatal display error, 0 otherwise. */
static int pump_events(App *a, int timeout_ms)
{
    if (wl_display_dispatch_pending(a->display) < 0)
        return -1;

    int want_write = 0;
    if (wl_display_flush(a->display) < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
        want_write = 1;
    }

    struct pollfd pfd[3];
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

    int pr = poll(pfd, (nfds_t)nfds, timeout_ms);
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
        "Pages: keybindings, startup, rules, input,\n"
        "       displays, session, about\n",
        argv0, argv0);
}

int main(int argc, char **argv)
{
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

    App a;
    memset(&a, 0, sizeof a);
    a.scale = 1;
    a.ctl_fd = -1;
    a.ctl_client_fd = -1;
    a.last_draw = -1.0;
    a.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    if (!wayland_start(&a)) {
        app_cleanup(&a);
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

        if (a.configured && now - a.last_draw >= 1.0 / 30.0) {
            app_draw(&a);
            a.last_draw = now;
        }

        int timeout = a.configured ? 33 : 100;
        if (pump_events(&a, timeout) < 0)
            break;
    }

    app_cleanup(&a);
    return 0;
}
