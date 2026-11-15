/*
 * Build engine system layer for the M5Stack CardputerZero.
 *
 * This file takes the place of JFBuild's sdlayer2.c, so the engine needs no
 * SDL video driver, no window system and no DRM master:
 *
 *   video : Linux fbdev (the 320x170 RGB565 ST7789 panel, /dev/fb_lcd)
 *   input : Linux evdev (the built-in TCA8418 keyboard, plus any USB keyboard)
 *   timer : CLOCK_MONOTONIC
 *
 * The engine renders 8-bit 320x200. The panel is 320x170, so every frame is
 * scaled down vertically with a two-row blend, which keeps the status bar
 * digits and menu text readable where plain row-dropping would not.
 *
 * Copyright (C) 2026 czduke contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <linux/fb.h>

#include "build.h"
#include "baselayer.h"
#include "baselayer_priv.h"
#include "a.h"
#include "osd.h"

#define SRC_W 320
#define SRC_H 200

extern void gameexit(const char *msg); /* the game's own orderly shutdown */

int displaycnt = 1;

/* ------------------------------------------------------------------ clock */

/* With CZDUKE_MAX_FRAMES set (tests), time is virtual: it advances by one
 * frame period per frame and a little per event poll, so a run is repeatable
 * and takes no longer than the CPU needs. */
#define VIRTUAL_START_US 1000000u
static int virtual_time;
static uint64_t virtual_us = VIRTUAL_START_US;
static long max_frames, frame_count;
static unsigned frame_us = 1000000 / 30;

static uint64_t real_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static uint64_t now_us(void)
{
    static uint64_t base;
    if (virtual_time) return virtual_us;
    if (!base) base = real_us() - 1000000u;
    return real_us() - base;
}

static int timer_tps;
static uint64_t timer_last;
static void (*timer_callback)(void);

int inittimer(int tickspersecond, void (*callback)(void))
{
    if (timer_tps) return 0;
    timer_tps = tickspersecond;
    timer_last = now_us() * (uint64_t)timer_tps / 1000000u;
    timer_callback = callback;
    return 0;
}

void uninittimer(void)
{
    timer_tps = 0;
}

void sampletimer(void)
{
    if (!timer_tps) return;
    uint64_t t = now_us() * (uint64_t)timer_tps / 1000000u;
    int n = (int)(t - timer_last);
    if (n > 0) {
        totalclock += n;
        timer_last = t;
        if (timer_callback) for (; n > 0; n--) timer_callback();
    }
}

unsigned int getticks(void)      { return (unsigned int)(now_us() / 1000u); }
unsigned int getusecticks(void)  { return (unsigned int)now_us(); }
int gettimerfreq(void)           { return timer_tps; }

/* ------------------------------------------------------------------ video */

static struct {
    int fd;
    uint8_t *map;       /* mmap of the device, or NULL when using pwrite() */
    size_t map_len;
    uint8_t *stage;     /* one finished frame in device pixel format */
    size_t stage_len;
    int w, h, bytespp, stride;
    size_t origin;      /* byte offset of the visible area in the device */
    int r_off, r_len, g_off, g_len, b_off, b_len;
    int is_rgb565;
    int fake;           /* a plain file standing in for the device (tests) */
    int dx, dy, dw, dh; /* where the picture goes inside the panel */
} fb = { .fd = -1 };

static unsigned char *frame;    /* the engine's 8-bit picture */

/* For each destination column/row: the two source samples and the weight
 * (0..256) of the second one. */
static uint16_t *x0_tab, *x1_tab, *xw_tab;
static uint16_t *y0_tab, *y1_tab, *yw_tab;
static uint32_t row_tmp[SRC_W];

static void build_axis(int dst_n, int src_n, uint16_t **i0, uint16_t **i1, uint16_t **w)
{
    *i0 = malloc(sizeof(uint16_t) * dst_n);
    *i1 = malloc(sizeof(uint16_t) * dst_n);
    *w = malloc(sizeof(uint16_t) * dst_n);
    for (int i = 0; i < dst_n; i++) {
        /* sample at pixel centres, in 1/256 source pixels */
        long pos = ((2L * i + 1) * src_n * 256) / (2L * dst_n) - 128;
        if (pos < 0) pos = 0;
        long a = pos >> 8;
        if (a > src_n - 1) a = src_n - 1;
        long b = a + 1 > src_n - 1 ? src_n - 1 : a + 1;
        (*i0)[i] = (uint16_t)a;
        (*i1)[i] = (uint16_t)b;
        (*w)[i] = (dst_n == src_n) ? 0 : (uint16_t)(pos & 0xff);
    }
}

static inline uint32_t mix(uint32_t a, uint32_t b, uint32_t w)
{
    uint32_t rb = ((((a & 0xff00ff) * (256 - w)) + ((b & 0xff00ff) * w)) >> 8) & 0xff00ff;
    uint32_t g = ((((a & 0x00ff00) * (256 - w)) + ((b & 0x00ff00) * w)) >> 8) & 0x00ff00;
    return rb | g;
}

static inline uint32_t pack(uint32_t xrgb)
{
    uint32_t r = (xrgb >> 16) & 0xff, g = (xrgb >> 8) & 0xff, b = xrgb & 0xff;
    return ((r >> (8 - fb.r_len)) << fb.r_off) |
           ((g >> (8 - fb.g_len)) << fb.g_off) |
           ((b >> (8 - fb.b_len)) << fb.b_off);
}

static void fb_flush(void)
{
    if (fb.map) {
        memcpy(fb.map + fb.origin, fb.stage, fb.stage_len);
    } else if (pwrite(fb.fd, fb.stage, fb.stage_len, (off_t)fb.origin) < 0) {
        /* nothing useful to do about a failed frame */
    }
}

static void fb_blank(void)
{
    if (fb.fd < 0 || !fb.stage || fb.fake) return; /* tests keep the last frame */
    memset(fb.stage, 0, fb.stage_len);
    fb_flush();
}

static const char *find_fb_device(void)
{
    static char path[64];
    const char *env = getenv("CZDUKE_FB");
    if (env && *env) return env;

    /* udev rule shipped with the CardputerZero image */
    if (access("/dev/fb_lcd", F_OK) == 0) return "/dev/fb_lcd";

    FILE *fp = fopen("/proc/fb", "r");
    if (fp) {
        char line[256];
        int n = -1;
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "st7789") || strstr(line, "mipi-dbi") || strstr(line, "mipi_dbi")) {
                if (sscanf(line, "%d", &n) == 1) break;
                n = -1;
            }
        }
        fclose(fp);
        if (n >= 0) {
            snprintf(path, sizeof(path), "/dev/fb%d", n);
            return path;
        }
    }
    return "/dev/fb0";
}

static int fb_open(void)
{
    const char *path = find_fb_device();
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    int bpp;

    fb.fd = open(path, O_RDWR);
    if (fb.fd < 0) {
        buildprintf("czduke: cannot open %s: %s\n", path, strerror(errno));
        buildprintf("czduke: is this user in the 'video' group?\n");
        return -1;
    }

    memset(&v, 0, sizeof(v));
    memset(&f, 0, sizeof(f));
    if (ioctl(fb.fd, FBIOGET_VSCREENINFO, &v) == 0 && ioctl(fb.fd, FBIOGET_FSCREENINFO, &f) == 0) {
        fb.w = (int)v.xres;
        fb.h = (int)v.yres;
        bpp = (int)v.bits_per_pixel;
        fb.stride = (int)f.line_length;
        fb.r_off = (int)v.red.offset;   fb.r_len = (int)v.red.length;
        fb.g_off = (int)v.green.offset; fb.g_len = (int)v.green.length;
        fb.b_off = (int)v.blue.offset;  fb.b_len = (int)v.blue.length;
        fb.origin = (size_t)v.yoffset * f.line_length + (size_t)v.xoffset * (bpp / 8);
        fb.map_len = f.smem_len ? f.smem_len : (size_t)f.line_length * v.yres_virtual;
    } else {
        /* Not a framebuffer device. CZDUKE_FAKEFB=WxHxBPP lets a plain file
         * stand in for one, which is how the port is tested off-device. */
        const char *fake = getenv("CZDUKE_FAKEFB");
        if (!fake || sscanf(fake, "%dx%dx%d", &fb.w, &fb.h, &bpp) != 3) {
            buildprintf("czduke: %s is not a framebuffer device\n", path);
            return -1;
        }
        fb.fake = 1;
        fb.stride = fb.w * (bpp / 8);
        if (bpp == 16) {
            fb.r_off = 11; fb.r_len = 5; fb.g_off = 5; fb.g_len = 6; fb.b_off = 0; fb.b_len = 5;
        } else {
            fb.r_off = 16; fb.r_len = 8; fb.g_off = 8; fb.g_len = 8; fb.b_off = 0; fb.b_len = 8;
        }
        fb.map_len = (size_t)fb.stride * fb.h;
        if (ftruncate(fb.fd, (off_t)fb.map_len) < 0) {
            buildprintf("czduke: cannot size %s: %s\n", path, strerror(errno));
            return -1;
        }
    }

    if ((bpp != 16 && bpp != 24 && bpp != 32) || fb.w <= 0 || fb.h <= 0 ||
        fb.r_len < 1 || fb.r_len > 8 || fb.g_len < 1 || fb.g_len > 8 || fb.b_len < 1 || fb.b_len > 8) {
        buildprintf("czduke: unsupported framebuffer format (%dx%d, %d bpp)\n", fb.w, fb.h, bpp);
        return -1;
    }
    fb.bytespp = bpp / 8;
    fb.is_rgb565 = (bpp == 16 && fb.r_off == 11 && fb.g_off == 5 && fb.b_off == 0 &&
                    fb.r_len == 5 && fb.g_len == 6 && fb.b_len == 5);

    fb.stage_len = (size_t)fb.stride * fb.h;
    fb.stage = calloc(1, fb.stage_len);

    fb.map = mmap(NULL, fb.map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb.fd, 0);
    if (fb.map == MAP_FAILED || fb.origin + fb.stage_len > fb.map_len) {
        if (fb.map != MAP_FAILED) munmap(fb.map, fb.map_len);
        fb.map = NULL; /* fall back to pwrite() */
    }

    /* Default: fill the whole panel. CZDUKE_ASPECT=4:3 keeps the original
     * proportions and pillarboxes instead. */
    fb.dx = 0; fb.dy = 0; fb.dw = fb.w; fb.dh = fb.h;
    const char *aspect = getenv("CZDUKE_ASPECT");
    if (aspect && strcmp(aspect, "4:3") == 0) {
        if (fb.h * 4 / 3 <= fb.w) {
            fb.dw = fb.h * 4 / 3;
            fb.dx = (fb.w - fb.dw) / 2;
        } else {
            fb.dh = fb.w * 3 / 4;
            fb.dy = (fb.h - fb.dh) / 2;
        }
    }

    build_axis(fb.dw, SRC_W, &x0_tab, &x1_tab, &xw_tab);
    build_axis(fb.dh, SRC_H, &y0_tab, &y1_tab, &yw_tab);

    buildprintf("czduke: video %s %dx%d %dbpp (%s), picture %dx%d at %d,%d\n", path, fb.w, fb.h, bpp,
                fb.map ? "mmap" : "pwrite", fb.dw, fb.dh, fb.dx, fb.dy);
    fb_blank();
    return 0;
}

void getvalidmodes(void)
{
    if (validmodecnt) return;
    addvalidmode(SRC_W, SRC_H, 8, 1, 0, 0, -1);
    addvalidmode(SRC_W, SRC_H, 8, 0, 0, 0, -1);
    sortvalidmodes();
}

/* There is exactly one mode: whatever is asked for, the engine gets 8-bit
 * 320x200 and the panel gets a scaled copy of it. */
int setvideomode(int xdim, int ydim, int bitspp, int fullsc)
{
    int i, j, pitch;

    (void)xdim; (void)ydim;
    if (bitspp != 8) return -1;
    fullsc &= 255;

    if (frame && fullsc == fullscreen && !videomodereset) {
        OSD_ResizeDisplay(xres, yres);
        return 0;
    }

    if (fb.fd < 0 && fb_open() < 0) {
        /* The game carries on without a picture if this merely fails, so
         * stop here, with the reason already in the log. */
        fflush(NULL);
        exit(1);
    }

    if (baselayer_videomodewillchange) baselayer_videomodewillchange();

    pitch = (((SRC_W | 1) + 4) & ~3);
    free(frame);
    frame = calloc(1, (size_t)pitch * (SRC_H + 1));
    if (!frame) return -1;

    frameplace = (intptr_t)frame;
    bytesperline = pitch;
    imageSize = bytesperline * SRC_H;
    numpages = 1;

    setvlinebpl(bytesperline);
    for (i = j = 0; i <= SRC_H; i++) {
        ylookup[i] = j;
        j += bytesperline;
    }

    xres = SRC_W;
    yres = SRC_H;
    bpp = 8;
    fullscreen = fullsc;

    videomodereset = 0;
    if (baselayer_videomodedidchange) baselayer_videomodedidchange();
    OSD_ResizeDisplay(xres, yres);
    setpalettefade(palfadergb.r, palfadergb.g, palfadergb.b, palfadedelta);
    return 0;
}

const char *getdisplayname(int display)
{
    return display == 0 ? "CardputerZero LCD" : NULL;
}

void showframe(void)
{
    uint32_t lut[256];

    if (!frame || fb.fd < 0) return;

    for (int i = 0; i < 256; i++) {
        lut[i] = ((uint32_t)curpalettefaded[i].r << 16) | ((uint32_t)curpalettefaded[i].g << 8) |
                 (uint32_t)curpalettefaded[i].b;
    }

    for (int y = 0; y < fb.dh; y++) {
        const unsigned char *ra = frame + (size_t)y0_tab[y] * bytesperline;
        uint32_t wy = yw_tab[y];

        if (wy) {
            const unsigned char *rb = frame + (size_t)y1_tab[y] * bytesperline;
            for (int x = 0; x < SRC_W; x++) row_tmp[x] = mix(lut[ra[x]], lut[rb[x]], wy);
        } else {
            for (int x = 0; x < SRC_W; x++) row_tmp[x] = lut[ra[x]];
        }

        uint8_t *out = fb.stage + (size_t)(fb.dy + y) * fb.stride + (size_t)fb.dx * fb.bytespp;
        for (int x = 0; x < fb.dw; x++) {
            uint32_t p = row_tmp[x0_tab[x]];
            if (xw_tab[x]) p = mix(p, row_tmp[x1_tab[x]], xw_tab[x]);

            if (fb.is_rgb565) {
                *(uint16_t *)(void *)out = (uint16_t)(((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) | ((p >> 3) & 0x001f));
            } else {
                uint32_t v = pack(p);
                if (fb.bytespp == 2) {
                    *(uint16_t *)(void *)out = (uint16_t)v;
                } else if (fb.bytespp == 4) {
                    *(uint32_t *)(void *)out = v;
                } else {
                    out[0] = (uint8_t)v; out[1] = (uint8_t)(v >> 8); out[2] = (uint8_t)(v >> 16);
                }
            }
            out += fb.bytespp;
        }
    }
    fb_flush();

    if (virtual_time) {
        virtual_us += frame_us;
        if (++frame_count >= max_frames) {
            buildprintf("czduke: stopping after %ld frames, %u ms\n", frame_count,
                        (unsigned)((virtual_us - VIRTUAL_START_US) / 1000u));
            fflush(NULL);
            _exit(0);
        }
    } else {
        /* The engine draws as fast as it can; the panel does not need more
         * than the game's own 30 tics a second. */
        static uint64_t next;
        uint64_t t = real_us();
        if (next > t && next - t <= frame_us) {
            struct timespec ts = { 0, (long)(next - t) * 1000L };
            nanosleep(&ts, NULL);
            next += frame_us;
        } else {
            next = t + frame_us;
        }
    }
}

int setpalette(int start, int num, unsigned char *dapal)
{
    (void)start; (void)num; (void)dapal; /* showframe() reads curpalettefaded */
    return 0;
}

int setsysgamma(float shadergamma, float sysgamma)
{
    (void)shadergamma; (void)sysgamma;
    return -1; /* no hardware gamma: the engine brightens the palette itself */
}

/* ------------------------------------------------------------------ input */

struct evdev_event {
    struct timeval time;
    uint16_t type;
    uint16_t code;
    int32_t value;
};
#define EVDEV_EV_KEY 0x01
#define EVDEV_IOC_GBIT_KEY(len) _IOC(_IOC_READ, 'E', 0x20 + EVDEV_EV_KEY, len)
#define EK_MAX 0x2ff

/* evdev key codes used by name below */
enum {
    EK_ESC = 1, EK_BACKSPACE = 14, EK_TAB = 15, EK_ENTER = 28, EK_LEFTCTRL = 29,
    EK_LEFTSHIFT = 42, EK_RIGHTSHIFT = 54, EK_LEFTALT = 56, EK_SPACE = 57,
    EK_KPENTER = 96, EK_RIGHTCTRL = 97, EK_RIGHTALT = 100
};

/* PC scancodes, as the Build engine and the game know them */
enum {
    SC_ESC = 0x01, SC_ENTER = 0x1c, SC_LCTRL = 0x1d, SC_A = 0x1e, SC_SEMICOLON = 0x27,
    SC_QUOTE = 0x28, SC_TILDE = 0x29, SC_Z = 0x2c, SC_COMMA = 0x33, SC_PERIOD = 0x34,
    SC_SPACE = 0x39, SC_KP5 = 0x4c, SC_PAUSE = 0x59, SC_LBRACKET = 0x1a, SC_RBRACKET = 0x1b,
    SC_R = 0x13, SC_UP = 0xc8, SC_PGUP = 0xc9, SC_LEFT = 0xcb, SC_RIGHT = 0xcd,
    SC_DOWN = 0xd0, SC_PGDN = 0xd1
};

/* evdev code -> scancode. Codes 1..83, 87 and 88 are the AT set 1 scancodes
 * already; the rest are the "extended" keys. */
static unsigned char scancode_of(int code)
{
    if ((code >= 1 && code <= 83) || code == 87 || code == 88) return (unsigned char)code;
    switch (code) {
    case 96:  return 0x9c; /* keypad Enter */
    case 97:  return 0x9d; /* right Ctrl */
    case 98:  return 0xb5; /* keypad / */
    case 100: return 0xb8; /* right Alt */
    case 102: return 0xc7; /* Home */
    case 103: return 0xc8; /* Up */
    case 104: return 0xc9; /* Page Up */
    case 105: return 0xcb; /* Left */
    case 106: return 0xcd; /* Right */
    case 107: return 0xcf; /* End */
    case 108: return 0xd0; /* Down */
    case 109: return 0xd1; /* Page Down */
    case 110: return 0xd2; /* Insert */
    case 111: return 0xd3; /* Delete */
    case 119: return SC_PAUSE;
    default:  return 0;
    }
}

/* The built-in keyboard only reaches arrows, F-keys and punctuation through
 * Fn, so the game is played on the letter keys: each one is reported to the
 * game as the key its default ("classic") configuration binds to the action.
 * Letters that are not listed keep their own meaning: H holoduke, J jetpack,
 * N night vision, M medkit, Y/N for prompts. */
static unsigned char play_layout(int code)
{
    switch (code) {
    case 17: return SC_UP;        /* W  forward, menu up */
    case 31: return SC_DOWN;      /* S  back, menu down */
    case 30: return SC_LEFT;      /* A  turn left */
    case 32: return SC_RIGHT;     /* D  turn right */
    case 16: return SC_COMMA;     /* Q  strafe left */
    case 18: return SC_PERIOD;    /* E  strafe right */
    case 37: return SC_LCTRL;     /* K  fire */
    case 38: return SC_SPACE;     /* L  open / use */
    case 57: return SC_A;         /* Space  jump */
    case 46: return SC_Z;         /* C  crouch (as is Z) */
    case 19: return SC_PGUP;      /* R  look up */
    case 33: return SC_PGDN;      /* F  look down */
    case 47: return SC_KP5;       /* V  centre view */
    case 20: return SC_QUOTE;     /* T  next weapon */
    case 34: return SC_SEMICOLON; /* G  previous weapon */
    case 22: return SC_LBRACKET;  /* U  inventory left */
    case 24: return SC_RBRACKET;  /* O  inventory right */
    case 23: return SC_ENTER;     /* I  use inventory item */
    case 45: return SC_R;         /* X  steroids */
    case 48: return SC_TILDE;     /* B  quick kick */
    case 25: return SC_PAUSE;     /* P  pause */
    default: return 0;
    }
}

/* The character a key types, for save names, cheats and the console. */
static unsigned char ascii_of(int code, int shift, int ctrl)
{
    static const char plain[] =
        "\0\033" "1234567890-=" "\010\011" "qwertyuiop[]" "\015\0" "asdfghjkl;'`" "\0\\" "zxcvbnm,./" "\0*\0 ";
    static const char shifted[] =
        "\0\033" "!@#$%^&*()_+" "\010\011" "QWERTYUIOP{}" "\015\0" "ASDFGHJKL:\"~" "\0|" "ZXCVBNM<>?" "\0*\0 ";
    unsigned char ch;

    if (code == EK_KPENTER) return 13;
    if (code < 0 || code >= (int)sizeof(plain) - 1) return 0;
    ch = (unsigned char)(shift ? shifted[code] : plain[code]);
    if (ctrl && ch >= 'a' && ch <= 'z') return (unsigned char)(ch - 'a' + 1);
    if (ctrl && ch >= 'A' && ch <= 'Z') return (unsigned char)(ch - 'A' + 1);
    return ch;
}

#define MAX_KBD 8

static struct kbd {
    int fd;
    int scripted;               /* plain file of events with timestamps (tests) */
    int have_pending;
    struct evdev_event pending;
} kbd[MAX_KBD];
static int kbd_count;
static int raw_keys;                     /* CZDUKE_RAWKEYS: no letter-key layout */
static int alt_down, shift_down, ctrl_down;
static unsigned char sent[EK_MAX + 1];   /* what each held key was reported as */
static char keynames[256][12];

static void push_scancode(int sc, int pressed)
{
    if (sc == OSD_CaptureKey(-1)) {
        if (pressed) OSD_ShowDisplay(-1);
        return;
    }
    if (OSD_HandleKey(sc, pressed) == 0) return;

    keystatus[sc] = (char)(pressed ? 1 : 0);
    keyfifo[keyfifoend] = sc;
    keyfifo[(keyfifoend + 1) & (KEYFIFOSIZ - 1)] = pressed ? 1 : 0;
    keyfifoend = (keyfifoend + 2) & (KEYFIFOSIZ - 1);
}

static void push_char(unsigned char ch)
{
    if (!ch || !OSD_HandleChar(ch)) return;
    if (((keyasciififoend + 1) & (KEYFIFOSIZ - 1)) == keyasciififoplc) return;
    keyasciififo[keyasciififoend] = ch;
    keyasciififoend = (keyasciififoend + 1) & (KEYFIFOSIZ - 1);
}

static void key_event(int code, int value)
{
    if (code <= 0 || code > EK_MAX) return;

    if (code == EK_LEFTSHIFT || code == EK_RIGHTSHIFT) shift_down = value != 0;
    if (code == EK_LEFTCTRL || code == EK_RIGHTCTRL) ctrl_down = value != 0;
    if (!raw_keys && (code == EK_LEFTALT || code == EK_RIGHTALT)) {
        /* Alt held: letters are themselves, not the play layout */
        alt_down = value != 0;
        return;
    }

    if (value == 2) { /* autorepeat: only typing repeats */
        push_char(ascii_of(code, shift_down, ctrl_down));
        return;
    }

    if (value) {
        unsigned char sc = (raw_keys || alt_down) ? 0 : play_layout(code);
        if (!sc) sc = scancode_of(code);
        if (sc) {
            if (sent[code]) push_scancode(sent[code], 0);
            sent[code] = sc;
            push_scancode(sc, 1);
        }
        /* Typing always gets the key's own character, whatever it plays as. */
        push_char(ascii_of(code, shift_down, ctrl_down));
    } else if (sent[code]) {
        /* release whatever the press was reported as */
        push_scancode(sent[code], 0);
        sent[code] = 0;
    }
}

static int is_keyboard(int fd, int *scripted)
{
    unsigned char bits[(EK_MAX + 8) / 8];
    struct stat st;

    /* A FIFO or plain file of raw events is accepted for off-device tests. */
    *scripted = 0;
    if (fstat(fd, &st) == 0 && !S_ISCHR(st.st_mode)) {
        *scripted = S_ISREG(st.st_mode);
        return 1;
    }

    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVDEV_IOC_GBIT_KEY(sizeof(bits)), bits) < 0) return 0;
    /* must have both W and Enter */
    return (bits[17 / 8] & (1 << (17 % 8))) && (bits[EK_ENTER / 8] & (1 << (EK_ENTER % 8)));
}

static void kbd_add(const char *path)
{
    struct stat a, b;
    int scripted;

    if (kbd_count >= MAX_KBD) return;
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return;
    if (!is_keyboard(fd, &scripted) || fstat(fd, &a) < 0) { close(fd); return; }
    for (int i = 0; i < kbd_count; i++) {
        /* same device reached through a symlink */
        if (fstat(kbd[i].fd, &b) == 0 && a.st_rdev == b.st_rdev && a.st_ino == b.st_ino) { close(fd); return; }
    }
    kbd[kbd_count].fd = fd;
    kbd[kbd_count].scripted = scripted;
    kbd_count++;
    buildprintf("czduke: keyboard %s\n", path);
}

static void kbd_open(void)
{
    const char *env = getenv("CZDUKE_KBD");
    if (env && *env) {
        kbd_add(env);
    } else {
        kbd_add("/dev/input/cardputer-zero-internal");
        kbd_add("/dev/input/by-path/platform-3f804000.i2c-event");
        DIR *d = opendir("/dev/input");
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                char path[300];
                if (strncmp(e->d_name, "event", 5) != 0) continue;
                snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
                kbd_add(path);
            }
            closedir(d);
        }
    }
    if (kbd_count == 0) {
        buildprintf("czduke: no keyboard found under /dev/input (is this user in the 'input' group?)\n");
    }
}

static void kbd_poll(void)
{
    for (int i = 0; i < kbd_count; i++) {
        struct kbd *k = &kbd[i];

        if (k->scripted) {
            /* Test script: each event carries the time since start at which
             * it happens. */
            for (;;) {
                if (!k->have_pending) {
                    if (read(k->fd, &k->pending, sizeof(k->pending)) != (ssize_t)sizeof(k->pending)) break;
                    k->have_pending = 1;
                }
                uint64_t due = (uint64_t)k->pending.time.tv_sec * 1000000u + (uint64_t)k->pending.time.tv_usec;
                if (due + VIRTUAL_START_US > now_us()) break;
                k->have_pending = 0;
                if (k->pending.type == EVDEV_EV_KEY) key_event(k->pending.code, k->pending.value);
            }
            continue;
        }

        struct evdev_event ev[32];
        ssize_t n;
        while ((n = read(k->fd, ev, sizeof(ev))) >= (ssize_t)sizeof(ev[0])) {
            for (size_t j = 0; j < (size_t)n / sizeof(ev[0]); j++) {
                if (ev[j].type == EVDEV_EV_KEY) key_event(ev[j].code, ev[j].value);
            }
        }
    }
}

static void name_keys(void)
{
    static const struct { unsigned char sc; const char *name; } named[] = {
        { 0x01, "Escape" }, { 0x0e, "Backspace" }, { 0x0f, "Tab" }, { 0x1c, "Enter" },
        { 0x1d, "Left Ctrl" }, { 0x2a, "Left Shift" }, { 0x36, "Right Shift" },
        { 0x38, "Left Alt" }, { 0x39, "Space" }, { 0x3a, "Caps Lock" }, { 0x45, "Num Lock" },
        { 0x46, "Scroll Lock" }, { 0x4c, "Keypad 5" }, { 0x59, "Pause" }, { 0x9c, "Keypad Enter" },
        { 0x9d, "Right Ctrl" }, { 0xb8, "Right Alt" }, { 0xc7, "Home" }, { 0xc8, "Up" },
        { 0xc9, "Page Up" }, { 0xcb, "Left" }, { 0xcd, "Right" }, { 0xcf, "End" },
        { 0xd0, "Down" }, { 0xd1, "Page Down" }, { 0xd2, "Insert" }, { 0xd3, "Delete" },
    };

    memset(keynames, 0, sizeof(keynames));
    for (int code = 2; code <= 53; code++) {
        unsigned char ch = ascii_of(code, 1, 0);
        if (ch > ' ' && ch < 127 && scancode_of(code)) {
            unsigned char lower = ascii_of(code, 0, 0);
            keynames[scancode_of(code)][0] = (char)((lower >= 'a' && lower <= 'z') ? ch : lower);
        }
    }
    for (int f = 1; f <= 12; f++) {
        snprintf(keynames[f <= 10 ? 0x3a + f : 0x57 + (f - 11)], sizeof(keynames[0]), "F%d", f);
    }
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        snprintf(keynames[named[i].sc], sizeof(keynames[0]), "%s", named[i].name);
    }
}

int initinput(void)
{
    const char *raw = getenv("CZDUKE_RAWKEYS");

    inputdevices = 1; /* keyboard only */
    raw_keys = raw && *raw && *raw != '0';
    name_keys();
    if (kbd_count == 0) kbd_open();
    return 0;
}

void uninitinput(void) { }

const char *getkeyname(int num)
{
    if ((unsigned)num >= 256) return NULL;
    return keynames[num];
}

const char *getjoyname(int what, int num) { (void)what; (void)num; return NULL; }

int initmouse(void) { return 0; }
void uninitmouse(void) { }
void grabmouse(int a) { (void)a; mousex = mousey = 0; }
void readmousexy(int *x, int *y) { *x = *y = 0; }
void readmousebstatus(int *b) { *b = 0; }
void releaseallbuttons(void) { }

/* ------------------------------------------------------------- lifecycle */

static volatile sig_atomic_t quit_requested;

static void on_signal(int sig)
{
    (void)sig;
    if (quit_requested) _exit(1); /* second signal: the game loop is stuck */
    quit_requested = 1;
}

int handleevents(void)
{
    static int quitting;

    if (virtual_time) virtual_us += 1000;

    if (quit_requested && !quitting) {
        /* The launcher's Home long-press: save the settings and leave, with
         * no "are you sure" prompt to get stuck on. */
        quitting = 1;
        gameexit("");
    }

    kbd_poll();
    sampletimer();
    return 0;
}

int initsystem(void)
{
    buildprintf("CardputerZero system interface (fbdev video, evdev input)\n");
    atexit(uninitsystem);
    return 0;
}

void uninitsystem(void)
{
    uninitinput();
    uninittimer();
    fb_blank();
}

void initputs(const char *str) { (void)str; }

void debugprintf(const char *f, ...) { (void)f; }

/* No window system: messages go to the log, questions get the safe answer. */
int wm_msgbox(const char *name, const char *fmt, ...)
{
    va_list va;
    printf("%s: ", name ? name : "czduke");
    va_start(va, fmt);
    vprintf(fmt, va);
    va_end(va);
    putchar('\n');
    fflush(stdout);
    return 0;
}

int wm_ynbox(const char *name, const char *fmt, ...)
{
    va_list va;
    printf("%s: ", name ? name : "czduke");
    va_start(va, fmt);
    vprintf(fmt, va);
    va_end(va);
    puts(" (assuming 'No')");
    fflush(stdout);
    return 0;
}

int wm_filechooser(const char *initialdir, const char *initialfile, const char *type, int foropen, char **choice)
{
    (void)initialdir; (void)initialfile; (void)type; (void)foropen; (void)choice;
    return -1;
}

int wm_idle(void *ptr) { (void)ptr; return 0; }
void wm_setapptitle(const char *name) { (void)name; }
void wm_setwindowtitle(const char *name) { (void)name; }
void wm_allowbackgroundidle(int onf) { (void)onf; }
void wm_allowtaskswitching(int onf) { (void)onf; }

int startwin_open(void) { return 0; }
int startwin_close(void) { return 0; }
int startwin_puts(const char *s) { (void)s; return 0; }
int startwin_idle(void *s) { (void)s; return 0; }
int startwin_settitle(const char *s) { (void)s; return 0; }

int main(int argc, char *argv[])
{
    struct sigaction sa;
    const char *env;

    setvbuf(stdout, NULL, _IOLBF, 0); /* keep the log complete if we crash */

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);   /* the launcher's Home long-press */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    env = getenv("CZDUKE_FPS");
    if (env && atoi(env) >= 5 && atoi(env) <= 120) frame_us = 1000000u / (unsigned)atoi(env);

    env = getenv("CZDUKE_MAX_FRAMES"); /* testing aid */
    if (env && atol(env) > 0) {
        max_frames = atol(env);
        virtual_time = 1;
    }

    _buildargc = argc;
    _buildargv = (const char **)argv;

    baselayer_init();
    return app_main(_buildargc, (char const * const *)_buildargv);
}
