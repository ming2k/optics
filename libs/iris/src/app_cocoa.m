/* app_cocoa.m — native macOS Cocoa window + Vulkan (Metal, via flux) +
 * lens_input. Mirrors app_wayland.c section-for-section:
 *
 *   connection → window → pointer → keyboard → IME → clipboard → frame
 *   pacing → run/teardown.
 *
 * Pure AppKit: a hand-rolled NSApplication loop (no NSApplicationMain) so the
 * frame-pacing policy stays identical to the other backends (ADR-0044). The
 * view is layer-backed by a CAMetalLayer and the Vulkan surface is created
 * with VK_EXT_metal_surface (MoltenVK). Pointer and keyboard events are
 * folded into one lens_input per frame (ADR-0029).
 *
 * Compiled with ARC (see the darwin branch in libs/iris/meson.build).
 *
 * ============================================================================
 *  UNVERIFIED ASSUMPTIONS (written on Linux without an Apple SDK; each needs
 *  on-hardware confirmation — see the task report):
 * ============================================================================
 *  1. nextEventMatchingMask:untilDate:inMode:dequeue: in NSDefaultRunLoopMode
 *     is our only wait primitive. Cross-thread wakeups use a posted
 *     NSEventTypeApplicationDefined event (postEvent:atStart: is documented
 *     by Apple as safe to call from any thread) instead of the
 *     CFRunLoopPerformBlock recipe in platform_wakeup.h: a PerformBlock
 *     scheduled on the main run loop would run during the wait but would NOT
 *     make nextEventMatchingMask return, so in the fully-idle state
 *     (distantFuture deadline) the frame loop would never wake to apply the
 *     posted change. The posted event both wakes the wait and gives the
 *     dispatch site a place to call iris_platform_wakeup_drain(). Semantics
 *     match the header contract (kick wakes the loop; drain runs queued
 *     callbacks on the loop thread).
 *  2. NSTextInputContext.handleEvent: drives the IME synchronously: it
 *     invokes our NSTextInputClient callbacks (insertText: / setMarkedText:…)
 *     before returning. keyDown: always forwards the lens key event unless a
 *     composition was active (the IME owns the key stream then), and skips
 *     manual text generation only when insertText: already delivered the
 *     text for that key — so a plain letter is both a lens key event and
 *     lens text, exactly like the Wayland backend.
 *  3. A layer-backed (wantsLayer=YES + makeBackingLayer) flipped view gets
 *     correct MoltenVK presentation orientation (AppKit marks the backing
 *     layer's contents as flipped; MoltenVK is expected to honour that). If
 *     the frame presents upside down on hardware, drop isFlipped and flip
 *     input Y coordinates manually instead.
 *  4. keyCode → sentinel mapping uses the hardware (layout-independent)
 *     Apple key codes (kVK_*); ASCII shortcuts come from
 *     charactersIgnoringModifiers so they match xkb's shifted-keysym
 *     behaviour on the Wayland backend.
 *  5. firstRectForCharacterRange:actualRange: returns screen coordinates via
 *     convertRect:toView:nil + convertRectToScreen:, which already account
 *     for the flipped view — no manual screen-height flip is applied.
 *  6. activateIgnoringOtherApps: (deprecated in macOS 14) still works; the
 *     14+ replacement (activateWithOptions:) would need an SDK-conditional
 *     call site. No menu bar is created: without an .app bundle the app is
 *     usable but Cmd+Q / Cmd+W key equivalents do nothing.
 *  7. Minimum supported runtime is macOS 10.14 (NSApplication.
 *     effectiveAppearance, NSTextInputClient as used here, clock_gettime).
 *  8. During a live window resize AppKit runs its own modal tracking loop;
 *     our frame loop stalls and the existing CAMetalLayer drawable is
 *     stretched until the drag ends (windowDidResize: re-fits the
 *     swapchain). This matches the simplest GLFW-style behaviour.
 *  9. cfg->app_id is unused on macOS (the bundle owns the app identity).
 */

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#include "platform_input.h"
#include "platform_internal.h"
#include "platform_text.h"
#include "platform_wakeup.h"
#include "theme_watch_internal.h"

#include <iris/a11y.h>
#include <iris/cursor.h>
#include <iris/theme.h>

#include <flux/flux.h>
#include <flux/vulkan.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_metal.h> /* vkCreateMetalSurfaceEXT (MoltenVK) */

#include <float.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ApplicationDefined event subtypes (NSEvent.subtype is a short — keep the
 * constants small positive integers). */
static const NSInteger kIrisEventSubtypeWakeup = 0x4952; /* 'IR' — wakeup seam */
static const NSInteger kIrisEventSubtypePaste = 0x4950;  /* 'IP' — clipboard   */

/* ------------------------------------------------------------------ */
/*  Accumulated input, drained into one lens_input per frame             */
/* ------------------------------------------------------------------ */

typedef struct cp_accum {
    double cx, cy;                  /* latest cursor, view-local (top-left) */
    bool down[LENS_MOUSE_COUNT];    /* held state (persists)                */
    bool pressed[LENS_MOUSE_COUNT]; /* edges this frame                     */
    bool released[LENS_MOUSE_COUNT];
    double scroll_x, scroll_y;      /* wheel steps this frame               */
    double scroll_px_x, scroll_px_y; /* precise (trackpad) points this frame */
    uint32_t mods;                  /* level state (persists)               */
    char text[32];                  /* committed text this frame            */
    char preedit[LENS_PREEDIT_MAX]; /* IME preedit string (level state)     */
    uint32_t preedit_cursor;        /* caret byte offset in preedit         */
    uint32_t preedit_sel_lo;        /* selection byte range in preedit      */
    uint32_t preedit_sel_hi;
    lens_key_event keys[LENS_INPUT_MAX_KEYS];
    uint32_t key_count;
} cp_accum;

/* ------------------------------------------------------------------ */
/*  Platform state                                                       */
/* ------------------------------------------------------------------ */

@class IrisView;

/* One ObjC object owns all per-app state. Using an NSObject (not a C struct
 * of ObjC pointers) keeps every NS reference ARC-managed; the C POD fields
 * live in @public ivars so the code stays shaped like app_wayland.c. */
@interface IrisPlatform : NSObject {
  @public
    NSWindow *window;
    IrisView *view;
    id win_delegate; /* IrisWindowDelegate *, kept opaque to dodge the cycle */

    lens *ui; /* set after lens_create; IME/theme callbacks check it */

    int width, height; /* window content size in *logical* pixels      */
    float scale;       /* current backing scale (device px per point)  */
    float pending_scale; /* last reported backingScaleFactor, 0 = none */

    int min_w, min_h; /* size hints last sent to the window            */
    int max_w, max_h; /* 0 = no limit                                   */

    bool running;
    bool resized;                 /* size or scale changed -> resize swap  */
    bool animation_frame_requested; /* host asked for active-rate follow-up */
    bool paint_static;              /* host declared this frame's canvas static */
    bool theme_watching;
    bool a11y_running;

    iris_cursor host_cursor;      /* last explicit iris_set_cursor value    */
    iris_cursor effective_cursor; /* host override, else lens widget hint   */

    NSString *pending_paste; /* clipboard text awaiting async lens_paste     */

    cp_accum acc;
}
@end

@implementation IrisPlatform
@end

/* iris_set_cursor is a public, context-free API; it reaches the active
 * app's platform through this single static pointer. Set at the top of
 * iris_app_run_cocoa, cleared at exit. Documented as thread-affine:
 * callers must drive it from the same thread that runs iris_app_run. */
static IrisPlatform *g_active_pl = nil;

void iris_request_animation_frame_cocoa(void) {
    IrisPlatform *pl = g_active_pl;
    if (pl)
        pl->animation_frame_requested = true;
}

void iris_paint_mark_static_cocoa(void) {
    IrisPlatform *pl = g_active_pl;
    if (pl)
        pl->paint_static = true;
}

/* ------------------------------------------------------------------ */
/*  Cursor (NSCursor)                                                  */
/* ------------------------------------------------------------------ */

/* Map the public cursor enum to the AppKit stock cursor. IRIS_CURSOR_BUSY
 * has no public AppKit equivalent (the spinning beach ball is not
 * settable); fall back to the arrow, as documented here. */
static NSCursor *cursor_for(iris_cursor c) {
    switch (c) {
    case IRIS_CURSOR_TEXT:
        return [NSCursor IBeamCursor];
    case IRIS_CURSOR_POINTER:
        return [NSCursor pointingHandCursor];
    case IRIS_CURSOR_BUSY:
        return [NSCursor arrowCursor]; /* documented fallback — no public busy cursor */
    case IRIS_CURSOR_CROSSHAIR:
        return [NSCursor crosshairCursor];
    case IRIS_CURSOR_NOT_ALLOWED:
        return [NSCursor operationNotAllowedCursor];
    case IRIS_CURSOR_RESIZE_EW:
        return [NSCursor resizeLeftRightCursor];
    case IRIS_CURSOR_RESIZE_NS:
        return [NSCursor resizeUpDownCursor];
    case IRIS_CURSOR_DEFAULT:
    default:
        return [NSCursor arrowCursor];
    }
}

/* Apply the effective cursor immediately. AppKit only honours -[NSCursor set]
 * while the pointer is over one of our windows during event processing; we
 * call this from the cursorUpdate:/mouseEntered: tracking events, from
 * iris_set_cursor, and from the per-frame hint follow-up, so the effective
 * change lands on the next pointer event at the latest. */
static void cursor_apply(IrisPlatform *pl) {
    [cursor_for(pl->effective_cursor) set];
}

/* Effective cursor: an explicit host iris_set_cursor wins; while it is
 * DEFAULT, follow the semantic hint of the lens widget under the pointer
 * (I-beam over text fields, hand over clickable elements, …). Same policy
 * as the Win32 backend. Returns true when the effective cursor changed. */
static bool cursor_follow_hint(IrisPlatform *pl) {
    iris_cursor eff = pl->host_cursor;
    if (eff == IRIS_CURSOR_DEFAULT && pl->ui) {
        switch (lens_get_cursor_hint(pl->ui)) {
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
    if (eff == pl->effective_cursor)
        return false;
    pl->effective_cursor = eff;
    return true;
}

/* Public API. No-op before iris_app_run starts or after it exits. */
IRIS_API void iris_set_cursor(iris_cursor cursor) {
    IrisPlatform *pl = g_active_pl;
    if (!pl)
        return;
    if (cursor < IRIS_CURSOR_DEFAULT)
        cursor = IRIS_CURSOR_DEFAULT;
    if (cursor == pl->host_cursor)
        return;
    pl->host_cursor = cursor;
    if (cursor_follow_hint(pl))
        cursor_apply(pl);
}

/* ------------------------------------------------------------------ */
/*  Window state (iris/window.h)                                       */
/* ------------------------------------------------------------------ */

/* All iris_window_* APIs operate on g_active_pl — the platform the active
 * iris_app_run call owns. They are documented as thread-affine to
 * iris_app_run and a no-op when no app is active. */

IRIS_API void iris_window_minimize(void) {
    IrisPlatform *pl = g_active_pl;
    if (pl && pl->window)
        [pl->window miniaturize:nil];
}

IRIS_API void iris_window_maximize(void) {
    IrisPlatform *pl = g_active_pl;
    if (pl && pl->window && ![pl->window isZoomed])
        [pl->window zoom:nil];
}

IRIS_API void iris_window_unmaximize(void) {
    IrisPlatform *pl = g_active_pl;
    if (pl && pl->window && [pl->window isZoomed])
        [pl->window zoom:nil];
}

IRIS_API void iris_window_fullscreen(void) {
    IrisPlatform *pl = g_active_pl;
    if (!pl || !pl->window)
        return;
    if (([pl->window styleMask] & NSWindowStyleMaskFullScreen) == 0)
        [pl->window toggleFullScreen:nil];
}

IRIS_API void iris_window_unfullscreen(void) {
    IrisPlatform *pl = g_active_pl;
    if (!pl || !pl->window)
        return;
    if (([pl->window styleMask] & NSWindowStyleMaskFullScreen) != 0)
        [pl->window toggleFullScreen:nil];
}

IRIS_API void iris_window_restore(void) {
    IrisPlatform *pl = g_active_pl;
    if (!pl || !pl->window)
        return;
    if ([pl->window isMiniaturized])
        [pl->window deminiaturize:nil];
    if (([pl->window styleMask] & NSWindowStyleMaskFullScreen) != 0)
        [pl->window toggleFullScreen:nil];
    if ([pl->window isZoomed])
        [pl->window zoom:nil];
}

IRIS_API void iris_window_focus(void) {
    IrisPlatform *pl = g_active_pl;
    if (!pl || !pl->window)
        return;
    [NSApp activateIgnoringOtherApps:YES];
    [pl->window makeKeyAndOrderFront:nil];
}

IRIS_API void iris_window_close(void) {
    IrisPlatform *pl = g_active_pl;
    if (pl)
        pl->running = false;
}

IRIS_API void iris_window_set_min_size(int32_t width, int32_t height) {
    IrisPlatform *pl = g_active_pl;
    if (!pl || !pl->window)
        return;
    pl->min_w = width;
    pl->min_h = height;
    /* {0,0} is AppKit's "no minimum" sentinel, matching the 0-clears
     * convention of iris/window.h. */
    [pl->window setContentMinSize:NSMakeSize(width > 0 ? width : 0, height > 0 ? height : 0)];
}

IRIS_API void iris_window_set_max_size(int32_t width, int32_t height) {
    IrisPlatform *pl = g_active_pl;
    if (!pl || !pl->window)
        return;
    pl->max_w = width;
    pl->max_h = height;
    /* AppKit has no "no maximum" sentinel; FLT_MAX is the de-facto one. */
    [pl->window setContentMaxSize:NSMakeSize(width > 0 ? width : FLT_MAX,
                                             height > 0 ? height : FLT_MAX)];
}

IRIS_API bool iris_window_get_geometry(int32_t *out_width, int32_t *out_height) {
    IrisPlatform *pl = g_active_pl;
    if (!pl)
        return false;
    if (out_width)
        *out_width = pl->width;
    if (out_height)
        *out_height = pl->height;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Pointer                                                            */
/* ------------------------------------------------------------------ */

/* NSEvent buttonNumber (0 = left, 1 = right, 2 = middle) → lens mouse
 * index, via the shared iris_pointer_button layer (platform_input.h).
 * Buttons ≥ 3 (X1/X2/…) return -1 and the event is dropped, as
 * app_wayland.c does for unknown evdev codes. */
static int mouse_index_from_event(NSEvent *event) {
    iris_pointer_button b;
    switch ([event buttonNumber]) {
    case 0:
        b = IRIS_POINTER_BUTTON_LEFT;
        break;
    case 1:
        b = IRIS_POINTER_BUTTON_RIGHT;
        break;
    case 2:
        b = IRIS_POINTER_BUTTON_MIDDLE;
        break;
    default:
        b = IRIS_POINTER_BUTTON_UNKNOWN;
        break;
    }
    return iris_pointer_button_to_lens(b);
}

static void update_cursor_pos(IrisPlatform *pl, NSEvent *event) {
    if (!pl->view)
        return;
    /* The view is flipped (isFlipped == YES), so the converted point is
     * already in lens's top-left-origin UI space — no Y inversion. */
    NSPoint p = [pl->view convertPoint:[event locationInWindow] fromView:nil];
    pl->acc.cx = p.x;
    pl->acc.cy = p.y;
}

static void mouse_button(IrisPlatform *pl, NSEvent *event, bool down) {
    int i = mouse_index_from_event(event);
    if (i < 0)
        return;
    if (down && !pl->acc.down[i])
        pl->acc.pressed[i] = true;
    if (!down && pl->acc.down[i])
        pl->acc.released[i] = true;
    pl->acc.down[i] = down;
}

/* ------------------------------------------------------------------ */
/*  Keyboard (NSEvent keyCode → lens sentinels)                         */
/* ------------------------------------------------------------------ */

/* Hardware (layout-independent) Apple virtual key codes for the keys lens
 * has sentinels for. Only the ones we map are listed. */
enum {
    kVK_Return = 0x24,
    kVK_Tab = 0x30,
    kVK_Delete = 0x33,        /* backspace */
    kVK_Escape = 0x35,
    kVK_ForwardDelete = 0x75,
    kVK_Home = 0x73,
    kVK_End = 0x77,
    kVK_LeftArrow = 0x7B,
    kVK_RightArrow = 0x7C,
    kVK_DownArrow = 0x7D,
    kVK_UpArrow = 0x7E,
    kVK_ANSI_KeypadEnter = 0x4C,
};

static int key_sentinel(unsigned short keyCode) {
    switch (keyCode) {
    case kVK_Escape:
        return LENS_KEY_ESCAPE;
    case kVK_Tab:
        return LENS_KEY_TAB;
    case kVK_Return:
    case kVK_ANSI_KeypadEnter:
        return LENS_KEY_RETURN;
    case kVK_Delete:
        return LENS_KEY_BACKSPACE;
    case kVK_ForwardDelete:
        return LENS_KEY_DELETE;
    case kVK_LeftArrow:
        return LENS_KEY_LEFT;
    case kVK_RightArrow:
        return LENS_KEY_RIGHT;
    case kVK_UpArrow:
        return LENS_KEY_UP;
    case kVK_DownArrow:
        return LENS_KEY_DOWN;
    case kVK_Home:
        return LENS_KEY_HOME;
    case kVK_End:
        return LENS_KEY_END;
    default:
        return 0;
    }
}

/* Append NUL-terminated UTF-8 to a capped accumulator buffer, truncating
 * on a code-point boundary (shared helper — the old byte-count cut could
 * split a multi-byte sequence and emit invalid UTF-8). */
static void acc_append_text(char *dst, size_t cap, const char *utf8) {
    iris_utf8_append(dst, cap, utf8, strlen(utf8));
}

/* ------------------------------------------------------------------ */
/*  View — input, IME (NSTextInputClient), CAMetalLayer backing         */
/* ------------------------------------------------------------------ */

@interface IrisView : NSView <NSTextInputClient> {
  @public
    /* Owned by iris_app_run_cocoa for the whole view lifetime; the view is
     * torn down first, so a weak back-pointer is safe. */
    __unsafe_unretained IrisPlatform *_platform;
    NSString *_marked_text; /* live IME composition, nil when none          */
    NSRange _selected_in_marked; /* caret/selection inside _marked_text     */
    bool _ime_delivered_this_event; /* insertText/setMarkedText fired during
                                     * the current keyDown: dispatch         */
}
- (instancetype)initWithFrame:(NSRect)frame platform:(IrisPlatform *)pl;
- (void)irisUpdateMetalLayer;
@end

@implementation IrisView

- (instancetype)initWithFrame:(NSRect)frame platform:(IrisPlatform *)pl {
    self = [super initWithFrame:frame];
    if (self) {
        _platform = pl;
        _selected_in_marked = NSMakeRange(NSNotFound, 0);
        /* Layer-backed by CAMetalLayer (makeBackingLayer below). */
        [self setWantsLayer:YES];
        NSTrackingAreaOptions opts = NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
                                     NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect |
                                     NSTrackingCursorUpdate;
        [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                           options:opts
                                                             owner:self
                                                          userInfo:nil]];
    }
    return self;
}

- (BOOL)isFlipped {
    /* lens's UI space is top-left-origin; flipping the view keeps input
     * coordinates and lens_caret_rect in the same space as view bounds. */
    return YES;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (CALayer *)makeBackingLayer {
    return [CAMetalLayer layer];
}

/* contentsScale + drawableSize in *device* pixels; the Vulkan swapchain is
 * sized to match drawableSize. Safe to call before the view has a window. */
- (void)irisUpdateMetalLayer {
    CALayer *layer = [self layer];
    if (![layer isKindOfClass:[CAMetalLayer class]])
        return;
    CAMetalLayer *metal = (CAMetalLayer *)layer;
    NSRect backing = [self convertRectToBacking:[self bounds]];
    if (backing.size.width < 1 || backing.size.height < 1)
        return;
    CGFloat factor = [self window] ? [[self window] backingScaleFactor] : 1.0;
    if (factor <= 0.0)
        factor = 1.0;
    [metal setContentsScale:factor];
    [metal setDrawableSize:backing.size];
}

/* --- Tracking / pointer ------------------------------------------- */

- (void)cursorUpdate:(NSEvent *)event {
    (void)event;
    if (_platform)
        cursor_apply(_platform);
}

- (void)mouseEntered:(NSEvent *)event {
    if (!_platform)
        return;
    update_cursor_pos(_platform, event);
    cursor_apply(_platform);
}

- (void)mouseExited:(NSEvent *)event {
    (void)event;
    if (!_platform)
        return;
    _platform->acc.cx = _platform->acc.cy = -100000.0; /* off-window: clears hover */
}

- (void)mouseMoved:(NSEvent *)event {
    if (_platform)
        update_cursor_pos(_platform, event);
}
- (void)mouseDragged:(NSEvent *)event {
    if (_platform)
        update_cursor_pos(_platform, event);
}
- (void)rightMouseDragged:(NSEvent *)event {
    if (_platform)
        update_cursor_pos(_platform, event);
}
- (void)otherMouseDragged:(NSEvent *)event {
    if (_platform)
        update_cursor_pos(_platform, event);
}

- (void)mouseDown:(NSEvent *)event {
    if (_platform) {
        update_cursor_pos(_platform, event);
        mouse_button(_platform, event, true);
    }
}
- (void)mouseUp:(NSEvent *)event {
    if (_platform) {
        update_cursor_pos(_platform, event);
        mouse_button(_platform, event, false);
    }
}
- (void)rightMouseDown:(NSEvent *)event {
    if (_platform) {
        update_cursor_pos(_platform, event);
        mouse_button(_platform, event, true);
    }
}
- (void)rightMouseUp:(NSEvent *)event {
    if (_platform) {
        update_cursor_pos(_platform, event);
        mouse_button(_platform, event, false);
    }
}
- (void)otherMouseDown:(NSEvent *)event {
    if (_platform) {
        update_cursor_pos(_platform, event);
        mouse_button(_platform, event, true);
    }
}
- (void)otherMouseUp:(NSEvent *)event {
    if (_platform) {
        update_cursor_pos(_platform, event);
        mouse_button(_platform, event, false);
    }
}

/* Scroll: two distinct channels. Trackpads (and Magic Mice) deliver precise
 * deltas in points — those land in lens_input.scroll_pixels_* so widgets do
 * not multiply finger motion by a line factor. Classic wheel mice deliver
 * line-step deltas — those land in scroll_x/y. Both follow the same sign
 * convention lens uses on Wayland (wheel-down / content-down is negative);
 * NSEvent deltas already honour the user's natural-scrolling preference, so
 * they are passed through un-inverted. Momentum phases arrive as precise
 * deltas and are passed through like any other trackpad motion. */
- (void)scrollWheel:(NSEvent *)event {
    if (!_platform)
        return;
    if ([event hasPreciseScrollingDeltas]) {
        _platform->acc.scroll_px_x += [event scrollingDeltaX];
        _platform->acc.scroll_px_y += [event scrollingDeltaY];
    } else {
        _platform->acc.scroll_x += [event deltaX];
        _platform->acc.scroll_y += [event deltaY];
    }
    (void)[event momentumPhase]; /* momentum is precise-delta traffic; handled above */
}

/* --- Keyboard ------------------------------------------------------ */

/* Manual key processing for events the IME is not composing over: lens key
 * sentinels from the hardware keyCode, ASCII from charactersIgnoringModifiers
 * (so Ctrl/Cmd+letter shortcuts match the xkb keysym behaviour of the
 * Wayland backend), and plain text from characters. skipText is set when the
 * input context already delivered this key's text via insertText: — the key
 * event still goes to lens (Wayland parity: a plain letter is both a key
 * event AND text), but the text must not be appended twice. */
- (void)irisHandleKey:(NSEvent *)event skipText:(BOOL)skipText {
    IrisPlatform *pl = _platform;
    if (!pl)
        return;

    int fk = key_sentinel([event keyCode]);
    if (!fk) {
        NSString *chars = [event charactersIgnoringModifiers];
        if ([chars length] == 1) {
            unichar c = [chars characterAtIndex:0];
            if (c >= 0x20 && c <= 0x7e)
                fk = (int)c;
        }
    }
    if (fk && pl->acc.key_count < LENS_INPUT_MAX_KEYS) {
        pl->acc.keys[pl->acc.key_count++] =
            (lens_key_event){.key = fk, .pressed = true, .repeat = [event isARepeat]};
    }

    /* Committed text, only when the input context did not already deliver
     * it and no Command / Control chord is down (otherwise Cmd+C would
     * insert "c"). Control characters (Return "\r", Tab "\t", DEL 0x7f)
     * are not text — the shared iris_cp_is_text predicate, so all three
     * backends agree. */
    NSEventModifierFlags mods = [event modifierFlags];
    if (skipText || (mods & (NSEventModifierFlagCommand | NSEventModifierFlagControl)))
        return;
    NSString *chars = [event characters];
    if ([chars length] > 0) {
        unichar c0 = [chars characterAtIndex:0];
        if (iris_cp_is_text(c0)) {
            const char *utf8 = [chars UTF8String];
            if (utf8)
                acc_append_text(pl->acc.text, sizeof pl->acc.text, utf8);
        }
    }
}

- (void)keyDown:(NSEvent *)event {
    IrisPlatform *pl = _platform;
    if (!pl)
        return;
    _ime_delivered_this_event = false;
    BOOL was_composing = [self hasMarkedText];
    /* Route through the input context so IMEs can start/update a
     * composition; the NSTextInputClient callbacks below fire synchronously
     * from inside this call. While a composition is active (running, or
     * just started/committed by this very key) the IME owns the key stream
     * — candidate-window navigation keys must not reach lens. */
    [[self inputContext] handleEvent:event];
    if (was_composing || [self hasMarkedText])
        return;
    [self irisHandleKey:event skipText:_ime_delivered_this_event];
}

- (void)keyUp:(NSEvent *)event {
    /* The input context sees the release first — some IMEs track key-up
     * state. Then report the release edge to lens: the keyboard contract
     * (platform_internal.h) requires both edges for every reported key,
     * with letters as unshifted lowercase ASCII (charactersIgnoringModifiers
     * is exactly that). Repeat is always false on release. */
    [[self inputContext] handleEvent:event];
    IrisPlatform *pl = _platform;
    if (!pl)
        return;
    int fk = key_sentinel([event keyCode]);
    if (!fk) {
        NSString *chars = [event charactersIgnoringModifiers];
        if ([chars length] == 1) {
            unichar c = [chars characterAtIndex:0];
            if (c >= 0x20 && c <= 0x7e)
                fk = (int)c;
        }
    }
    if (fk && pl->acc.key_count < LENS_INPUT_MAX_KEYS) {
        pl->acc.keys[pl->acc.key_count++] =
            (lens_key_event){.key = fk, .pressed = false, .repeat = false};
    }
}

- (void)flagsChanged:(NSEvent *)event {
    IrisPlatform *pl = _platform;
    if (!pl)
        return;
    NSEventModifierFlags f = [event modifierFlags];
    uint32_t m = 0;
    if (f & NSEventModifierFlagShift)
        m |= LENS_MOD_SHIFT;
    if (f & NSEventModifierFlagControl)
        m |= LENS_MOD_CTRL;
    if (f & NSEventModifierFlagOption)
        m |= LENS_MOD_ALT;
    if (f & NSEventModifierFlagCommand)
        m |= LENS_MOD_SUPER;
    pl->acc.mods = m;
}

/* --- NSTextInputClient (IME) --------------------------------------- */

/* Byte offset of character index `loc` in `s` (UTF-8), for preedit_cursor /
 * preedit_sel_*. */
static uint32_t utf8_byte_offset(NSString *s, NSUInteger loc) {
    if (loc > [s length])
        loc = [s length];
    return (uint32_t)[[s substringToIndex:loc] lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
}

- (void)irisUnmark {
    _marked_text = nil;
    _selected_in_marked = NSMakeRange(NSNotFound, 0);
    if (_platform) {
        _platform->acc.preedit[0] = '\0';
        _platform->acc.preedit_cursor = 0;
        _platform->acc.preedit_sel_lo = 0;
        _platform->acc.preedit_sel_hi = 0;
    }
}

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
    (void)replacementRange; /* lens textfields own the buffer; the IME commit
                             * is plain inserted text from our side          */
    IrisPlatform *pl = _platform;
    if (!pl)
        return;
    NSString *s = [string isKindOfClass:[NSAttributedString class]] ? [string string] : string;
    if (![s isKindOfClass:[NSString class]] || [s length] == 0)
        return;
    /* A commit ends any in-flight composition. */
    [self irisUnmark];
    const char *utf8 = [s UTF8String];
    if (utf8)
        acc_append_text(pl->acc.text, sizeof pl->acc.text, utf8);
    _ime_delivered_this_event = true;
}

- (void)setMarkedText:(id)string
          selectedRange:(NSRange)selectedRange
       replacementRange:(NSRange)replacementRange {
    (void)replacementRange;
    IrisPlatform *pl = _platform;
    if (!pl)
        return;
    NSString *s = [string isKindOfClass:[NSAttributedString class]] ? [string string] : string;
    if (![s isKindOfClass:[NSString class]] || [s length] == 0) {
        [self irisUnmark];
        _ime_delivered_this_event = true;
        return;
    }
    _marked_text = [s copy];

    const char *utf8 = [_marked_text UTF8String];
    if (utf8) {
        /* boundary-aware copy: a raw byte cap could split a multi-byte
         * sequence at LENS_PREEDIT_MAX and hand lens invalid UTF-8. */
        iris_utf8_copy(pl->acc.preedit, sizeof pl->acc.preedit, utf8);
    } else {
        pl->acc.preedit[0] = '\0';
    }

    NSUInteger loc = selectedRange.location;
    NSUInteger len = selectedRange.length;
    if (loc == NSNotFound || loc > [_marked_text length]) {
        loc = [_marked_text length];
        len = 0;
    }
    if (len > [_marked_text length] - loc)
        len = [_marked_text length] - loc;
    _selected_in_marked = NSMakeRange(loc, len);
    pl->acc.preedit_cursor = utf8_byte_offset(_marked_text, loc);
    pl->acc.preedit_sel_lo = utf8_byte_offset(_marked_text, loc);
    pl->acc.preedit_sel_hi = utf8_byte_offset(_marked_text, loc + len);
    /* The preedit buffer caps at LENS_PREEDIT_MAX - 1 bytes; an over-long
     * composition is truncated, so clamp the offsets into the truncated
     * string to keep them in range for lens. */
    size_t plen = strlen(pl->acc.preedit);
    if (pl->acc.preedit_cursor > plen)
        pl->acc.preedit_cursor = (uint32_t)plen;
    if (pl->acc.preedit_sel_lo > plen)
        pl->acc.preedit_sel_lo = (uint32_t)plen;
    if (pl->acc.preedit_sel_hi > plen)
        pl->acc.preedit_sel_hi = (uint32_t)plen;
    _ime_delivered_this_event = true;
}

- (void)unmarkText {
    [self irisUnmark];
}

- (BOOL)hasMarkedText {
    return [_marked_text length] > 0;
}

- (NSRange)markedRange {
    if ([_marked_text length] > 0)
        return NSMakeRange(0, [_marked_text length]);
    return NSMakeRange(NSNotFound, 0);
}

- (NSRange)selectedRange {
    if ([_marked_text length] > 0)
        return _selected_in_marked;
    /* lens exposes no app-level selection through this seam. */
    return NSMakeRange(NSNotFound, 0);
}

- (NSArray *)validAttributesForMarkedText {
    return @[];
}

- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range
                                                actualRange:(NSRangePointer)actualRange {
    /* lens owns the text buffer; we cannot hand out attributed substrings.
     * Returning nil is documented ("could not extract") and IMEs cope. */
    (void)range;
    if (actualRange)
        *actualRange = NSMakeRange(NSNotFound, 0);
    return nil;
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    /* lens has no point→text-index seam; 0 is a safe, in-range answer. */
    (void)point;
    return 0;
}

/* IME candidate-window anchor. lens_caret_rect is in view (UI) coordinates
 * — the view is flipped, so no Y inversion. convertRectToScreen: returns
 * global screen coordinates (bottom-left origin), which is exactly the
 * coordinate system this method must answer in; the flip is subsumed by the
 * conversion chain. */
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    if (actualRange)
        *actualRange = range;
    IrisPlatform *pl = _platform;
    if (!pl || !pl->ui || ![self window])
        return NSZeroRect;
    flux_rect caret = lens_caret_rect(pl->ui);
    if (caret.w <= 0.0f)
        return NSZeroRect;
    NSRect r = NSMakeRect(caret.x, caret.y, caret.w, caret.h);
    r = [self convertRect:r toView:nil];
    r = [[self window] convertRectToScreen:r];
    return r;
}

/* Swallow unhandled action selectors (moveLeft:, deleteBackward:, …) so
 * AppKit does not beep for keys lens already consumed through lens_input. */
- (void)doCommandBySelector:(SEL)selector {
    (void)selector;
}

@end

/* ------------------------------------------------------------------ */
/*  Window delegate                                                    */
/* ------------------------------------------------------------------ */

@interface IrisWindowDelegate : NSObject <NSWindowDelegate> {
  @public
    __unsafe_unretained IrisPlatform *_platform;
}
- (instancetype)initWithPlatform:(IrisPlatform *)pl;
@end

@implementation IrisWindowDelegate

- (instancetype)initWithPlatform:(IrisPlatform *)pl {
    self = [super init];
    if (self)
        _platform = pl;
    return self;
}

- (BOOL)windowShouldClose:(NSWindow *)sender {
    (void)sender;
    if (_platform)
        _platform->running = false;
    return NO; /* teardown happens on the iris_app_run_cocoa exit path */
}

- (void)windowDidResize:(NSNotification *)notification {
    (void)notification;
    IrisPlatform *pl = _platform;
    if (!pl || !pl->view)
        return;
    NSSize s = [pl->view bounds].size;
    int w = (int)s.width;
    int h = (int)s.height;
    if (w > 0 && h > 0 && (w != pl->width || h != pl->height)) {
        pl->width = w;
        pl->height = h;
        pl->resized = true;
    }
    [pl->view irisUpdateMetalLayer];
}

- (void)windowDidChangeBackingProperties:(NSNotification *)notification {
    (void)notification;
    IrisPlatform *pl = _platform;
    if (!pl || !pl->window)
        return;
    CGFloat f = [pl->window backingScaleFactor];
    if (f > 0.0)
        pl->pending_scale = (float)f;
    [pl->view irisUpdateMetalLayer];
}

@end

/* ------------------------------------------------------------------ */
/*  Clipboard (NSPasteboard) — bridges lens_clipboard to the pasteboard  */
/* ------------------------------------------------------------------ */

/* lens_clipboard.set_text — replace the general pasteboard's string. */
static void clip_set_text(const char *utf8, size_t len, void *user) {
    (void)user;
    @autoreleasepool {
        if (!utf8)
            return;
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        NSString *s = [[NSString alloc] initWithBytes:utf8 length:len
                                             encoding:NSUTF8StringEncoding];
        if (s)
            [pb setString:s forType:NSPasteboardTypeString];
    }
}

static void post_app_defined(NSInteger subtype) {
    if (!NSApp)
        return;
    /* +otherEventWithType:… with a nil context is the documented way to
     * synthesize an ApplicationDefined event; windowNumber 0 is fine — we
     * intercept the event in the loop before sendEvent: would route it. */
    NSEvent *ev = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                     location:NSZeroPoint
                                modifierFlags:0
                                    timestamp:0
                                 windowNumber:0
                                      context:nil
                                      subtype:subtype
                                        data1:0
                                        data2:0];
    if (ev)
        [NSApp postEvent:ev atStart:NO];
}

/* lens_clipboard.request_text — snapshot the pasteboard now, deliver later.
 * The lens contract is asynchronous ("answered later by the host calling
 * lens_paste"): lens calls request_text from inside a build callback, and
 * lens_paste must not re-enter lens mid-build, so the text is delivered
 * through a posted ApplicationDefined event handled at the top of the next
 * loop iteration, outside any lens_begin/end pair. */
static void clip_request_text(void *user) {
    IrisPlatform *pl = user;
    @autoreleasepool {
        NSString *s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        if (!s)
            return;
        pl->pending_paste = s;
        post_app_defined(kIrisEventSubtypePaste);
    }
}

static void deliver_paste(IrisPlatform *pl) {
    if (!pl->pending_paste)
        return;
    NSString *s = pl->pending_paste;
    pl->pending_paste = nil;
    if (!pl->ui)
        return;
    const char *utf8 = [s UTF8String];
    if (utf8)
        lens_paste(pl->ui, utf8, strlen(utf8));
}

/* ------------------------------------------------------------------ */
/*  Build one lens_input from the accumulated state                     */
/* ------------------------------------------------------------------ */

static void drain_input(IrisPlatform *pl, lens_input *in, float dt) {
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
    /* ime_delete_before/after: NSTextInputClient carries no
     * delete-surrounding request, so both stay 0 (memset above). */

    /* clear per-frame edges; keep level state (down/cursor/mods/preedit) */
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
    for (uint32_t k = 0; k < in->key_count; k++)
        fprintf(stderr, "[raw] key %d %s%s\n", in->keys[k].key, in->keys[k].pressed ? "down" : "up  ",
                in->keys[k].repeat ? " (repeat)" : "");
}

/* Monotonic nanoseconds, for frame pacing. */
static long long now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + (long long)t.tv_nsec;
}

/* True if the input accumulator holds genuine user activity (pointer motion,
 * buttons, scroll, keys, or text/IME) since the last check. Same contract as
 * app_wayland.c's acc_has_user_input; `*pcx`/`*pcy` are updated in place. */
static bool acc_has_user_input(IrisPlatform *pl, double *pcx, double *pcy) {
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
/*  Cross-thread wakeup seam (platform_wakeup.h)                        */
/* ------------------------------------------------------------------ */

/* Kick for the wakeup seam: safe to call from any thread (posting NSEvents
 * from secondary threads is documented), never blocks, and makes the
 * loop's nextEventMatchingMask: wait return the posted ApplicationDefined
 * event; the dispatch site then runs iris_platform_wakeup_drain() on the
 * loop thread. The header's literal CFRunLoopPerformBlock recipe is not
 * used — see the header comment at the top of this file for the rationale. */
static void iris_cocoa_wakeup_kick(void *user) {
    (void)user;
    @autoreleasepool {
        post_app_defined(kIrisEventSubtypeWakeup);
    }
}

/* ------------------------------------------------------------------ */
/*  Theme watcher callback (iris/theme.h live watching)                 */
/* ------------------------------------------------------------------ */

/* Invoked on the iris main thread (KVO on NSApp.effectiveAppearance fires
 * on the main thread; theme_cocoa.m guarantees main-thread delivery). */
static void on_color_scheme_changed(iris_color_scheme scheme, void *user) {
    IrisPlatform *pl = user;
    if (!pl || !pl->ui)
        return;
    lens_theme th =
        (scheme == IRIS_COLOR_SCHEME_PREFER_LIGHT) ? lens_theme_default() : lens_theme_dark();
    lens_set_theme(pl->ui, th);
}

/* ------------------------------------------------------------------ */
/*  Vulkan surface helpers (platform-specific)                         */
/* ------------------------------------------------------------------ */

static VkSurfaceKHR create_vk_surface(const flux_device *device, CALayer *layer) {
    VkMetalSurfaceCreateInfoEXT msci = {
        .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
        .pLayer = (__bridge const CAMetalLayer *)layer,
    };
    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    if (vkCreateMetalSurfaceEXT(flux_device_vk_instance(device), &msci, NULL, &vk_surface) !=
        VK_SUCCESS)
        return VK_NULL_HANDLE;
    return vk_surface;
}

static void destroy_vk_surface(const flux_device *device, VkSurfaceKHR vk_surface) {
    if (vk_surface && device)
        vkDestroySurfaceKHR(flux_device_vk_instance(device), vk_surface, NULL);
}

/* Drawable size in *device* pixels: view bounds × backing scale. */
static void view_physical_size(IrisPlatform *pl, uint32_t *out_w, uint32_t *out_h) {
    NSRect backing = [pl->view convertRectToBacking:[pl->view bounds]];
    uint32_t w = backing.size.width > 1.0 ? (uint32_t)backing.size.width : 1;
    uint32_t h = backing.size.height > 1.0 ? (uint32_t)backing.size.height : 1;
    *out_w = w;
    *out_h = h;
}

/* ------------------------------------------------------------------ */
/*  Run                                                                */
/* ------------------------------------------------------------------ */

int iris_app_run_cocoa(const iris_app_config *cfg) {
    /* All resources declared up front (and every ObjC strong local nil-
     * initialized) so a single `fail:` cleanup block runs on every exit
     * path; under ARC a goto may not jump over a declaration with an
     * initializer, hence the declaration block here. */
    flux_device *device = NULL;
    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    flux_surface *surface = NULL;
    flux_canvas *canvas = NULL;
    lens *ui = NULL;
    bool host_started = false;
    bool wakeup_registered = false;
    int rc = 1; /* pessimistic; set to 0 only on success */

    IrisPlatform *pl = nil;
    NSWindow *window = nil;
    IrisView *view = nil;
    IrisWindowDelegate *win_delegate = nil;

    @autoreleasepool {
        pl = [IrisPlatform new];
        pl->running = true;
        pl->width = cfg->width > 0 ? cfg->width : 960;
        pl->height = cfg->height > 0 ? cfg->height : 720;
        pl->scale = 1.0f;
        pl->pending_scale = 0.0f;
        pl->host_cursor = IRIS_CURSOR_DEFAULT;
        pl->effective_cursor = IRIS_CURSOR_DEFAULT;
        pl->acc.cx = pl->acc.cy = -100000.0;

        /* Publish `pl` as the active app instance so the context-free
         * iris_set_cursor() can reach it. Cleared on the way out (success
         * or fail). */
        g_active_pl = pl;

        /* --- NSApplication + window --------------------------------- */
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        /* finishLaunching is once-per-process; guard so a second
         * iris_app_run in the same process does not re-post it. */
        static bool did_finish_launching = false;
        if (!did_finish_launching) {
            [NSApp finishLaunching];
            did_finish_launching = true;
        }

        /* Cross-thread wakeup seam: register the kick before anything
         * (the theme watcher) can post through it; unregistered on the
         * teardown path. The kick posts an ApplicationDefined event —
         * see the header comment for why not CFRunLoopPerformBlock. */
        iris_platform_wakeup_set_kick(iris_cocoa_wakeup_kick, pl);
        wakeup_registered = true;

        window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, pl->width, pl->height)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        if (!window) {
            fprintf(stderr, "NSWindow creation failed\n");
            goto fail;
        }
        [window setReleasedWhenClosed:NO]; /* ARC owns the lifetime */
        [window setTitle:cfg->title ? [NSString stringWithUTF8String:cfg->title] : @"iris"];
        /* cfg->app_id is unused on macOS — the bundle owns the identity. */

        view = [[IrisView alloc] initWithFrame:NSMakeRect(0, 0, pl->width, pl->height)
                                      platform:pl];
        if (!view) {
            fprintf(stderr, "IrisView creation failed\n");
            goto fail;
        }
        [window setContentView:view];
        win_delegate = [[IrisWindowDelegate alloc] initWithPlatform:pl];
        [window setDelegate:win_delegate];

        pl->window = window;
        pl->view = view;
        pl->win_delegate = win_delegate;

        [window makeKeyAndOrderFront:nil];
        [window makeFirstResponder:view];
        [NSApp activateIgnoringOtherApps:YES];

        CGFloat factor = [window backingScaleFactor];
        pl->scale = factor > 0.0 ? (float)factor : 1.0f;
        pl->pending_scale = pl->scale;
        [view irisUpdateMetalLayer];

        /* --- Vulkan instance via flux (Metal WSI extensions) -------- */
        const char *inst_exts[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_EXT_METAL_SURFACE_EXTENSION_NAME,
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
        /* MoltenVK's VK_KHR_portability_enumeration / _subset are handled
         * inside flux_device_create (it auto-enables both when advertised). */
        if (flux_device_create(&ddesc, &device) != FLUX_OK) {
            fprintf(stderr, "flux_device_create failed (is MoltenVK installed?)\n");
            goto fail;
        }

        /* --- Metal surface + flux surface/canvas -------------------- */
        if (![view.layer isKindOfClass:[CAMetalLayer class]]) {
            fprintf(stderr, "view backing layer is not CAMetalLayer\n");
            goto fail;
        }
        vk_surface = create_vk_surface(device, view.layer);
        if (!vk_surface) {
            fprintf(stderr, "vkCreateMetalSurfaceEXT failed\n");
            goto fail;
        }

        uint32_t phys_w, phys_h;
        view_physical_size(pl, &phys_w, &phys_h);

        flux_surface_desc sdesc = {
            .type = FLUX_TYPE_SURFACE_DESC,
            .vk_surface_khr = vk_surface,
            .width = phys_w,
            .height = phys_h,
            /* Non-blocking present — same rationale as the Wayland backend:
             * a FIFO/vsync present would block the main thread and freeze
             * input handling; the frame loop paces itself instead. */
            .vsync = false,
        };
        if (flux_surface_create(device, &sdesc, &surface) != FLUX_OK) {
            fprintf(stderr, "flux_surface_create failed\n");
            goto fail;
        }

        if (flux_canvas_create(
                &(flux_canvas_desc){.type = FLUX_TYPE_CANVAS_DESC, .surface = surface},
                &canvas) != FLUX_OK) {
            fprintf(stderr, "flux_canvas_create failed\n");
            goto fail;
        }

        if (lens_create(&(lens_desc){.device = device,
                                     .theme = cfg->dark
                                                  ? lens_theme_dark()
                                                  : (iris_system_prefers_dark()
                                                         ? lens_theme_dark()
                                                         : lens_theme_default()),
                                     .scale = pl->scale,
                                     .clipboard = {.set_text = clip_set_text,
                                                   .request_text = clip_request_text,
                                                   .user = pl}},
                        &ui) != FLUX_OK) {
            fprintf(stderr, "lens_create failed\n");
            goto fail;
        }

        if (cfg->start && !cfg->start(ui, device, cfg->user)) {
            fprintf(stderr, "iris host start callback failed\n");
            goto fail;
        }
        host_started = true;

        /* --- Frame loop ----------------------------------------------
         * Same pacing policy as app_wayland.c (see the long comment
         * there): non-blocking present + self-paced deadlines. Active
         * ~60 Hz right after input or while an animation is in flight;
         * ~4 Hz idle while a caret blinks; fully unscheduled (blocking
         * wait with no deadline) when nothing time-driven is pending.
         * Hosts with cfg->paint keep the always-render pacing. The only
         * mechanical difference: the wait primitive is
         * nextEventMatchingMask:untilDate: with an NSDate deadline
         * instead of poll(2). */
        const long long ACTIVE_PERIOD_NS = 16666667LL; /* ~60 Hz when active */
        const long long IDLE_PERIOD_NS = 250000000LL;  /* ~4 Hz when idle:
                                                        * caret blink pace */
        const long long INPUT_GRACE_NS = 400000000LL;  /* stay fast 400ms   */

        struct timespec prev;
        clock_gettime(CLOCK_MONOTONIC, &prev);
        int frame_no = 0;

        long long next_deadline = now_ns();
        bool frame_scheduled = true; /* the first frame must always paint */
        /* Sticky until a frame containing the latest lens build is actually
         * presented — see app_wayland.c for the full rationale. */
        bool surface_needs_paint = true;
        long long last_input_ns = next_deadline;
        long long last_render_ns = next_deadline - ACTIVE_PERIOD_NS;
        double prev_cx = pl->acc.cx, prev_cy = pl->acc.cy;

        pl->ui = ui;

        /* Live colour-scheme watching: only when not forcing dark. The KVO
         * callback fires on this thread and updates the lens theme in place
         * — the next frame renders with the new palette. -1 means
         * unavailable (macOS < 10.14) — degrade to the startup-only query.
         * The backend registers on its reserved internal slot
         * (theme_watch_internal.h), never the host's public watch slot. */
        pl->theme_watching = false;
        if (!cfg->dark)
            pl->theme_watching = (iris_theme__watch_backend(on_color_scheme_changed, pl) == 0);

        /* Accessibility bridge: the darwin build ships a11y_stub.c, so this
         * is inert today; the call sites stay so a future NSAccessibility
         * bridge drops in without backend changes. Fail-soft. */
        pl->a11y_running = (iris_a11y_init() == 0);

        while (pl->running) {
            @autoreleasepool {
                /* Wait until the next frame is due, waking early on any
                 * event. With no frame scheduled (fully idle) there is no
                 * deadline at all — the wait blocks until an event; unlike
                 * Wayland there is no a11y bus fd whose poll mask needs
                 * refreshing, so no 200 ms cap is needed. */
                long long t = now_ns();
                NSDate *until;
                if (!frame_scheduled)
                    until = [NSDate distantFuture];
                else if (next_deadline > t)
                    until = [NSDate dateWithTimeIntervalSinceNow:(double)(next_deadline - t) / 1e9];
                else
                    until = nil; /* overdue: poll without blocking */

                NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                    untilDate:until
                                                       inMode:NSDefaultRunLoopMode
                                                      dequeue:YES];
                bool woke_on_event = (event != nil);

                /* Drain the event burst without blocking so one frame folds
                 * all pending input (same shape as dispatch_pending on
                 * Wayland). Our own ApplicationDefined events are consumed
                 * here; everything else goes through the normal AppKit
                 * dispatch. */
                while (event) {
                    if ([event type] == NSEventTypeApplicationDefined &&
                        [event subtype] == kIrisEventSubtypeWakeup) {
                        iris_platform_wakeup_drain();
                    } else if ([event type] == NSEventTypeApplicationDefined &&
                               [event subtype] == kIrisEventSubtypePaste) {
                        deliver_paste(pl);
                    } else {
                        [NSApp sendEvent:event];
                    }
                    event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                               untilDate:nil
                                                  inMode:NSDefaultRunLoopMode
                                                 dequeue:YES];
                }
                [NSApp updateWindows];

                /* Real user input wakes us out of the idle rate: pull the
                 * next render forward (never sooner than one active period
                 * after the last) and stay at the active rate through the
                 * grace window. */
                if (acc_has_user_input(pl, &prev_cx, &prev_cy)) {
                    last_input_ns = now_ns();
                    long long earliest = last_render_ns + ACTIVE_PERIOD_NS;
                    if (!frame_scheduled || next_deadline > earliest)
                        next_deadline = earliest;
                    frame_scheduled = true;
                } else if (woke_on_event && !frame_scheduled) {
                    /* A non-input event (wakeup-posted callback such as a
                     * theme change, resize, backing-scale change) while
                     * fully idle: run one frame so lens sees the new state;
                     * the repaint query below decides whether it paints. */
                    next_deadline = now_ns();
                    frame_scheduled = true;
                }

                /* Not time to draw yet — keep waiting for events. */
                if (!frame_scheduled || now_ns() < next_deadline)
                    continue;

                /* Render this iteration — identical policy to Wayland. */
                t = now_ns();
                bool host_animating = pl->animation_frame_requested;
                pl->animation_frame_requested = false;
                long long period = (t - last_input_ns < INPUT_GRACE_NS || host_animating)
                                       ? ACTIVE_PERIOD_NS
                                       : IDLE_PERIOD_NS;
                next_deadline = t + period;
                frame_scheduled = true;
                last_render_ns = t;

                /* Backing-scale change (window dragged across Retina /
                 * non-Retina displays): tell lens so its replay transform
                 * matches, then re-fit the swapchain below. */
                bool resized_this_frame = false;
                if (pl->pending_scale > 0.0f && pl->pending_scale != pl->scale) {
                    pl->scale = pl->pending_scale;
                    lens_set_scale(ui, pl->scale);
                    pl->resized = true;
                }
                if (pl->resized) {
                    view_physical_size(pl, &phys_w, &phys_h);
                    (void)flux_surface_resize(surface, phys_w, phys_h);
                    pl->resized = false;
                    resized_this_frame = true;
                }

                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                float dt = (float)(now.tv_sec - prev.tv_sec) +
                           (float)(now.tv_nsec - prev.tv_nsec) * 1e-9f;
                if (dt <= 0.0f)
                    dt = 1.0f / 60.0f;
                prev = now;

                lens_input in;
                drain_input(pl, &in, dt);
                if (cfg->log_raw)
                    log_raw(&in);

                lens_begin(ui, &in);
                if (cfg->build)
                    cfg->build(ui, &in, cfg->user);
                lens_end(ui);

                /* Inert with a11y_stub.c; kept for a future NSAccessibility
                 * bridge. */
                if (pl->a11y_running)
                    iris_a11y_update(ui);

                /* Cursor: follow the hovered widget's semantic hint unless
                 * the host pinned a cursor with iris_set_cursor. */
                if (cursor_follow_hint(pl))
                    cursor_apply(pl);

                /* IME candidate window position is pull-based on macOS (the
                 * input context calls firstRectForCharacterRange: when it
                 * needs the anchor), so there is nothing per-frame to do
                 * here — unlike zwp_text_input_v3_set_cursor_rectangle. */

                /* Static-frame skip: identical to app_wayland.c. Hosts with
                 * a paint callback opt in per frame via
                 * iris_paint_mark_static(); the declaration is consumed here
                 * and must be re-issued every frame. It only covers the
                 * host's own pixels — lens chrome damage still forces a
                 * paint, or a hover highlight would freeze mid-transition
                 * while the host scene is static. */
                bool chrome_damaged = lens_frame_needs_repaint(ui);
                bool host_canvas_static = cfg->paint != NULL && pl->paint_static &&
                                          !host_animating && !resized_this_frame &&
                                          !surface_needs_paint && !chrome_damaged;
                pl->paint_static = false;
                bool must_paint =
                    !host_canvas_static &&
                    (cfg->paint != NULL || chrome_damaged || host_animating ||
                     resized_this_frame || surface_needs_paint);
                if (must_paint) {
                    surface_needs_paint = true;
                    flux_frame *frame = NULL;
                    flux_result r = flux_surface_begin_frame(surface, NULL, &frame);
                    if (r == FLUX_ERROR_SURFACE_LOST) {
                        view_physical_size(pl, &phys_w, &phys_h);
                        (void)flux_surface_resize(surface, phys_w, phys_h);
                        continue;
                    }
                    if (r == FLUX_ERROR_INVALID_STATE)
                        continue;
                    /* Acquire timeout (display asleep or surface occluded):
                     * not an error — skip this frame and retry on the next
                     * deadline instead of exiting the loop. */
                    if (r == FLUX_ERROR_TIMEOUT)
                        continue;
                    if (r != FLUX_OK)
                        break;

                    flux_surface_info info;
                    flux_surface_get_info(surface, &info);

                    /* Clear to the current theme's body background; the
                     * paint callback draws *under* lens's chrome (iris calls
                     * it before lens_render). */
                    lens_theme th = lens_get_theme(ui);
                    flux_color clear = th.color_bg;
                    bool drew = false;
                    if (flux_canvas_begin(canvas, frame, &clear) == FLUX_OK) {
                        if (cfg->paint)
                            cfg->paint(canvas, device, pl->scale, cfg->user);
                        drew = lens_render(ui, canvas) == FLUX_OK;
                        flux_canvas_end(canvas);
                    }

                    if (flux_frame_submit(frame) != FLUX_OK)
                        break;
                    r = flux_frame_present(frame);
                    if (r == FLUX_ERROR_SURFACE_LOST) {
                        view_physical_size(pl, &phys_w, &phys_h);
                        (void)flux_surface_resize(surface, phys_w, phys_h);
                    } else if (r != FLUX_OK) {
                        break;
                    } else if (drew) {
                        surface_needs_paint = false;
                    }

                    if (++frame_no == 1)
                        fprintf(stderr,
                                "first frame presented: %dx%d logical, %ux%u device (scale=%.1f)\n",
                                pl->width, pl->height, info.width, info.height, (double)pl->scale);
                }

                /* Refine the tentative deadline — same rules as Wayland:
                 * active rate while input is warm or an animation is in
                 * flight; low cadence for the caret blink; otherwise stop
                 * scheduling frames entirely and block on the next event.
                 * A host paint callback keeps the always-render pacing
                 * unless it declared this frame static. */
                if (cfg->paint && !host_canvas_static) {
                    if (pl->animation_frame_requested)
                        next_deadline = last_render_ns + ACTIVE_PERIOD_NS;
                    frame_scheduled = true;
                } else if (cfg->paint) {
                    /* Static-declaring host: keep the low idle tick so
                     * build/paint keep running and the host can resume
                     * animating on its own; only the GPU work skips. */
                    next_deadline = t + IDLE_PERIOD_NS;
                    frame_scheduled = true;
                } else if (t - last_input_ns < INPUT_GRACE_NS || pl->animation_frame_requested ||
                           lens_anim_pending(ui)) {
                    next_deadline = t + ACTIVE_PERIOD_NS;
                    frame_scheduled = true;
                } else if (lens_caret_rect(ui).w > 0.0f) {
                    next_deadline = t + IDLE_PERIOD_NS;
                    frame_scheduled = true;
                } else {
                    frame_scheduled = false;
                    /* Fully idle: release the text engine's high-water
                     * scratch (ADR-0072 item 5). Mirrors the Wayland
                     * backend's idle branch — the frame-pacing policy is
                     * ported line-by-line across backends (ADR-0056
                     * item 2). */
                    lens_text_compact(ui);
                }
            } /* @autoreleasepool (loop iteration) */
        }

        rc = 0; /* success — fall through to the unified cleanup below */

        /* --- Cleanup (shared by the success path and every goto fail) --- */
    fail:
        /* Let the host release every resource created from iris's borrowed
         * device before that device, the lens context, or the canvas
         * disappears. Keep the active platform published during the
         * callback so thread-affine iris helpers remain valid. */
        if (host_started && cfg->stop)
            cfg->stop(ui, device, cfg->user);

        /* Stop publishing this platform to the context-free APIs — any host
         * call after this returns is a no-op rather than a use-after-free. */
        g_active_pl = nil;

        /* GPU side first: let in-flight work finish before tearing down. */
        if (device)
            flux_device_wait_idle(device);
        if (ui)
            lens_destroy(ui);
        pl->ui = NULL; /* after this, queued main-thread callbacks must not touch lens */
        if (pl->theme_watching)
            iris_theme__unwatch_backend();
        if (pl->a11y_running)
            iris_a11y_shutdown();
        /* Wakeup seam teardown: unwatch above removed the KVO observer, so
         * nothing posts past this point — but a detached subsystem thread
         * still could, so unregister the kick first (late posts then fail
         * cleanly), then drain whatever was legitimately queued. */
        if (wakeup_registered) {
            iris_platform_wakeup_set_kick(NULL, NULL);
            iris_platform_wakeup_drain();
        }
        if (canvas)
            flux_canvas_destroy(canvas);
        if (surface)
            flux_surface_release(surface);
        if (vk_surface)
            destroy_vk_surface(device, vk_surface);
        if (device)
            flux_device_release(device);

        /* AppKit side: detach delegate/back-pointers first so no late
         * callback can touch torn-down state, then close the window. ARC
         * releases the NS objects when the strong locals go out of scope. */
        pl->ui = NULL;
        pl->pending_paste = nil;
        if (view)
            view->_platform = nil;
        if (window) {
            [window setDelegate:nil];
            [window close];
        }
    } /* @autoreleasepool (function) */
    return rc;
}
