/* app_win32.c — native Win32 window + Vulkan (via flux) + lens_input.
 *
 * The Windows backend for iris, mirroring app_wayland.c's structure and
 * behaviour: one overlapped window, a MsgWaitForMultipleObjectsEx + PeekMessage
 * pump that carries the same frame-pacing state machine (active ~60 Hz after
 * input, ~4 Hz idle while a caret blinks, fully event-driven when static),
 * IMM32 for IME, CF_UNICODETEXT for the clipboard, and
 * VK_KHR_win32_surface to hand flux core a VkSurfaceKHR. Pointer and
 * keyboard events are folded into one lens_input per frame (ADR-0029).
 *
 * NOT YET VERIFIED ON A REAL WINDOWS MACHINE. Everything in this file is
 * compile-checked for x86_64-windows-gnu via tools/zig-win32-check.sh only.
 * The parts that most need on-hardware validation:
 *   - IMM32 composition/candidate behaviour against real IMEs (MS Pinyin,
 *     Japanese IME, Korean IME): GCS_CURSORPOS units, ATTR target-clause
 *     runs, candidate-window placement via CFS_POINT / CFS_CANDIDATEPOS.
 *   - PerMonitorV2 DPI transitions (WM_DPICHANGED ordering vs WM_SIZE,
 *     drag across mixed-DPI monitors, non-integer scales like 150 %).
 *   - Smooth-wheel (high-resolution) mouse deltas: the steps/pixels split
 *     documented in on_mouse_wheel.
 *   - Fullscreen style/placement restore, and DWM interactions of
 *     vsync=false (MAILBOX) present on a borderless-ish swapchain.
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 /* Windows 10: PerMonitorV2 DPI, GetDpiForWindow, … */
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

/* meson supplies -DIRIS_BUILDING=1 (iris_c_args) so IRIS_API spells
 * dllexport; define it here as well so single-file compile checks
 * (tools/zig-win32-check.sh) don't see dllimport on definitions. */
#ifndef IRIS_BUILDING
#define IRIS_BUILDING 1
#endif

#include "a11y_prefs_internal.h"
#include "platform_input.h"
#include "platform_internal.h"
#include "platform_text.h"
#include "platform_wakeup.h"
#include "theme_watch_internal.h"

#include <iris/a11y.h>
#include <iris/a11y_prefs.h>
#include <iris/cursor.h>
#include <iris/dnd.h>
#include <iris/theme.h>

#include <flux/flux.h>
#include <flux/vulkan.h>

/* clang-format off */
/* windows.h MUST precede imm.h, ole2.h, shellapi.h, and vulkan_win32.h:
 * imm.h uses the base handle macros (DECLARE_HANDLE, HKL, UINT, DWORD,
 * POINT, RECT) without defining them. MinGW-w64 headers tolerate the
 * reverse order by accident; zig cc's bundled MinGW headers do not. */
#include <windows.h>
#include <windowsx.h>
#include <imm.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
/* clang-format on */

/* Win32 platform surface glue: included as the platform child header (the
 * way app_wayland.c pulls <vulkan/vulkan_wayland.h>), after windows.h has
 * provided the native handle types. */
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Internal seam with theme_win32.c: called from the WndProc on
 * WM_SETTINGCHANGE. Runs on the loop thread, so the watcher callback is
 * invoked directly (theme.h's main-thread delivery guarantee) without
 * detouring through the wakeup seam. */
void iris_theme_win32__notify_setting_change(void);

/* Internal seam with a11y_prefs_win32.c: same delivery as above, for the
 * accessibility preference set (reduced motion / high contrast / text
 * scale). Declared in a11y_prefs_internal.h when _WIN32 is defined; this
 * forward declaration keeps the WndProc's call site self-describing. */
void iris_a11y_prefs_win32__notify_setting_change(void);

/* Backend-reserved thread/window messages (WM_APP band). WM_IRIS_WAKEUP is
 * the wakeup-seam kick (PostThreadMessage; hwnd == NULL). WM_IRIS_PASTE_DELIVER
 * carries an asynchronous clipboard payload (PostMessage to the window). */
#define WM_IRIS_WAKEUP (WM_APP + 0x100)
#define WM_IRIS_PASTE_DELIVER (WM_APP + 0x101)

#define IRIS_WNDCLASS_NAME L"iris_window"

/* Smooth-wheel deltas below one notch are reported on lens's pixel channel
 * at this many logical pixels per notch. 40 matches the lens-side consumer
 * convention (slider.c divides scroll_pixels_y by 40). */
#define W32_WHEEL_PIXELS_PER_NOTCH 40.0

/* ------------------------------------------------------------------ */
/*  Accumulated input, drained into one lens_input per frame             */
/* ------------------------------------------------------------------ */

typedef struct w32_accum {
    double cx, cy;                  /* latest cursor, client logical px */
    bool down[LENS_MOUSE_COUNT];    /* held state (persists)          */
    bool pressed[LENS_MOUSE_COUNT]; /* edges this frame               */
    bool released[LENS_MOUSE_COUNT];
    double scroll_x, scroll_y;       /* wheel notches this frame       */
    double scroll_px_x, scroll_px_y; /* smooth pixel deltas this frame */
    uint32_t mods;                   /* level state (persists)         */
    char text[256];                  /* committed text this frame; must match
                                       lens_input.text_utf8 (static_assert below) */
    char preedit[LENS_PREEDIT_MAX];  /* IME preedit string             */
    uint32_t preedit_cursor;         /* caret byte offset in preedit   */
    uint32_t preedit_sel_lo;         /* active clause, byte range      */
    uint32_t preedit_sel_hi;
    lens_key_event keys[LENS_INPUT_MAX_KEYS];
    uint32_t key_count;
} w32_accum;

/* The staging buffers feed lens_input by whole-buffer memcpy in
 * drain_input; pin the sizes so a lens-side change fails at compile time
 * instead of over-reading the accumulator (the wayland backend carries
 * the same guards). */
static_assert(sizeof((w32_accum *)0)->text == sizeof((lens_input *)0)->text_utf8,
              "w32_accum.text must match lens_input.text_utf8");
static_assert(sizeof((w32_accum *)0)->preedit == sizeof((lens_input *)0)->preedit_utf8,
              "w32_accum.preedit must match lens_input.preedit_utf8");

/* ------------------------------------------------------------------ */
/*  Platform state                                                     */
/* ------------------------------------------------------------------ */

typedef struct w32_platform {
    HINSTANCE hinstance;
    HWND hwnd;
    DWORD loop_thread_id; /* for the wakeup-seam kick (PostThreadMessage) */

    int width, height; /* client size in *logical* pixels              */
    int min_w, min_h;  /* size hints (logical); 0 = unset              */
    int max_w, max_h;
    float scale;         /* device pixels per logical pixel (dpi / 96) */
    float pending_scale; /* from WM_DPICHANGED, applied next frame     */

    bool running;
    bool resized;                   /* size or scale changed -> resize swapchain */
    bool minimized;                 /* SIZE_MINIMIZED: no valid render target    */
    bool force_paint;               /* WM_PAINT asked for a real frame      */
    bool animation_frame_requested; /* host asked for active-rate follow-up */
    bool paint_static;              /* host declared this frame's canvas static */
    bool frame_skip_render;         /* host asked to skip rendering but keep the active cadence */
    bool theme_watching;            /* backend theme watch registered       */
    bool a11y_prefs_watching;       /* backend a11y-pref watch registered   */
    bool a11y_running;              /* a11y bridge initialised (inert stub) */
    lens *ui;                       /* for caret/hint/paste on the loop thread */

    iris_cursor host_cursor;      /* last iris_set_cursor value (explicit) */
    iris_cursor effective_cursor; /* what WM_SETCURSOR currently applies   */
    bool cursor_inside;           /* pointer currently over the client area */
    bool tracking_leave;          /* TrackMouseEvent(TME_LEAVE) armed       */

    /* Fullscreen: style/placement saved on entry, restored on exit. */
    bool fullscreen;
    DWORD saved_style;
    DWORD saved_ex_style;
    WINDOWPLACEMENT saved_placement;

    /* IME (IMM32). The design keeps the whole composition in w32_accum so a
     * future TSF bridge can replace the message handlers without touching
     * the frame loop. */
    bool ime_composing;
    uint32_t pending_high_surrogate; /* carried UTF-16 lead surrogate, 0 = none */

    /* Drag-and-drop state (ADR-0086) */
    iris_dnd_source drag_source;
    bool drag_active;

    w32_accum acc;
} w32_platform;

/* iris_set_cursor / iris_window_* are public, context-free APIs; they reach
 * the active app's platform through this single static pointer. Set at the
 * top of iris_app_run_win32, cleared before teardown. Documented as
 * thread-affine: callers must drive them from the iris_app_run thread. */
static w32_platform *g_active_pl = NULL;

/* Internal seam for file_dialog_win32.c: the common file dialogs take an
 * owner HWND so they stay modal to the app window. Not a public symbol. */
HWND iris_win32__dialog_owner(void) {
    return g_active_pl ? g_active_pl->hwnd : NULL;
}

void iris_request_animation_frame_win32(void) {
    w32_platform *pl = g_active_pl;
    if (pl)
        pl->animation_frame_requested = true;
}

void iris_paint_mark_static_win32(void) {
    w32_platform *pl = g_active_pl;
    if (pl)
        pl->paint_static = true;
}

void iris_request_frame_skip_render_win32(void) {
    w32_platform *pl = g_active_pl;
    if (pl)
        pl->frame_skip_render = true;
}

/* Theme watcher callback: invoked on the iris main thread (from the WndProc
 * via theme_win32.c's notify hook) when the system colour scheme flips. */
static void w32_on_color_scheme_changed(iris_color_scheme scheme, void *user) {
    w32_platform *pl = user;
    if (!pl || !pl->ui)
        return;
    lens_theme th =
        (scheme == IRIS_COLOR_SCHEME_PREFER_LIGHT) ? lens_theme_default() : lens_theme_dark();
    lens_set_theme(pl->ui, th);
}

/* Accessibility-preference watcher callback (ADR-0075): delivered from the
 * WndProc's WM_SETTINGCHANGE via a11y_prefs_win32.c's notify hook — already
 * on the iris main thread. Applies the OS accessibility preferences to
 * lens directly (lens is headless and cannot see them itself). */
static void w32_on_a11y_prefs(const iris_a11y_prefs *prefs, void *user) {
    w32_platform *pl = user;
    if (!pl || !pl->ui)
        return;
    lens_set_reduced_motion(pl->ui, prefs->reduced_motion);
    lens_set_text_scale(pl->ui, prefs->text_scale);
    if (prefs->high_contrast) {
        lens_theme th = lens_get_theme(pl->ui);
        th.color_bg = flux_color_rgba(0x00, 0x00, 0x00, 0xff);
        th.color_fg = flux_color_rgba(0xff, 0xff, 0xff, 0xff);
        th.color_hover = flux_color_rgba(0x2a, 0x2a, 0x2a, 0xff);
        th.color_active = flux_color_rgba(0x55, 0x55, 0x55, 0xff);
        th.color_border = th.color_fg;
        th.color_disabled = flux_color_rgba(0xaa, 0xaa, 0xaa, 0xff);
        lens_set_theme(pl->ui, th);
    } else {
        w32_on_color_scheme_changed(iris_query_system_color_scheme(), pl);
    }
    /* Force a frame so the new preferences reach the screen even while the
     * loop is idle-parked. */
    pl->animation_frame_requested = true;
}

/* ------------------------------------------------------------------ */
/*  UTF-8 / UTF-16 helpers                                              */
/* ------------------------------------------------------------------ */

/* Encode one scalar value as UTF-8. Returns the byte count (1..4). Callers
 * guarantee cp is a valid scalar (surrogates are paired before this). */
static int utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Append UTF-8 text to the per-frame commit accumulator (cap 255 + NUL,
 * matching lens_input.text_utf8; enforced by the static_assert at the
 * w32_accum definition), truncating on a code-point boundary (shared
 * helper, platform_text.h). Same cap as app_wayland.c. */
static void accum_text_append(w32_platform *pl, const char *utf8, size_t n) {
    iris_utf8_append(pl->acc.text, sizeof pl->acc.text, utf8, n);
}

static void accum_text_cp(w32_platform *pl, uint32_t cp) {
    /* Control characters (Tab / Return / Backspace) travel on the key path,
     * matching the other backends. iris_cp_is_text also rejects 0x7f:
     * TranslateMessage delivers WM_CHAR 0x7f for Ctrl+Backspace, and the
     * plain < 0x20 test let it through as "text". */
    if (!iris_cp_is_text(cp))
        return;
    char buf[4];
    int n = utf8_encode(cp, buf);
    accum_text_append(pl, buf, (size_t)n);
}

/* UTF-8 -> freshly allocated NUL-terminated WCHAR (NULL on failure).
 * Caller frees with free(). */
static WCHAR *w32_wide_from_utf8(const char *utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    WCHAR *w = malloc((size_t)n * sizeof *w);
    if (!w)
        return NULL;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
    return w;
}

/* ------------------------------------------------------------------ */
/*  Input mapping (platform_input.h contract)                         */
/* ------------------------------------------------------------------ */

/* Win32 button message → lens mouse index, via the shared iris_pointer_button
 * layer (platform_input.h) so the Win32-only codes stay inside this file.
 * X1/X2 (WM_XBUTTON*) and anything unknown return -1 and are ignored. */
static int mouse_index(UINT msg) {
    iris_pointer_button b;
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        b = IRIS_POINTER_BUTTON_LEFT;
        break;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        b = IRIS_POINTER_BUTTON_RIGHT;
        break;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        b = IRIS_POINTER_BUTTON_MIDDLE;
        break;
    default:
        b = IRIS_POINTER_BUTTON_UNKNOWN;
        break;
    }
    return iris_pointer_button_to_lens(b);
}

/* Virtual-key code → lens key: LENS_KEY_* sentinels for the navigation set,
 * lowercase ASCII for letters/digits/space (matching the Wayland backend's
 * unshifted xkb keysyms so Ctrl+C/V/X/A shortcuts line up), raw VK codes
 * (< 256, below the sentinel band) for everything else. Pure modifiers and
 * IME process keys return 0 = not recorded (they live in lens_input.mods). */
static int vk_to_lens_key(WPARAM vk) {
    switch (vk) {
    case VK_ESCAPE:
        return LENS_KEY_ESCAPE;
    case VK_TAB:
        return LENS_KEY_TAB;
    case VK_RETURN:
        return LENS_KEY_RETURN;
    case VK_BACK:
        return LENS_KEY_BACKSPACE;
    case VK_DELETE:
        return LENS_KEY_DELETE;
    case VK_LEFT:
        return LENS_KEY_LEFT;
    case VK_RIGHT:
        return LENS_KEY_RIGHT;
    case VK_UP:
        return LENS_KEY_UP;
    case VK_DOWN:
        return LENS_KEY_DOWN;
    case VK_HOME:
        return LENS_KEY_HOME;
    case VK_END:
        return LENS_KEY_END;
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
    case VK_PROCESSKEY: /* IME-owned key; the result arrives via IME/WM_CHAR */
        return 0;
    default:
        break;
    }
    if (vk >= 'A' && vk <= 'Z')
        return 'a' + (int)(vk - 'A');
    if (vk >= '0' && vk <= '9')
        return (int)vk;
    if (vk == VK_SPACE)
        return ' ';
    return (int)vk;
}

/* Modifier level state, sampled from the thread's key state. Called from the
 * input message handlers, where GetKeyState reflects the state at the time
 * the message was generated. */
static void update_mods(w32_platform *pl) {
    uint32_t m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000)
        m |= LENS_MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000)
        m |= LENS_MOD_CTRL;
    if (GetKeyState(VK_MENU) & 0x8000)
        m |= LENS_MOD_ALT;
    if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000))
        m |= LENS_MOD_SUPER;
    pl->acc.mods = m;
}

static void accum_key(w32_platform *pl, int key, bool pressed, bool repeat) {
    if (pl->acc.key_count >= LENS_INPUT_MAX_KEYS)
        return;
    pl->acc.keys[pl->acc.key_count++] =
        (lens_key_event){.key = key, .pressed = pressed, .repeat = repeat};
}

static void on_key(w32_platform *pl, WPARAM vk, LPARAM lp, bool down_edge) {
    update_mods(pl);
    int key = vk_to_lens_key(vk);
    if (!key)
        return;
    /* lParam bit 30: previous key state — 1 on auto-repeat WM_KEYDOWN. */
    bool repeat = down_edge && (lp & (1u << 30)) != 0;
    accum_key(pl, key, down_edge, repeat);
}

/* WM_CHAR carries a UTF-16 code unit; surrogate pairs arrive as two
 * consecutive messages. Suppressed entirely while an IME composition is
 * active: committed text then arrives via GCS_RESULTSTR instead, and taking
 * both would double-insert. */
static void on_char(w32_platform *pl, WPARAM wParam) {
    if (pl->ime_composing)
        return;
    uint32_t wc = (uint32_t)(WCHAR)wParam;
    if (pl->pending_high_surrogate) {
        uint32_t hi = pl->pending_high_surrogate;
        pl->pending_high_surrogate = 0;
        if (wc >= 0xDC00 && wc <= 0xDFFF) {
            accum_text_cp(pl, 0x10000u + ((hi - 0xD800u) << 10) + (wc - 0xDC00u));
            return;
        }
        /* Lone lead surrogate: drop it, fall through with this code unit. */
    }
    if (wc >= 0xD800 && wc <= 0xDBFF) {
        pl->pending_high_surrogate = wc;
        return;
    }
    if (wc >= 0xDC00 && wc <= 0xDFFF)
        return; /* lone trail surrogate */
    accum_text_cp(pl, wc);
}

static void on_mouse_button(w32_platform *pl, UINT msg, bool down_edge) {
    int i = mouse_index(msg);
    if (i < 0)
        return;
    update_mods(pl);
    if (down_edge) {
        if (!pl->acc.down[i])
            pl->acc.pressed[i] = true;
        pl->acc.down[i] = true;
        /* Implicit grab: keep receiving moves/up outside the client area
         * while a button is held (what the Wayland seat gives us for free). */
        SetCapture(pl->hwnd);
    } else {
        if (pl->acc.down[i])
            pl->acc.released[i] = true;
        pl->acc.down[i] = false;
        if (!pl->acc.down[LENS_MOUSE_LEFT] && !pl->acc.down[LENS_MOUSE_RIGHT] &&
            !pl->acc.down[LENS_MOUSE_MIDDLE])
            ReleaseCapture();
    }
}

/* Wheel: WM_MOUSEWHEEL positive = wheel up (away from the user), which is
 * lens's positive scroll_y direction, so vertical deltas pass straight
 * through; WM_MOUSEHWHEEL positive = tilt right, which the Wayland backend
 * reports as negative scroll_x, so horizontal is inverted. Deltas that are
 * an exact multiple of WHEEL_DELTA go to the step channel (scroll_x/y);
 * smaller high-resolution deltas go to the pixel channel (scroll_pixels_*).
 * lens consumes both channels additively (slider.c, textarea.c), so a delta
 * must land in exactly one — splitting by magnitude avoids double-counting
 * while keeping smooth wheels smooth. */
static void on_mouse_wheel(w32_platform *pl, WPARAM wParam, bool horizontal) {
    int delta = GET_WHEEL_DELTA_WPARAM(wParam);
    update_mods(pl);
    double *steps = horizontal ? &pl->acc.scroll_x : &pl->acc.scroll_y;
    double *pixels = horizontal ? &pl->acc.scroll_px_x : &pl->acc.scroll_px_y;
    double notch = (double)delta / (double)WHEEL_DELTA;
    if (horizontal)
        notch = -notch;
    if (delta % WHEEL_DELTA == 0)
        *steps += notch;
    else
        *pixels += notch * W32_WHEEL_PIXELS_PER_NOTCH;
}

/* ------------------------------------------------------------------ */
/*  IME (IMM32)                                                        */
/* ------------------------------------------------------------------ */

/* Point the IME's composition/candidate windows at the lens caret. The caret
 * rect is in logical UI space; IMM32 wants physical client coordinates. */
static void ime_update_position(w32_platform *pl) {
    if (!pl->ui)
        return;
    flux_rect caret = lens_caret_rect(pl->ui);
    if (caret.w <= 0.0f)
        return;
    HIMC himc = ImmGetContext(pl->hwnd);
    if (!himc)
        return;
    LONG cx = (LONG)lroundf(caret.x * pl->scale);
    LONG cy = (LONG)lroundf(caret.y * pl->scale);
    COMPOSITIONFORM cf = {CFS_POINT, {cx, cy}, {0, 0, 0, 0}};
    ImmSetCompositionWindow(himc, &cf);
    CANDIDATEFORM cand = {0, CFS_CANDIDATEPOS, {cx, cy}, {0, 0, 0, 0}};
    ImmSetCandidateWindow(himc, &cand);
    ImmReleaseContext(pl->hwnd, himc);
}

/* WM_IME_COMPOSITION: pull the composition/result strings out of the input
 * context into the accumulator. Composition strings are read as UTF-16 and
 * transcoded; cursor/clause offsets are WCHAR indices, converted to UTF-8
 * byte offsets (lens's preedit contract) by re-encoding the prefix. */
static void ime_read_composition(w32_platform *pl, LPARAM flags) {
    HIMC himc = ImmGetContext(pl->hwnd);
    if (!himc)
        return;

    if (flags & GCS_RESULTSTR) {
        WCHAR wbuf[256];
        LONG bytes = ImmGetCompositionStringW(himc, GCS_RESULTSTR, wbuf, sizeof wbuf);
        if (bytes > 0) {
            char u8[768]; /* 256 WCHARs fit: BMP is 1 WCHAR -> 3 bytes */
            int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)(bytes / 2), u8, (int)sizeof u8,
                                        NULL, NULL);
            if (n > 0)
                accum_text_append(pl, u8, (size_t)n);
        }
        /* Result committed: the composition is over, preedit clears. */
        pl->acc.preedit[0] = '\0';
        pl->acc.preedit_cursor = 0;
        pl->acc.preedit_sel_lo = pl->acc.preedit_sel_hi = 0;
    }

    if (flags & (GCS_COMPSTR | GCS_CURSORPOS | GCS_COMPATTR)) {
        WCHAR wbuf[256];
        LONG bytes = ImmGetCompositionStringW(himc, GCS_COMPSTR, wbuf, sizeof wbuf);
        LONG wlen = bytes > 0 ? bytes / 2 : 0;
        char u8[768];
        int n = wlen > 0 ? WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)wlen, u8, (int)sizeof u8,
                                               NULL, NULL)
                         : 0;
        size_t kept = iris_utf8_floor_boundary(u8, (size_t)n, sizeof pl->acc.preedit - 1);
        memcpy(pl->acc.preedit, u8, kept);
        pl->acc.preedit[kept] = '\0';

        LONG cur = ImmGetCompositionStringW(himc, GCS_CURSORPOS, NULL, 0);
        if (cur < 0 || cur > wlen)
            cur = wlen;
        int cb = cur > 0 ? WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)cur, NULL, 0, NULL, NULL) : 0;
        pl->acc.preedit_cursor = (uint32_t)((size_t)cb <= kept ? (size_t)cb : kept);

        /* Active clause: the run of ATTR_TARGET_* bytes under conversion.
         * IMM32 reports one attribute byte per WCHAR. */
        pl->acc.preedit_sel_lo = pl->acc.preedit_sel_hi = 0;
        if (wlen > 0) {
            BYTE attr[256];
            LONG abytes = ImmGetCompositionStringW(himc, GCS_COMPATTR, attr, sizeof attr);
            LONG lo = -1, hi = -1;
            for (LONG i = 0; i < abytes; i++) {
                bool target =
                    attr[i] == ATTR_TARGET_CONVERTED || attr[i] == ATTR_TARGET_NOTCONVERTED;
                if (target && lo < 0)
                    lo = i;
                else if (!target && lo >= 0) {
                    hi = i;
                    break;
                }
            }
            if (lo >= 0) {
                if (hi < 0)
                    hi = abytes;
                int blo = WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)lo, NULL, 0, NULL, NULL);
                int bhi = WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)hi, NULL, 0, NULL, NULL);
                pl->acc.preedit_sel_lo = (uint32_t)((size_t)blo <= kept ? (size_t)blo : kept);
                pl->acc.preedit_sel_hi = (uint32_t)((size_t)bhi <= kept ? (size_t)bhi : kept);
            }
        }
    }

    ImmReleaseContext(pl->hwnd, himc);
}

/* ------------------------------------------------------------------ */
/*  Clipboard (CF_UNICODETEXT) — bridges lens_clipboard to Win32        */
/* ------------------------------------------------------------------ */

/* lens_clipboard.set_text — publish `utf8` as the system clipboard text. */
static void clip_set_text(const char *utf8, size_t len, void *user) {
    w32_platform *pl = user;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, NULL, 0);
    if (wlen <= 0)
        return;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, ((SIZE_T)wlen + 1) * sizeof(WCHAR));
    if (!hg)
        return;
    WCHAR *dst = GlobalLock(hg);
    if (!dst) {
        GlobalFree(hg);
        return;
    }
    MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, dst, wlen);
    dst[wlen] = L'\0';
    GlobalUnlock(hg);

    if (!OpenClipboard(pl->hwnd)) {
        GlobalFree(hg);
        return;
    }
    EmptyClipboard();
    /* On success the system owns the handle; on failure we still do. */
    if (!SetClipboardData(CF_UNICODETEXT, hg))
        GlobalFree(hg);
    CloseClipboard();
}

/* lens_clipboard.request_text — the lens contract is asynchronous: the
 * answer must arrive later via lens_paste, not synchronously inside the
 * request (Wayland's offer/read round-trip has the same shape). We read the
 * clipboard eagerly, then PostMessage the payload to ourselves; the pump
 * delivers it through lens_paste on the next loop iteration, outside any
 * lens_begin/end pair. */
static void clip_request_text(void *user) {
    w32_platform *pl = user;
    char *utf8 = NULL;
    if (OpenClipboard(pl->hwnd)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            const WCHAR *w = GlobalLock(h);
            if (w) {
                int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
                if (n > 1) { /* > 1: ignore the empty string */
                    utf8 = malloc((size_t)n);
                    if (utf8)
                        WideCharToMultiByte(CP_UTF8, 0, w, -1, utf8, n, NULL, NULL);
                }
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }
    if (utf8 && !PostMessageW(pl->hwnd, WM_IRIS_PASTE_DELIVER, 0, (LPARAM)utf8))
        free(utf8);
}

/* ------------------------------------------------------------------ */
/*  Cursor                                                             */
/* ------------------------------------------------------------------ */

/* iris_cursor → system cursor handle. LoadCursor(NULL, …) returns shared
 * system cursors that must not be destroyed, so this is cheap to call
 * per frame. */
static HCURSOR w32_cursor_handle(iris_cursor c) {
    switch (c) {
    case IRIS_CURSOR_TEXT:
        return LoadCursorW(NULL, IDC_IBEAM);
    case IRIS_CURSOR_POINTER:
        return LoadCursorW(NULL, IDC_HAND);
    case IRIS_CURSOR_BUSY:
        return LoadCursorW(NULL, IDC_WAIT);
    case IRIS_CURSOR_CROSSHAIR:
        return LoadCursorW(NULL, IDC_CROSS);
    case IRIS_CURSOR_NOT_ALLOWED:
        return LoadCursorW(NULL, IDC_NO);
    case IRIS_CURSOR_RESIZE_EW:
        return LoadCursorW(NULL, IDC_SIZEWE);
    case IRIS_CURSOR_RESIZE_NS:
        return LoadCursorW(NULL, IDC_SIZENS);
    case IRIS_CURSOR_DEFAULT:
    default:
        return LoadCursorW(NULL, IDC_ARROW);
    }
}

/* Public API. The host's explicit choice wins; when it is DEFAULT the
 * per-frame lens cursor hint drives the cursor instead (see the frame loop).
 * No-op when no app is active. */
IRIS_API void iris_set_cursor(iris_cursor cursor) {
    w32_platform *pl = g_active_pl;
    if (!pl)
        return;
    if (cursor < IRIS_CURSOR_DEFAULT)
        cursor = IRIS_CURSOR_DEFAULT;
    if (cursor == pl->host_cursor)
        return;
    pl->host_cursor = cursor;
    if (cursor != IRIS_CURSOR_DEFAULT) {
        pl->effective_cursor = cursor;
        if (pl->cursor_inside)
            SetCursor(w32_cursor_handle(cursor));
    }
}

/* ------------------------------------------------------------------ */
/*  Window state (iris/window.h)                                       */
/* ------------------------------------------------------------------ */

/* All iris_window_* APIs operate on g_active_pl — the platform the active
 * iris_app_run call owns. They are documented as thread-affine to
 * iris_app_run and a no-op when no app is active. */

IRIS_API void iris_window_minimize(void) {
    w32_platform *pl = g_active_pl;
    if (pl && pl->hwnd)
        ShowWindow(pl->hwnd, SW_MINIMIZE);
}

IRIS_API void iris_window_maximize(void) {
    w32_platform *pl = g_active_pl;
    if (pl && pl->hwnd)
        ShowWindow(pl->hwnd, SW_MAXIMIZE);
}

IRIS_API void iris_window_unmaximize(void) {
    w32_platform *pl = g_active_pl;
    if (pl && pl->hwnd && !pl->fullscreen)
        ShowWindow(pl->hwnd, SW_RESTORE);
}

IRIS_API void iris_window_fullscreen(void) {
    w32_platform *pl = g_active_pl;
    if (!pl || !pl->hwnd || pl->fullscreen)
        return;
    pl->saved_style = GetWindowLongW(pl->hwnd, GWL_STYLE);
    pl->saved_ex_style = GetWindowLongW(pl->hwnd, GWL_EXSTYLE);
    pl->saved_placement.length = sizeof pl->saved_placement;
    GetWindowPlacement(pl->hwnd, &pl->saved_placement);

    MONITORINFO mi = {0};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(pl->hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowLongW(pl->hwnd, GWL_STYLE, pl->saved_style & ~(DWORD)WS_OVERLAPPEDWINDOW);
    SetWindowLongW(pl->hwnd, GWL_EXSTYLE, pl->saved_ex_style & ~(DWORD)WS_EX_WINDOWEDGE);
    SetWindowPos(pl->hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    pl->fullscreen = true;
}

IRIS_API void iris_window_unfullscreen(void) {
    w32_platform *pl = g_active_pl;
    if (!pl || !pl->hwnd || !pl->fullscreen)
        return;
    pl->fullscreen = false;
    SetWindowLongW(pl->hwnd, GWL_STYLE, pl->saved_style);
    SetWindowLongW(pl->hwnd, GWL_EXSTYLE, pl->saved_ex_style);
    SetWindowPlacement(pl->hwnd, &pl->saved_placement); /* restores geometry + show state */
    SetWindowPos(pl->hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

IRIS_API void iris_window_restore(void) {
    w32_platform *pl = g_active_pl;
    if (!pl || !pl->hwnd)
        return;
    if (pl->fullscreen) {
        iris_window_unfullscreen();
        return;
    }
    ShowWindow(pl->hwnd, SW_RESTORE); /* un-minimises and un-maximises */
}

IRIS_API void iris_window_focus(void) {
    w32_platform *pl = g_active_pl;
    /* Best effort: Windows may only flash the taskbar button when the call
     * lacks a user activation — the same policy class as Wayland refusing
     * focus steals outright. */
    if (pl && pl->hwnd)
        SetForegroundWindow(pl->hwnd);
}

IRIS_API void iris_window_close(void) {
    w32_platform *pl = g_active_pl;
    if (pl)
        pl->running = false;
}

IRIS_API void iris_window_set_min_size(int32_t width, int32_t height) {
    w32_platform *pl = g_active_pl;
    if (!pl)
        return;
    pl->min_w = width;
    pl->min_h = height;
}

IRIS_API void iris_window_set_max_size(int32_t width, int32_t height) {
    w32_platform *pl = g_active_pl;
    if (!pl)
        return;
    pl->max_w = width;
    pl->max_h = height;
}

IRIS_API bool iris_window_get_geometry(int32_t *out_width, int32_t *out_height) {
    w32_platform *pl = g_active_pl;
    if (!pl)
        return false;
    if (out_width)
        *out_width = pl->width;
    if (out_height)
        *out_height = pl->height;
    return true;
}

/* ================================================================== */
/*  OLE / COM Drag-and-Drop Subsystem (ADR-0086)                      */
/* ================================================================== */

/* --- Drop Target Implementation (Receiving drops) --- */

typedef struct w32_drop_target_impl {
    IDropTarget target;
    LONG ref_count;
    w32_platform *pl;
} w32_drop_target_impl;

static HRESULT STDMETHODCALLTYPE dt_QueryInterface(IDropTarget *This, REFIID riid,
                                                   void **ppvObject) {
    if (!ppvObject)
        return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropTarget)) {
        *ppvObject = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppvObject = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dt_AddRef(IDropTarget *This) {
    w32_drop_target_impl *impl = (w32_drop_target_impl *)This;
    return (ULONG)InterlockedIncrement(&impl->ref_count);
}

static ULONG STDMETHODCALLTYPE dt_Release(IDropTarget *This) {
    w32_drop_target_impl *impl = (w32_drop_target_impl *)This;
    LONG c = InterlockedDecrement(&impl->ref_count);
    return (ULONG)(c > 0 ? c : 0);
}

static HRESULT STDMETHODCALLTYPE dt_DragEnter(IDropTarget *This, IDataObject *pDataObj,
                                              DWORD grfKeyState, POINTL pt, DWORD *pdwEffect) {
    (void)This;
    (void)pDataObj;
    (void)grfKeyState;
    (void)pt;
    if (pdwEffect)
        *pdwEffect &= (DROPEFFECT_COPY | DROPEFFECT_MOVE);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dt_DragOver(IDropTarget *This, DWORD grfKeyState, POINTL pt,
                                             DWORD *pdwEffect) {
    (void)This;
    (void)grfKeyState;
    (void)pt;
    if (pdwEffect)
        *pdwEffect &= (DROPEFFECT_COPY | DROPEFFECT_MOVE);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dt_DragLeave(IDropTarget *This) {
    (void)This;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dt_Drop(IDropTarget *This, IDataObject *pDataObj,
                                         DWORD grfKeyState, POINTL pt, DWORD *pdwEffect) {
    (void)grfKeyState;
    w32_drop_target_impl *impl = (w32_drop_target_impl *)This;
    if (!impl || !impl->pl || !impl->pl->ui || !pDataObj)
        return E_FAIL;

    POINT p = {pt.x, pt.y};
    ScreenToClient(impl->pl->hwnd, &p);
    float scale = (impl->pl->scale > 0.0f) ? impl->pl->scale : 1.0f;
    flux_point drop_pos = {(float)p.x / scale, (float)p.y / scale};

    FORMATETC fmt_text = {CF_UNICODETEXT, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM med = {0};
    if (SUCCEEDED(pDataObj->lpVtbl->GetData(pDataObj, &fmt_text, &med))) {
        const wchar_t *wstr = (const wchar_t *)GlobalLock(med.hGlobal);
        if (wstr) {
            int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
            if (utf8_len > 1) {
                char *utf8 = (char *)malloc((size_t)utf8_len);
                if (utf8) {
                    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8, utf8_len, NULL, NULL);
                    lens_paste(impl->pl->ui, utf8, (size_t)(utf8_len - 1));
                    lens_deliver_drop(impl->pl->ui, utf8, (size_t)(utf8_len - 1), drop_pos);
                    free(utf8);
                }
            }
            GlobalUnlock(med.hGlobal);
        }
        ReleaseStgMedium(&med);
    } else {
        FORMATETC fmt_file = {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        if (SUCCEEDED(pDataObj->lpVtbl->GetData(pDataObj, &fmt_file, &med))) {
            HDROP hdrop = (HDROP)GlobalLock(med.hGlobal);
            if (hdrop) {
                UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, NULL, 0);
                if (count > 0) {
                    wchar_t path_w[MAX_PATH];
                    if (DragQueryFileW(hdrop, 0, path_w, MAX_PATH) > 0) {
                        int utf8_len =
                            WideCharToMultiByte(CP_UTF8, 0, path_w, -1, NULL, 0, NULL, NULL);
                        if (utf8_len > 1) {
                            char *utf8 = (char *)malloc((size_t)utf8_len);
                            if (utf8) {
                                WideCharToMultiByte(CP_UTF8, 0, path_w, -1, utf8, utf8_len, NULL,
                                                    NULL);
                                lens_paste(impl->pl->ui, utf8, (size_t)(utf8_len - 1));
                                lens_deliver_drop(impl->pl->ui, utf8, (size_t)(utf8_len - 1),
                                                  drop_pos);
                                free(utf8);
                            }
                        }
                    }
                }
                GlobalUnlock(med.hGlobal);
            }
            ReleaseStgMedium(&med);
        }
    }

    if (pdwEffect)
        *pdwEffect = DROPEFFECT_COPY;
    return S_OK;
}

static IDropTargetVtbl g_drop_target_vtbl = {
    .QueryInterface = dt_QueryInterface,
    .AddRef = dt_AddRef,
    .Release = dt_Release,
    .DragEnter = dt_DragEnter,
    .DragOver = dt_DragOver,
    .DragLeave = dt_DragLeave,
    .Drop = dt_Drop,
};

/* --- Drop Source Implementation (Initiating drag) --- */

typedef struct w32_drop_source_impl {
    IDropSource source;
    LONG ref_count;
} w32_drop_source_impl;

static HRESULT STDMETHODCALLTYPE ds_QueryInterface(IDropSource *This, REFIID riid,
                                                   void **ppvObject) {
    if (!ppvObject)
        return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropSource)) {
        *ppvObject = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppvObject = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE ds_AddRef(IDropSource *This) {
    w32_drop_source_impl *impl = (w32_drop_source_impl *)This;
    return (ULONG)InterlockedIncrement(&impl->ref_count);
}

static ULONG STDMETHODCALLTYPE ds_Release(IDropSource *This) {
    w32_drop_source_impl *impl = (w32_drop_source_impl *)This;
    LONG c = InterlockedDecrement(&impl->ref_count);
    return (ULONG)(c > 0 ? c : 0);
}

static HRESULT STDMETHODCALLTYPE ds_QueryContinueDrag(IDropSource *This, BOOL fEscapePressed,
                                                      DWORD grfKeyState) {
    (void)This;
    if (fEscapePressed)
        return DRAGDROP_S_CANCEL;
    if (!(grfKeyState & (MK_LBUTTON | MK_RBUTTON)))
        return DRAGDROP_S_DROP;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ds_GiveFeedback(IDropSource *This, DWORD dwEffect) {
    (void)This;
    (void)dwEffect;
    return DRAGDROP_S_USEDEFAULTCURSORS;
}

static IDropSourceVtbl g_drop_source_vtbl = {
    .QueryInterface = ds_QueryInterface,
    .AddRef = ds_AddRef,
    .Release = ds_Release,
    .QueryContinueDrag = ds_QueryContinueDrag,
    .GiveFeedback = ds_GiveFeedback,
};

/* --- Data Object Implementation (Payload container) --- */

typedef struct w32_data_object_impl {
    IDataObject data_obj;
    LONG ref_count;
    wchar_t *text_w;
} w32_data_object_impl;

static HRESULT STDMETHODCALLTYPE do_QueryInterface(IDataObject *This, REFIID riid,
                                                   void **ppvObject) {
    if (!ppvObject)
        return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDataObject)) {
        *ppvObject = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppvObject = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE do_AddRef(IDataObject *This) {
    w32_data_object_impl *impl = (w32_data_object_impl *)This;
    return (ULONG)InterlockedIncrement(&impl->ref_count);
}

static ULONG STDMETHODCALLTYPE do_Release(IDataObject *This) {
    w32_data_object_impl *impl = (w32_data_object_impl *)This;
    LONG c = InterlockedDecrement(&impl->ref_count);
    return (ULONG)(c > 0 ? c : 0);
}

static HRESULT STDMETHODCALLTYPE do_GetData(IDataObject *This, FORMATETC *pformatetcIn,
                                            STGMEDIUM *pmedium) {
    w32_data_object_impl *impl = (w32_data_object_impl *)This;
    if (!pformatetcIn || !pmedium)
        return E_POINTER;
    if (pformatetcIn->cfFormat == CF_UNICODETEXT && (pformatetcIn->tymed & TYMED_HGLOBAL) &&
        impl->text_w) {
        size_t len = (wcslen(impl->text_w) + 1) * sizeof(wchar_t);
        HGLOBAL hg = GlobalAlloc(GHND, len);
        if (!hg)
            return E_OUTOFMEMORY;
        void *dst = GlobalLock(hg);
        if (!dst) {
            GlobalFree(hg);
            return E_OUTOFMEMORY;
        }
        memcpy(dst, impl->text_w, len);
        GlobalUnlock(hg);
        pmedium->tymed = TYMED_HGLOBAL;
        pmedium->hGlobal = hg;
        pmedium->pUnkForRelease = NULL;
        return S_OK;
    }
    return DV_E_FORMATETC;
}

static HRESULT STDMETHODCALLTYPE do_GetDataHere(IDataObject *This, FORMATETC *pformatetc,
                                                STGMEDIUM *pmedium) {
    (void)This;
    (void)pformatetc;
    (void)pmedium;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE do_QueryGetData(IDataObject *This, FORMATETC *pformatetc) {
    w32_data_object_impl *impl = (w32_data_object_impl *)This;
    if (!pformatetc)
        return E_POINTER;
    if (pformatetc->cfFormat == CF_UNICODETEXT && (pformatetc->tymed & TYMED_HGLOBAL) &&
        impl->text_w)
        return S_OK;
    return DV_E_FORMATETC;
}

static HRESULT STDMETHODCALLTYPE do_GetCanonicalFormatEtc(IDataObject *This,
                                                          FORMATETC *pformatectIn,
                                                          FORMATETC *pformatetcOut) {
    (void)This;
    (void)pformatectIn;
    (void)pformatetcOut;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE do_SetData(IDataObject *This, FORMATETC *pformatetc,
                                            STGMEDIUM *pmedium, BOOL fRelease) {
    (void)This;
    (void)pformatetc;
    (void)pmedium;
    (void)fRelease;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE do_EnumFormatEtc(IDataObject *This, DWORD dwDirection,
                                                  IEnumFORMATETC **ppenumFormatEtc) {
    (void)This;
    (void)dwDirection;
    if (ppenumFormatEtc)
        *ppenumFormatEtc = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE do_DAdvise(IDataObject *This, FORMATETC *pformatetc, DWORD advf,
                                            IAdviseSink *pAdvSink, DWORD *pdwConnection) {
    (void)This;
    (void)pformatetc;
    (void)advf;
    (void)pAdvSink;
    (void)pdwConnection;
    return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE do_DUnadvise(IDataObject *This, DWORD dwConnection) {
    (void)This;
    (void)dwConnection;
    return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE do_EnumDAdvise(IDataObject *This, IEnumSTATDATA **ppenumAdvise) {
    (void)This;
    if (ppenumAdvise)
        *ppenumAdvise = NULL;
    return OLE_E_ADVISENOTSUPPORTED;
}

static IDataObjectVtbl g_data_object_vtbl = {
    .QueryInterface = do_QueryInterface,
    .AddRef = do_AddRef,
    .Release = do_Release,
    .GetData = do_GetData,
    .GetDataHere = do_GetDataHere,
    .QueryGetData = do_QueryGetData,
    .GetCanonicalFormatEtc = do_GetCanonicalFormatEtc,
    .SetData = do_SetData,
    .EnumFormatEtc = do_EnumFormatEtc,
    .DAdvise = do_DAdvise,
    .DUnadvise = do_DUnadvise,
    .EnumDAdvise = do_EnumDAdvise,
};

IRIS_API int iris_dnd_start(const iris_dnd_source *source) {
    w32_platform *pl = g_active_pl;
    if (!pl || !pl->hwnd || !source)
        return -1;

    pl->drag_source = *source;
    pl->drag_active = true;

    wchar_t *wstr = NULL;
    if (source->static_text && source->static_text_len) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, source->static_text,
                                       (int)source->static_text_len, NULL, 0);
        if (wlen > 0) {
            wstr = (wchar_t *)malloc((size_t)(wlen + 1) * sizeof(wchar_t));
            if (wstr) {
                MultiByteToWideChar(CP_UTF8, 0, source->static_text, (int)source->static_text_len,
                                    wstr, wlen);
                wstr[wlen] = L'\0';
            }
        }
    }

    w32_data_object_impl data_obj = {
        .data_obj = {.lpVtbl = &g_data_object_vtbl},
        .ref_count = 1,
        .text_w = wstr,
    };
    w32_drop_source_impl drop_src = {
        .source = {.lpVtbl = &g_drop_source_vtbl},
        .ref_count = 1,
    };

    DWORD effect = 0;
    DWORD ok_effects = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    HRESULT hr =
        DoDragDrop((IDataObject *)&data_obj, (IDropSource *)&drop_src, ok_effects, &effect);

    pl->drag_active = false;
    free(wstr);

    if (hr == DRAGDROP_S_DROP) {
        if (source->callbacks.finished)
            source->callbacks.finished(IRIS_DND_ACTION_COPY, source->callbacks.user);
        return 0;
    } else {
        if (source->callbacks.cancelled)
            source->callbacks.cancelled(source->callbacks.user);
        return (hr == DRAGDROP_S_CANCEL) ? 0 : -1;
    }
}

IRIS_API bool iris_dnd_is_active(void) {
    w32_platform *pl = g_active_pl;
    return pl && pl->drag_active;
}

IRIS_API void iris_dnd_cancel(void) {
    w32_platform *pl = g_active_pl;
    if (pl)
        pl->drag_active = false;
}

/* ------------------------------------------------------------------ */
/*  Window procedure                                                   */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK w32_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    w32_platform *pl = (w32_platform *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_NCCREATE:
        /* Publish pl for every later message; lpCreateParams carries it. */
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lParam)->lpCreateParams);
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_CLOSE:
        /* The window object outlives the loop; teardown destroys it. */
        if (pl)
            pl->running = false;
        return 0;
    case WM_DESTROY:
        if (pl)
            pl->running = false;
        return 0;

    case WM_PAINT: {
        /* We draw through the Vulkan swapchain, not GDI — but the region
         * must be validated or Windows keeps sending WM_PAINT and DWM marks
         * the window unresponsive. Flag a real frame: after a restore or
         * composition change the swapchain contents may be stale even when
         * lens reports no damage. */
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        if (pl)
            pl->force_paint = true;
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; /* everything is repainted by flux */

    case WM_SIZE: {
        if (!pl)
            break;
        if (wParam == SIZE_MINIMIZED) {
            pl->minimized = true; /* zero-sized target: the loop sleeps */
            return 0;
        }
        pl->minimized = false;
        int pw = (int)(short)LOWORD(lParam); /* physical client pixels */
        int ph = (int)(short)HIWORD(lParam);
        int lw = (int)lroundf((float)pw / pl->scale);
        int lh = (int)lroundf((float)ph / pl->scale);
        if (lw > 0 && lh > 0 && (lw != pl->width || lh != pl->height)) {
            pl->width = lw;
            pl->height = lh;
            pl->resized = true;
        }
        return 0;
    }

    case WM_DPICHANGED: {
        if (!pl)
            break;
        /* HIWORD = Y dpi; PerMonitorV2 keeps X == Y. The suggested window
         * rect (physical) keeps the logical size across the move. WM_SIZE
         * follows and folds the new client size into the frame loop. */
        pl->pending_scale = (float)HIWORD(wParam) / 96.0f;
        const RECT *suggested = (const RECT *)lParam;
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        if (!pl)
            break;
        MINMAXINFO *mmi = (MINMAXINFO *)lParam;
        UINT dpi = (UINT)lroundf(pl->scale * 96.0f);
        DWORD style = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
        DWORD exstyle = (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE);
        /* Track sizes are window (not client) sizes: grow the logical
         * client bound by the frame at the current DPI. */
        if (pl->min_w > 0 && pl->min_h > 0) {
            RECT r = {0, 0, (LONG)lroundf(pl->min_w * pl->scale),
                      (LONG)lroundf(pl->min_h * pl->scale)};
            AdjustWindowRectExForDpi(&r, style, FALSE, exstyle, dpi);
            mmi->ptMinTrackSize.x = r.right - r.left;
            mmi->ptMinTrackSize.y = r.bottom - r.top;
        }
        if (pl->max_w > 0 && pl->max_h > 0) {
            RECT r = {0, 0, (LONG)lroundf(pl->max_w * pl->scale),
                      (LONG)lroundf(pl->max_h * pl->scale)};
            AdjustWindowRectExForDpi(&r, style, FALSE, exstyle, dpi);
            mmi->ptMaxTrackSize.x = r.right - r.left;
            mmi->ptMaxTrackSize.y = r.bottom - r.top;
        }
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            SetCursor(w32_cursor_handle(pl ? pl->effective_cursor : IRIS_CURSOR_DEFAULT));
            return TRUE;
        }
        break; /* non-client areas keep the default frame cursors */

    case WM_MOUSEMOVE:
        if (pl) {
            pl->acc.cx = (double)GET_X_LPARAM(lParam) / (double)pl->scale;
            pl->acc.cy = (double)GET_Y_LPARAM(lParam) / (double)pl->scale;
            if (!pl->cursor_inside) {
                pl->cursor_inside = true;
                SetCursor(w32_cursor_handle(pl->effective_cursor));
            }
            if (!pl->tracking_leave) {
                TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                pl->tracking_leave = true;
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        if (pl) {
            pl->tracking_leave = false;
            pl->cursor_inside = false;
            pl->acc.cx = pl->acc.cy = -100000.0; /* off-window: clears hover */
        }
        return 0;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        if (pl)
            on_mouse_button(pl, msg, true);
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        if (pl)
            on_mouse_button(pl, msg, false);
        return 0;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        /* X1/X2 have no lens slot (platform_input.h): dropped by mouse_index;
         * returning TRUE for XBUTTON is the documented convention. */
        return TRUE;

    case WM_MOUSEWHEEL:
        if (pl)
            on_mouse_wheel(pl, wParam, false);
        return 0;
    case WM_MOUSEHWHEEL:
        if (pl)
            on_mouse_wheel(pl, wParam, true);
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (pl)
            on_key(pl, wParam, lParam, true);
        break; /* DefWindowProc keeps Alt+F4 / system keys working */
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (pl)
            on_key(pl, wParam, lParam, false);
        break;

    case WM_CHAR:
        if (pl)
            on_char(pl, wParam);
        return 0;

    case WM_IME_STARTCOMPOSITION:
        if (pl) {
            pl->ime_composing = true;
            ime_update_position(pl);
        }
        /* lens renders the preedit itself; suppress the IME's default
         * composition string window (the candidate list still shows). */
        return 0;
    case WM_IME_COMPOSITION:
        if (pl)
            ime_read_composition(pl, lParam);
        return 0;
    case WM_IME_ENDCOMPOSITION:
        if (pl) {
            pl->ime_composing = false;
            pl->acc.preedit[0] = '\0';
            pl->acc.preedit_cursor = 0;
            pl->acc.preedit_sel_lo = pl->acc.preedit_sel_hi = 0;
        }
        return 0;

    case WM_SETTINGCHANGE:
        /* Theme watch: re-read AppsUseLightTheme and fire the registered
         * callback on change (theme_win32.c owns the compare). Already on
         * the loop thread — no wakeup-seam detour needed. */
        iris_theme_win32__notify_setting_change();
        /* Accessibility preferences: same broadcast, same thread —
         * SPI_GETCLIENTAREAANIMATION / SPI_GETHIGHCONTRASTW / text metrics
         * re-read and fired on change (a11y_prefs_win32.c owns the
         * compare). */
        iris_a11y_prefs_win32__notify_setting_change();
        break;

    case WM_IRIS_PASTE_DELIVER: {
        /* Asynchronous clipboard delivery (clip_request_text contract). */
        char *utf8 = (char *)lParam;
        if (pl && pl->ui && utf8)
            lens_paste(pl->ui, utf8, strlen(utf8));
        free(utf8);
        return 0;
    }

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/*  Build one lens_input from the accumulated state                      */
/* ------------------------------------------------------------------ */

static void drain_input(w32_platform *pl, lens_input *in, float dt) {
    memset(in, 0, sizeof *in);
    in->cursor = (flux_point){(float)pl->acc.cx, (float)pl->acc.cy};
    in->display_size = (flux_point){(float)pl->width, (float)pl->height};
    in->dt_seconds = dt;
    in->mods = pl->acc.mods;
    in->scroll_x = (float)pl->acc.scroll_x;
    in->scroll_y = (float)pl->acc.scroll_y;
    in->scroll_pixels_x = (float)pl->acc.scroll_px_x;
    in->scroll_pixels_y = (float)pl->acc.scroll_px_y;
    for (int i = 0; i < LENS_MOUSE_COUNT; i++) {
        in->mouse_down[i] = pl->acc.down[i];
        in->mouse_pressed[i] = pl->acc.pressed[i];
        in->mouse_released[i] = pl->acc.released[i];
    }
    memcpy(in->text_utf8, pl->acc.text, sizeof in->text_utf8);
    memcpy(in->preedit_utf8, pl->acc.preedit, sizeof in->preedit_utf8);
    in->preedit_cursor = pl->acc.preedit_cursor;
    in->preedit_sel_lo = pl->acc.preedit_sel_lo;
    in->preedit_sel_hi = pl->acc.preedit_sel_hi;
    in->key_count = pl->acc.key_count;
    for (uint32_t i = 0; i < pl->acc.key_count; i++)
        in->keys[i] = pl->acc.keys[i];
    /* IMM32 has no delete-surrounding concept (that is text-input-v3), so
     * ime_delete_before/after stay zero — the memset above covers them. */

    /* Clear per-frame edges; keep level state (down/cursor/mods/preedit —
     * the preedit persists until the IME updates or ends it). */
    for (int i = 0; i < LENS_MOUSE_COUNT; i++)
        pl->acc.pressed[i] = pl->acc.released[i] = false;
    pl->acc.scroll_x = pl->acc.scroll_y = 0.0;
    pl->acc.scroll_px_x = pl->acc.scroll_px_y = 0.0;
    pl->acc.key_count = 0;
    pl->acc.text[0] = '\0';
}

/* Diagnostics go to stderr, never stdout: a consumer's stdout may be an
 * IPC wire (see flux_console_logger). */
static void log_raw(const lens_input *in) {
    static const char *names[LENS_MOUSE_COUNT] = {"left", "right", "middle"};
    for (int b = 0; b < LENS_MOUSE_COUNT; b++) {
        if (in->mouse_pressed[b])
            fprintf(stderr, "[raw] mouse press   %s\n", names[b]);
        if (in->mouse_released[b])
            fprintf(stderr, "[raw] mouse release %s\n", names[b]);
    }
    if (in->scroll_x != 0.0f || in->scroll_y != 0.0f)
        fprintf(stderr, "[raw] scroll dx=%.2f dy=%.2f\n", in->scroll_x, in->scroll_y);
    if (in->scroll_pixels_x != 0.0f || in->scroll_pixels_y != 0.0f)
        fprintf(stderr, "[raw] scroll pixels dx=%.2f dy=%.2f\n", in->scroll_pixels_x,
                in->scroll_pixels_y);
    for (uint32_t k = 0; k < in->key_count; k++)
        fprintf(stderr, "[raw] key %d %s%s\n", in->keys[k].key,
                in->keys[k].pressed ? "down" : "up  ", in->keys[k].repeat ? " (repeat)" : "");
}

/* ------------------------------------------------------------------ */
/*  Message pump + frame pacing                                        */
/* ------------------------------------------------------------------ */

/* Kick for the cross-thread wakeup seam (platform_wakeup.h): callable from
 * any thread, never blocks, and wakes a loop blocked in pump_events.
 * PostThreadMessage is thread-safe by design; coalescing in the queue is
 * fine because the drain empties the whole callback queue. */
static void w32_wakeup_kick(void *user) {
    w32_platform *pl = user;
    PostThreadMessageW(pl->loop_thread_id, WM_IRIS_WAKEUP, 0, 0);
}

/* Block (up to timeout_ms) for thread/window messages, then drain the queue.
 * This is the Win32 form of app_wayland.c's pump_events: MsgWaitForMultipleObjectsEx
 * plays poll(2), PeekMessage(PM_REMOVE) plays wl_display_dispatch_pending.
 * Returns true when any message was dispatched (an event wake, as opposed to
 * a deadline expiry), so the frame loop can tell the two apart. */
static bool pump_events(w32_platform *pl, DWORD timeout_ms) {
    (void)MsgWaitForMultipleObjectsEx(0, NULL, timeout_ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    bool any = false;
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        any = true;
        if (msg.message == WM_QUIT) {
            pl->running = false;
            break;
        }
        /* Cross-thread wakeup: a subsystem posted a callback from its own
         * thread. Thread messages have no HWND and must not go through
         * DispatchMessage — drain the queue here, on the loop thread. */
        if (msg.message == WM_IRIS_WAKEUP && msg.hwnd == NULL) {
            iris_platform_wakeup_drain();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return any;
}

/* Monotonic nanoseconds, for frame pacing (QueryPerformanceCounter). */
static long long now_ns(void) {
    static long long freq = 0;
    if (!freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart;
    }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (long long)(c.QuadPart / freq) * 1000000000LL +
           (long long)((c.QuadPart % freq) * 1000000000LL / freq);
}

/* True if the input accumulator holds genuine user activity (pointer motion,
 * buttons, scroll, keys, or text/IME) since the last check. Same role as in
 * app_wayland.c: it is what lets the loop drop to a low idle rate when the
 * user is idle. `*pcx`/`*pcy` carry the previous cursor position. */
static bool acc_has_user_input(w32_platform *pl, double *pcx, double *pcy) {
    bool moved = (pl->acc.cx != *pcx) || (pl->acc.cy != *pcy);
    *pcx = pl->acc.cx;
    *pcy = pl->acc.cy;
    bool edge = pl->acc.key_count != 0 || pl->acc.text[0] != '\0' || pl->acc.preedit[0] != '\0' ||
                pl->acc.scroll_x != 0.0 || pl->acc.scroll_y != 0.0 || pl->acc.scroll_px_x != 0.0 ||
                pl->acc.scroll_px_y != 0.0;
    for (int i = 0; i < LENS_MOUSE_COUNT; i++)
        edge = edge || pl->acc.pressed[i] || pl->acc.released[i] || pl->acc.down[i];
    return moved || edge;
}

/* ------------------------------------------------------------------ */
/*  Vulkan surface helpers (platform-specific)                        */
/* ------------------------------------------------------------------ */

static VkSurfaceKHR w32_create_vk_surface(const flux_device *device, HINSTANCE hinstance,
                                          HWND hwnd) {
    VkWin32SurfaceCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        .hinstance = hinstance,
        .hwnd = hwnd,
    };
    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    if (vkCreateWin32SurfaceKHR(flux_device_vk_instance(device), &ci, NULL, &vk_surface) !=
        VK_SUCCESS)
        return VK_NULL_HANDLE;
    return vk_surface;
}

static void w32_destroy_vk_surface(const flux_device *device, VkSurfaceKHR vk_surface) {
    if (vk_surface && device)
        vkDestroySurfaceKHR(flux_device_vk_instance(device), vk_surface, NULL);
}

/* ------------------------------------------------------------------ */
/*  Run                                                                */
/* ------------------------------------------------------------------ */

static int lens_host_start_drag_win32(const char *text, size_t len, uint32_t actions, void *user) {
    (void)user;
    iris_dnd_source src = {
        .actions = actions,
        .static_text = text,
        .static_text_len = len,
    };
    return iris_dnd_start(&src);
}

int iris_app_run_win32(const iris_app_config *cfg) {
    /* All resources declared up front and NULL/VK_NULL_HANDLE-initialized so
     * a single `fail:` cleanup block can run on every exit path (error or
     * success) without referencing out-of-scope or indeterminate pointers. */
    flux_device *device = NULL;
    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    flux_surface *surface = NULL;
    flux_canvas *canvas = NULL;
    lens *ui = NULL;
    bool host_started = false;
    int rc = 1; /* pessimistic; set to 0 only on success */

    w32_platform pl = {
        .running = true,
        .width = cfg->width > 0 ? cfg->width : 960,
        .height = cfg->height > 0 ? cfg->height : 720,
        .scale = 1.0f,
        .pending_scale = 1.0f,
        .host_cursor = IRIS_CURSOR_DEFAULT,
        .effective_cursor = IRIS_CURSOR_DEFAULT,
        .acc = {.cx = -100000.0, .cy = -100000.0}, /* outside until first move */
    };

    /* Publish `pl` as the active app instance so the context-free
     * iris_set_cursor() / iris_window_*() can reach it. Cleared on the way
     * out (success or fail). */
    g_active_pl = &pl;

    OleInitialize(NULL);

    w32_drop_target_impl drop_target = {
        .target = {.lpVtbl = &g_drop_target_vtbl},
        .ref_count = 1,
        .pl = &pl,
    };

    /* Per-monitor DPI awareness must be opted into before any window exists
     * (a process manifest would be stronger still, but iris is a library and
     * cannot ship one for the host). Failure on pre-1607 Windows is ignored:
     * the app simply stays system-DPI-scaled by DWM. */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    pl.hinstance = GetModuleHandleW(NULL);
    pl.loop_thread_id = GetCurrentThreadId();

    /* Cross-thread wakeup seam: the kick posts WM_IRIS_WAKEUP to this
     * thread's message queue; pump_events drains the callback queue when it
     * retrieves one. Registered before any subsystem can post. */
    iris_platform_wakeup_set_kick(w32_wakeup_kick, &pl);

    /* --- Window class + window ------------------------------------- */
    WNDCLASSEXW wc = {
        .cbSize = sizeof wc,
        .style = CS_HREDRAW | CS_VREDRAW,
        .lpfnWndProc = w32_wnd_proc,
        .hInstance = pl.hinstance,
        .hCursor = LoadCursorW(NULL, IDC_ARROW),
        .hbrBackground = NULL, /* no GDI background: WM_ERASEBKGND returns 1 */
        .lpszClassName = IRIS_WNDCLASS_NAME,
    };
    (void)RegisterClassExW(&wc); /* ERROR_CLASS_ALREADY_EXISTS is fine */

    /* Initial scale: the system DPI (primary display). If the window lands
     * on a different-DPI monitor it is corrected right after creation. */
    UINT sys_dpi = GetDpiForSystem();
    if (sys_dpi == 0)
        sys_dpi = 96;
    pl.scale = pl.pending_scale = (float)sys_dpi / 96.0f;

    const DWORD style = WS_OVERLAPPEDWINDOW;
    const DWORD exstyle = WS_EX_APPWINDOW; /* guarantee a taskbar button */
    RECT want = {0, 0, (LONG)lroundf((float)pl.width * pl.scale),
                 (LONG)lroundf((float)pl.height * pl.scale)};
    AdjustWindowRectExForDpi(&want, style, FALSE, exstyle, sys_dpi);

    WCHAR *title_w = w32_wide_from_utf8(cfg->title ? cfg->title : "iris");
    pl.hwnd = CreateWindowExW(exstyle, IRIS_WNDCLASS_NAME, title_w ? title_w : L"iris", style,
                              CW_USEDEFAULT, CW_USEDEFAULT, want.right - want.left,
                              want.bottom - want.top, NULL, NULL, pl.hinstance, &pl);
    free(title_w);
    if (!pl.hwnd) {
        fprintf(stderr, "CreateWindowExW failed (%lu)\n", GetLastError());
        goto fail;
    }

    RegisterDragDrop(pl.hwnd, (IDropTarget *)&drop_target);

    /* The window may have been created on a monitor whose DPI differs from
     * the system DPI: re-derive the scale and resize the window so the
     * client area keeps the host-requested logical size. */
    UINT wnd_dpi = GetDpiForWindow(pl.hwnd);
    if (wnd_dpi == 0)
        wnd_dpi = sys_dpi;
    pl.scale = pl.pending_scale = (float)wnd_dpi / 96.0f;
    if (wnd_dpi != sys_dpi) {
        RECT r = {0, 0, (LONG)lroundf((float)pl.width * pl.scale),
                  (LONG)lroundf((float)pl.height * pl.scale)};
        AdjustWindowRectExForDpi(&r, style, FALSE, exstyle, wnd_dpi);
        SetWindowPos(pl.hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    /* --- Vulkan instance via flux (Win32 WSI extensions) ------------ */
    const char *inst_exts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };
    const char *dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    flux_device_desc ddesc = {
        .type = FLUX_TYPE_DEVICE_DESC,
        .log = flux_console_logger,
        .validation = FLUX_VALIDATION_AUTO,
        .required_instance_extensions = inst_exts,
        .required_instance_extension_count = sizeof inst_exts / sizeof *inst_exts,
        .required_device_extensions = dev_exts,
        .required_device_extension_count = sizeof dev_exts / sizeof *dev_exts,
        .frames_in_flight = 2,
    };
    if (flux_device_create(&ddesc, &device) != FLUX_OK) {
        fprintf(stderr, "flux_device_create failed\n");
        goto fail;
    }

    /* --- Vulkan surface + flux surface/canvas ----------------------- */
    vk_surface = w32_create_vk_surface(device, pl.hinstance, pl.hwnd);
    if (!vk_surface) {
        fprintf(stderr, "vkCreateWin32SurfaceKHR failed\n");
        goto fail;
    }

    /* Swapchain is sized in *device* pixels — logical × scale — while
     * layout and lens_input stay in logical units. */
    flux_surface_desc sdesc = {
        .type = FLUX_TYPE_SURFACE_DESC,
        .vk_surface_khr = vk_surface,
        .width = (uint32_t)lroundf((float)pl.width * pl.scale),
        .height = (uint32_t)lroundf((float)pl.height * pl.scale),
        /* Non-blocking present (MAILBOX, or IMMEDIATE as fallback — see
         * pick_present_mode in flux). A FIFO/vsync present blocks the main
         * thread in vkAcquireNextImageKHR, freezing input handling and
         * time-based UI between frames; the frame loop paces itself
         * instead (see below). */
        .vsync = false,
    };
    if (flux_surface_create(device, &sdesc, &surface) != FLUX_OK) {
        fprintf(stderr, "flux_surface_create failed\n");
        goto fail;
    }

    if (flux_canvas_create(&(flux_canvas_desc){.type = FLUX_TYPE_CANVAS_DESC, .surface = surface},
                           &canvas) != FLUX_OK) {
        fprintf(stderr, "flux_canvas_create failed\n");
        goto fail;
    }

    if (lens_create(&(lens_desc){.device = device,
                                 .theme = cfg->dark
                                              ? lens_theme_dark()
                                              : (iris_system_prefers_dark() ? lens_theme_dark()
                                                                            : lens_theme_default()),
                                 .scale = pl.scale,
                                 .clipboard = {.set_text = clip_set_text,
                                               .request_text = clip_request_text,
                                               .user = &pl},
                                 .dnd = {.start_drag = lens_host_start_drag_win32, .user = &pl}},
                    &ui) != FLUX_OK) {
        fprintf(stderr, "lens_create failed\n");
        goto fail;
    }

    if (cfg->start && !cfg->start(ui, device, cfg->user)) {
        fprintf(stderr, "iris host start callback failed\n");
        goto fail;
    }
    host_started = true;

    ShowWindow(pl.hwnd, SW_SHOW);

    /* --- Frame loop --------------------------------------------------
     * Same state machine as app_wayland.c, transplanted onto the Win32
     * message pump: the surface presents without vsync (non-blocking), so
     * the loop sleeps inside pump_events' MsgWaitForMultipleObjectsEx until
     * the next frame is due — or until real input wakes it early. The rate
     * is adaptive: ~60 Hz right after user input, then lens_frame_needs_repaint()
     * gates the whole acquire/paint/present cycle, and with nothing
     * time-driven pending (no animation, no focused caret) the loop stops
     * scheduling frames and blocks until the next message. A focused text
     * field keeps a ~4 Hz deadline alive for the caret blink.
     *
     * Hosts with a paint callback (cfg->paint) opt out of all of this:
     * their content is opaque to lens, so they keep the always-render
     * pacing (~60 Hz after input, ~4 Hz idle). */
    const long long ACTIVE_PERIOD_NS = 16666667LL; /* ~60 Hz when active    */
    const long long IDLE_PERIOD_NS = 250000000LL;  /* ~4 Hz when idle: just  */
                                                   /* enough for the caret   */
                                                   /* blink (500ms period).  */
    const long long INPUT_GRACE_NS = 400000000LL;  /* stay fast 400ms after  */

    long long prev_ns = now_ns();
    int frame_no = 0;

    long long next_deadline = prev_ns;
    bool frame_scheduled = true; /* the first frame must always paint */
    /* Sticky until a frame containing the latest lens build is actually
     * presented (see app_wayland.c for the acquire-timeout rationale). */
    bool surface_needs_paint = true;
    long long last_input_ns = next_deadline;
    long long last_render_ns = next_deadline - ACTIVE_PERIOD_NS;
    double prev_cx = pl.acc.cx, prev_cy = pl.acc.cy;

    pl.ui = ui;

    /* Live colour-scheme watching: only when not forcing dark. On Win32 the
     * change signal (WM_SETTINGCHANGE) already arrives on this thread, so
     * the watcher is registration-only (theme_win32.c); the callback flips
     * the lens theme in place and the next frame renders the new palette.
     * The backend registers on its reserved internal slot
     * (theme_watch_internal.h), never the host's public watch slot. */
    pl.theme_watching = false;
    if (!cfg->dark)
        pl.theme_watching = (iris_theme__watch_backend(w32_on_color_scheme_changed, &pl) == 0);

    /* Accessibility preferences (ADR-0075): apply the OS's reduced-motion /
     * high-contrast / text-scale preferences to lens at startup, then watch
     * via WM_SETTINGCHANGE (backend slot, a11y_prefs_internal.h). Applied
     * unconditionally — accessibility is not opt-in. */
    {
        iris_a11y_prefs prefs = iris_a11y_prefs_query();
        lens_set_reduced_motion(ui, prefs.reduced_motion);
        lens_set_text_scale(ui, prefs.text_scale);
        if (prefs.high_contrast)
            w32_on_a11y_prefs(&prefs, &pl);
    }
    pl.a11y_prefs_watching = (iris_a11y_prefs__watch_backend(w32_on_a11y_prefs, &pl) == 0);

    /* Accessibility: the Win32 build links a11y_stub.c today, so all three
     * calls are inert no-ops (iris_a11y_init reports the bridge unavailable
     * and a11y_running stays false). The call sites deliberately mirror
     * app_cocoa.m so a future UI Automation bridge plugs in behind the same
     * public seam without backend changes. */
    pl.a11y_running = (iris_a11y_init() == 0);

    while (pl.running) {
        /* While minimised there is no valid render target (a zero-size
         * swapchain is invalid): sleep until a message (restore/close)
         * wakes us. WM_SIZE clears the flag and marks the resize. */
        if (pl.minimized) {
            (void)pump_events(&pl, INFINITE);
            continue;
        }

        /* Sleep (inside the wait) until the next frame is due, waking early
         * on any message. With no frame scheduled (fully idle) there is no
         * deadline at all — the wait blocks until an event. */
        long long t = now_ns();
        long long budget_ns = frame_scheduled ? next_deadline - t : 0;
        bool woke_on_event;
        if (!frame_scheduled) {
            woke_on_event = pump_events(&pl, INFINITE);
        } else if (budget_ns > 0) {
            DWORD budget_ms = (DWORD)(budget_ns / 1000000LL);
            woke_on_event = pump_events(&pl, budget_ms);
        } else {
            woke_on_event = pump_events(&pl, 0);
        }

        /* Real user input wakes us out of the idle rate: pull the next render
         * forward (but never sooner than one active period after the last one,
         * so a burst of motion events can't exceed ~60 Hz) and mark the moment
         * so we stay at the active rate through the grace window. */
        if (acc_has_user_input(&pl, &prev_cx, &prev_cy)) {
            last_input_ns = now_ns();
            long long earliest = last_render_ns + ACTIVE_PERIOD_NS;
            if (!frame_scheduled || next_deadline > earliest)
                next_deadline = earliest;
            frame_scheduled = true;
        } else if (woke_on_event && !frame_scheduled) {
            /* A non-input event (wakeup-posted callback, theme change,
             * WM_PAINT, …) while fully idle: run one frame so lens sees the
             * new state; whether it paints is decided by the repaint query
             * below. */
            next_deadline = now_ns();
            frame_scheduled = true;
        }

        /* Not time to draw yet — keep draining messages and sleeping. */
        if (!frame_scheduled || now_ns() < next_deadline)
            continue;

        /* Render this iteration. A tentative deadline is scheduled up front
         * (no catch-up bursts) so the error `continue`s in the present path
         * still retry; it is refined — or dropped entirely — after lens_end,
         * once the repaint query and animation state are known. A host
         * animation request made by the previous frame also keeps this
         * iteration at the active cadence. */
        t = now_ns();
        bool host_animating = pl.animation_frame_requested;
        pl.animation_frame_requested = false;
        long long period = (t - last_input_ns < INPUT_GRACE_NS || host_animating) ? ACTIVE_PERIOD_NS
                                                                                  : IDLE_PERIOD_NS;
        next_deadline = t + period;
        frame_scheduled = true;
        last_render_ns = t;

        /* DPI change (window dragged to a mixed-DPI monitor): apply the new
         * scale, resize the swapchain in device pixels, and tell lens so its
         * replay transform matches. The first frame after a resize/scale must
         * always paint (the swapchain contents were discarded). */
        bool resized_this_frame = false;
        if (pl.pending_scale > 0.0f && fabsf(pl.pending_scale - pl.scale) > 0.0001f) {
            pl.scale = pl.pending_scale;
            lens_set_scale(ui, pl.scale);
            pl.resized = true;
        }
        if (pl.resized) {
            (void)flux_surface_resize(surface, (uint32_t)lroundf((float)pl.width * pl.scale),
                                      (uint32_t)lroundf((float)pl.height * pl.scale));
            pl.resized = false;
            resized_this_frame = true;
        }

        long long now = now_ns();
        float dt = (float)(now - prev_ns) * 1e-9f;
        if (dt <= 0.0f)
            dt = 1.0f / 60.0f;
        prev_ns = now;

        lens_input in;
        drain_input(&pl, &in, dt);
        if (cfg->log_raw)
            log_raw(&in);

        lens_begin(ui, &in);
        if (cfg->build)
            cfg->build(ui, &in, cfg->user);
        lens_end(ui);

        /* Inert with a11y_stub.c; kept for a future UI Automation bridge
         * (same seam app_cocoa.m uses). */
        if (pl.a11y_running)
            iris_a11y_update(ui);

        /* Keep the IME composition/candidate windows glued to the caret
         * while a composition is active (the caret moves as text commits). */
        if (pl.ime_composing)
            ime_update_position(&pl);

        /* Cursor: an explicit host iris_set_cursor wins; while it is DEFAULT,
         * follow the semantic hint of the lens widget under the pointer
         * (I-beam over text fields, hand over clickable elements, …). */
        lens_cursor_hint hint = lens_get_cursor_hint(ui);
        iris_cursor eff = pl.host_cursor;
        if (eff == IRIS_CURSOR_DEFAULT) {
            switch (hint) {
            case LENS_CURSOR_POINTER:
                eff = IRIS_CURSOR_POINTER;
                break;
            case LENS_CURSOR_TEXT:
                eff = IRIS_CURSOR_TEXT;
                break;
            case LENS_CURSOR_RESIZE_EW:
                eff = IRIS_CURSOR_RESIZE_EW;
                break;
            case LENS_CURSOR_RESIZE_NS:
                eff = IRIS_CURSOR_RESIZE_NS;
                break;
            case LENS_CURSOR_DEFAULT:
            default:
                break;
            }
        }
        if (eff != pl.effective_cursor) {
            pl.effective_cursor = eff;
            if (pl.cursor_inside)
                SetCursor(w32_cursor_handle(eff));
        }

        /* A WM_PAINT since the last frame (restore, DWM composition change):
         * the swapchain contents may be stale even when lens sees no damage,
         * so latch a repaint like a resize would. */
        if (pl.force_paint) {
            pl.force_paint = false;
            surface_needs_paint = true;
        }

        /* Static-frame skip: when lens reports no damage, the host has no
         * paint callback, no host animation is in flight, and no resize /
         * scale just happened, the frame just built is pixel-identical to
         * what is on screen — skip the whole begin_frame → canvas → present
         * cycle (no swapchain image acquired, nothing committed).
         *
         * Hosts with a paint callback can opt into the same skip per frame
         * via iris_paint_mark_static(): only they know whether their opaque
         * content moved. The declaration is consumed here and must be
         * re-issued every frame; lens chrome damage, host animation,
         * resizes, and a forced WM_PAINT always paint regardless. */
        /* A static declaration is the host's final word for the frame's
         * canvas content, but it only covers the host's own pixels: lens
         * chrome damage still forces a paint, or a hover highlight would
         * freeze mid-transition while the host scene is static. Host
         * animation, resizes, and a forced WM_PAINT always paint too. */
        bool chrome_damaged = lens_frame_needs_repaint(ui);
        /* Zero-render skip (iris_request_frame_skip_render): the host
         * promises this frame's canvas content is unchanged while still
         * streaming at the media cadence. Mirrors the Wayland backend:
         * the prior-frame animation request does NOT veto a skip (the
         * request that arms the next active deadline would otherwise
         * kill every streamed skip), but lens chrome damage, resizes,
         * a forced WM_PAINT, and a never-yet-presented surface all
         * change what is on screen and force a real paint below. */
        bool surface_forced = resized_this_frame || surface_needs_paint || pl.force_paint;
        bool host_skip_render =
            cfg->paint != NULL && pl.frame_skip_render && !surface_forced && !chrome_damaged;
        pl.frame_skip_render = false;
        bool host_canvas_static = cfg->paint != NULL && pl.paint_static && !host_animating &&
                                  !resized_this_frame && !surface_needs_paint && !chrome_damaged &&
                                  !pl.force_paint;
        pl.paint_static = false;
        bool must_paint = !host_canvas_static && !host_skip_render &&
                          (cfg->paint != NULL || chrome_damaged || host_animating ||
                           resized_this_frame || surface_needs_paint);
        if (must_paint) {
            surface_needs_paint = true;
            flux_frame *frame = NULL;
            flux_result r = flux_surface_begin_frame(surface, NULL, &frame);
            if (r == FLUX_ERROR_SURFACE_LOST) {
                (void)flux_surface_resize(surface, (uint32_t)lroundf((float)pl.width * pl.scale),
                                          (uint32_t)lroundf((float)pl.height * pl.scale));
                continue;
            }
            if (r == FLUX_ERROR_INVALID_STATE)
                continue;
            /* Acquire timeout (display asleep or window occluded): not an
             * error — no swapchain image was consumed, so skip this frame and
             * retry on the next deadline instead of exiting the loop. */
            if (r == FLUX_ERROR_TIMEOUT)
                continue;
            if (r != FLUX_OK)
                break;

            flux_surface_info info;
            flux_surface_get_info(surface, &info);

            /* Clear to the current theme's body background; the host paint
             * callback (if any) draws *under* lens's chrome. */
            lens_theme th = lens_get_theme(ui);
            flux_color clear = th.color_bg;
            bool drew = false;
            if (flux_canvas_begin_frame(canvas, frame, &clear) == FLUX_OK) {
                if (cfg->paint)
                    cfg->paint(canvas, device, pl.scale, cfg->user);
                drew = lens_render(ui, canvas) == FLUX_OK;
                flux_canvas_end_frame(canvas);
            }

            if (flux_frame_submit(frame) != FLUX_OK)
                break;
            r = flux_frame_present(frame);
            if (r == FLUX_ERROR_SURFACE_LOST)
                (void)flux_surface_resize(surface, (uint32_t)lroundf((float)pl.width * pl.scale),
                                          (uint32_t)lroundf((float)pl.height * pl.scale));
            else if (r != FLUX_OK)
                break;
            else if (drew)
                surface_needs_paint = false;

            if (++frame_no == 1)
                fprintf(stderr, "first frame presented: %dx%d logical, %ux%u device (scale=%.2f)\n",
                        pl.width, pl.height, info.width, info.height, (double)pl.scale);
        }

        /* Refine the tentative deadline now that the repaint query and the
         * post-build animation state are known (identical policy to
         * app_wayland.c: active rate while input is warm or animation is in
         * flight, idle cadence for a focused caret, otherwise unschedule
         * and sleep in the message wait until the next event). Hosts with
         * a paint callback keep the always-render pacing unless they
         * declared this frame static. */
        if (cfg->paint && !host_canvas_static) {
            if (pl.animation_frame_requested)
                next_deadline = last_render_ns + ACTIVE_PERIOD_NS;
            frame_scheduled = true;
        } else if (cfg->paint) {
            /* Static-declaring host: keep the low idle tick so build/paint
             * keep running and the host can resume animating on its own;
             * only the GPU work skips. */
            next_deadline = t + IDLE_PERIOD_NS;
            frame_scheduled = true;
        } else if (t - last_input_ns < INPUT_GRACE_NS || pl.animation_frame_requested ||
                   lens_anim_pending(ui)) {
            next_deadline = t + ACTIVE_PERIOD_NS;
            frame_scheduled = true;
        } else if (lens_caret_rect(ui).w > 0.0f) {
            next_deadline = t + IDLE_PERIOD_NS;
            frame_scheduled = true;
        } else {
            frame_scheduled = false;
            /* Fully idle: release the text engine's high-water scratch
             * (ADR-0072 item 5). Mirrors the Wayland backend's idle
             * branch — the frame-pacing policy is ported line-by-line
             * across backends (ADR-0056 item 2). */
            lens_text_compact(ui);
        }
    }

    rc = 0; /* success — fall through to the unified cleanup below */

    /* --- Cleanup (shared by the success path and every `goto fail`) --- */
fail:
    /* Let the host release every resource created from iris's borrowed
     * device before that device, the lens context, or the canvas disappears.
     * Keep the active platform published during the callback so thread-affine
     * iris helpers remain valid through the host's teardown. */
    if (host_started && cfg->stop)
        cfg->stop(ui, device, cfg->user);

    /* Stop publishing this w32_platform to the context-free APIs — any host
     * call after this returns is a no-op rather than a use-after-free. */
    g_active_pl = NULL;

    /* GPU side first: let in-flight work finish before tearing down objects. */
    if (device)
        flux_device_wait_idle(device);
    if (ui)
        lens_destroy(ui);
    pl.ui = NULL; /* after this, queued main-thread callbacks must not touch lens */
    /* The Win32 theme watcher holds no thread or handle; unwatch just drops
     * the backend's internal registration (the host's public watch, if any,
     * is untouched). */
    if (pl.theme_watching)
        iris_theme__unwatch_backend();
    if (pl.a11y_prefs_watching)
        iris_a11y_prefs__unwatch_backend();
    /* Inert with a11y_stub.c; mirrors app_cocoa.m. */
    if (pl.a11y_running)
        iris_a11y_shutdown();
    /* Wakeup seam teardown: nothing can post after the unregistrations
     * above (Win32 has no watcher thread), except detached subsystem
     * threads — unregister the kick first so late posts fail cleanly, then
     * drain whatever was legitimately queued. */
    iris_platform_wakeup_set_kick(NULL, NULL);
    iris_platform_wakeup_drain();
    if (canvas)
        flux_canvas_destroy(canvas);
    if (surface)
        flux_surface_release(surface);
    if (vk_surface)
        w32_destroy_vk_surface(device, vk_surface);
    if (device)
        flux_device_release(device);
    if (pl.hwnd) {
        RevokeDragDrop(pl.hwnd);
        DestroyWindow(pl.hwnd);
    }
    OleUninitialize();
    return rc;
}
