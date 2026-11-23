#ifndef OPTICS_UI_ICON_H
#define OPTICS_UI_ICON_H

#include <stdint.h>

/* Single source of truth for OPTICS_UI_API — never restate it here. */
#include <optics/ui/export.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum optics_ui_icon_id : uint32_t {
    OPTICS_UI_ICON_ACTIVITY = 0,
    OPTICS_UI_ICON_AIRPLAY = 1,
    OPTICS_UI_ICON_ALERT_CIRCLE = 2,
    OPTICS_UI_ICON_ALERT_OCTAGON = 3,
    OPTICS_UI_ICON_ALERT_TRIANGLE = 4,
    OPTICS_UI_ICON_ALIGN_CENTER = 5,
    OPTICS_UI_ICON_ALIGN_JUSTIFY = 6,
    OPTICS_UI_ICON_ALIGN_LEFT = 7,
    OPTICS_UI_ICON_ALIGN_RIGHT = 8,
    OPTICS_UI_ICON_ANCHOR = 9,
    OPTICS_UI_ICON_APERTURE = 10,
    OPTICS_UI_ICON_ARCHIVE = 11,
    OPTICS_UI_ICON_ARROW_DOWN_CIRCLE = 12,
    OPTICS_UI_ICON_ARROW_DOWN_LEFT = 13,
    OPTICS_UI_ICON_ARROW_DOWN_RIGHT = 14,
    OPTICS_UI_ICON_ARROW_DOWN = 15,
    OPTICS_UI_ICON_ARROW_LEFT_CIRCLE = 16,
    OPTICS_UI_ICON_ARROW_LEFT = 17,
    OPTICS_UI_ICON_ARROW_RIGHT_CIRCLE = 18,
    OPTICS_UI_ICON_ARROW_RIGHT = 19,
    OPTICS_UI_ICON_ARROW_UP_CIRCLE = 20,
    OPTICS_UI_ICON_ARROW_UP_LEFT = 21,
    OPTICS_UI_ICON_ARROW_UP_RIGHT = 22,
    OPTICS_UI_ICON_ARROW_UP = 23,
    OPTICS_UI_ICON_AT_SIGN = 24,
    OPTICS_UI_ICON_AWARD = 25,
    OPTICS_UI_ICON_BAR_CHART_2 = 26,
    OPTICS_UI_ICON_BAR_CHART = 27,
    OPTICS_UI_ICON_BATTERY_CHARGING = 28,
    OPTICS_UI_ICON_BATTERY = 29,
    OPTICS_UI_ICON_BELL_OFF = 30,
    OPTICS_UI_ICON_BELL = 31,
    OPTICS_UI_ICON_BLUETOOTH = 32,
    OPTICS_UI_ICON_BOLD = 33,
    OPTICS_UI_ICON_BOOK_OPEN = 34,
    OPTICS_UI_ICON_BOOK = 35,
    OPTICS_UI_ICON_BOOKMARK = 36,
    OPTICS_UI_ICON_BOX = 37,
    OPTICS_UI_ICON_BRIEFCASE = 38,
    OPTICS_UI_ICON_CALENDAR = 39,
    OPTICS_UI_ICON_CAMERA_OFF = 40,
    OPTICS_UI_ICON_CAMERA = 41,
    OPTICS_UI_ICON_CAST = 42,
    OPTICS_UI_ICON_CHECK_CIRCLE = 43,
    OPTICS_UI_ICON_CHECK_SQUARE = 44,
    OPTICS_UI_ICON_CHECK = 45,
    OPTICS_UI_ICON_CHEVRON_DOWN = 46,
    OPTICS_UI_ICON_CHEVRON_LEFT = 47,
    OPTICS_UI_ICON_CHEVRON_RIGHT = 48,
    OPTICS_UI_ICON_CHEVRON_UP = 49,
    OPTICS_UI_ICON_CHEVRONS_DOWN = 50,
    OPTICS_UI_ICON_CHEVRONS_LEFT = 51,
    OPTICS_UI_ICON_CHEVRONS_RIGHT = 52,
    OPTICS_UI_ICON_CHEVRONS_UP = 53,
    OPTICS_UI_ICON_CHROME = 54,
    OPTICS_UI_ICON_CIRCLE = 55,
    OPTICS_UI_ICON_CLIPBOARD = 56,
    OPTICS_UI_ICON_CLOCK = 57,
    OPTICS_UI_ICON_CLOUD_DRIZZLE = 58,
    OPTICS_UI_ICON_CLOUD_LIGHTNING = 59,
    OPTICS_UI_ICON_CLOUD_OFF = 60,
    OPTICS_UI_ICON_CLOUD_RAIN = 61,
    OPTICS_UI_ICON_CLOUD_SNOW = 62,
    OPTICS_UI_ICON_CLOUD = 63,
    OPTICS_UI_ICON_CODE = 64,
    OPTICS_UI_ICON_CODEPEN = 65,
    OPTICS_UI_ICON_CODESANDBOX = 66,
    OPTICS_UI_ICON_COFFEE = 67,
    OPTICS_UI_ICON_COLUMNS = 68,
    OPTICS_UI_ICON_COMMAND = 69,
    OPTICS_UI_ICON_COMPASS = 70,
    OPTICS_UI_ICON_COPY = 71,
    OPTICS_UI_ICON_CORNER_DOWN_LEFT = 72,
    OPTICS_UI_ICON_CORNER_DOWN_RIGHT = 73,
    OPTICS_UI_ICON_CORNER_LEFT_DOWN = 74,
    OPTICS_UI_ICON_CORNER_LEFT_UP = 75,
    OPTICS_UI_ICON_CORNER_RIGHT_DOWN = 76,
    OPTICS_UI_ICON_CORNER_RIGHT_UP = 77,
    OPTICS_UI_ICON_CORNER_UP_LEFT = 78,
    OPTICS_UI_ICON_CORNER_UP_RIGHT = 79,
    OPTICS_UI_ICON_CPU = 80,
    OPTICS_UI_ICON_CREDIT_CARD = 81,
    OPTICS_UI_ICON_CROP = 82,
    OPTICS_UI_ICON_CROSSHAIR = 83,
    OPTICS_UI_ICON_DATABASE = 84,
    OPTICS_UI_ICON_DELETE = 85,
    OPTICS_UI_ICON_DISC = 86,
    OPTICS_UI_ICON_DIVIDE_CIRCLE = 87,
    OPTICS_UI_ICON_DIVIDE_SQUARE = 88,
    OPTICS_UI_ICON_DIVIDE = 89,
    OPTICS_UI_ICON_DOLLAR_SIGN = 90,
    OPTICS_UI_ICON_DOWNLOAD_CLOUD = 91,
    OPTICS_UI_ICON_DOWNLOAD = 92,
    OPTICS_UI_ICON_DRIBBBLE = 93,
    OPTICS_UI_ICON_DROPLET = 94,
    OPTICS_UI_ICON_EDIT_2 = 95,
    OPTICS_UI_ICON_EDIT_3 = 96,
    OPTICS_UI_ICON_EDIT = 97,
    OPTICS_UI_ICON_EXTERNAL_LINK = 98,
    OPTICS_UI_ICON_EYE_OFF = 99,
    OPTICS_UI_ICON_EYE = 100,
    OPTICS_UI_ICON_FACEBOOK = 101,
    OPTICS_UI_ICON_FAST_FORWARD = 102,
    OPTICS_UI_ICON_FEATHER = 103,
    OPTICS_UI_ICON_FIGMA = 104,
    OPTICS_UI_ICON_FILE_MINUS = 105,
    OPTICS_UI_ICON_FILE_PLUS = 106,
    OPTICS_UI_ICON_FILE_TEXT = 107,
    OPTICS_UI_ICON_FILE = 108,
    OPTICS_UI_ICON_FILM = 109,
    OPTICS_UI_ICON_FILTER = 110,
    OPTICS_UI_ICON_FLAG = 111,
    OPTICS_UI_ICON_FOLDER_MINUS = 112,
    OPTICS_UI_ICON_FOLDER_PLUS = 113,
    OPTICS_UI_ICON_FOLDER = 114,
    OPTICS_UI_ICON_FRAMER = 115,
    OPTICS_UI_ICON_FROWN = 116,
    OPTICS_UI_ICON_GIFT = 117,
    OPTICS_UI_ICON_GIT_BRANCH = 118,
    OPTICS_UI_ICON_GIT_COMMIT = 119,
    OPTICS_UI_ICON_GIT_MERGE = 120,
    OPTICS_UI_ICON_GIT_PULL_REQUEST = 121,
    OPTICS_UI_ICON_GITHUB = 122,
    OPTICS_UI_ICON_GITLAB = 123,
    OPTICS_UI_ICON_GLOBE = 124,
    OPTICS_UI_ICON_GRID = 125,
    OPTICS_UI_ICON_HARD_DRIVE = 126,
    OPTICS_UI_ICON_HASH = 127,
    OPTICS_UI_ICON_HEADPHONES = 128,
    OPTICS_UI_ICON_HEART = 129,
    OPTICS_UI_ICON_HELP_CIRCLE = 130,
    OPTICS_UI_ICON_HEXAGON = 131,
    OPTICS_UI_ICON_HOME = 132,
    OPTICS_UI_ICON_IMAGE = 133,
    OPTICS_UI_ICON_INBOX = 134,
    OPTICS_UI_ICON_INFO = 135,
    OPTICS_UI_ICON_INSTAGRAM = 136,
    OPTICS_UI_ICON_ITALIC = 137,
    OPTICS_UI_ICON_KEY = 138,
    OPTICS_UI_ICON_LAYERS = 139,
    OPTICS_UI_ICON_LAYOUT = 140,
    OPTICS_UI_ICON_LIFE_BUOY = 141,
    OPTICS_UI_ICON_LINK_2 = 142,
    OPTICS_UI_ICON_LINK = 143,
    OPTICS_UI_ICON_LINKEDIN = 144,
    OPTICS_UI_ICON_LIST = 145,
    OPTICS_UI_ICON_LOADER = 146,
    OPTICS_UI_ICON_LOCK = 147,
    OPTICS_UI_ICON_LOG_IN = 148,
    OPTICS_UI_ICON_LOG_OUT = 149,
    OPTICS_UI_ICON_MAIL = 150,
    OPTICS_UI_ICON_MAP_PIN = 151,
    OPTICS_UI_ICON_MAP = 152,
    OPTICS_UI_ICON_MAXIMIZE_2 = 153,
    OPTICS_UI_ICON_MAXIMIZE = 154,
    OPTICS_UI_ICON_MEH = 155,
    OPTICS_UI_ICON_MENU = 156,
    OPTICS_UI_ICON_MESSAGE_CIRCLE = 157,
    OPTICS_UI_ICON_MESSAGE_SQUARE = 158,
    OPTICS_UI_ICON_MIC_OFF = 159,
    OPTICS_UI_ICON_MIC = 160,
    OPTICS_UI_ICON_MINIMIZE_2 = 161,
    OPTICS_UI_ICON_MINIMIZE = 162,
    OPTICS_UI_ICON_MINUS_CIRCLE = 163,
    OPTICS_UI_ICON_MINUS_SQUARE = 164,
    OPTICS_UI_ICON_MINUS = 165,
    OPTICS_UI_ICON_MONITOR = 166,
    OPTICS_UI_ICON_MOON = 167,
    OPTICS_UI_ICON_MORE_HORIZONTAL = 168,
    OPTICS_UI_ICON_MORE_VERTICAL = 169,
    OPTICS_UI_ICON_MOUSE_POINTER = 170,
    OPTICS_UI_ICON_MOVE = 171,
    OPTICS_UI_ICON_MUSIC = 172,
    OPTICS_UI_ICON_NAVIGATION_2 = 173,
    OPTICS_UI_ICON_NAVIGATION = 174,
    OPTICS_UI_ICON_OCTAGON = 175,
    OPTICS_UI_ICON_PACKAGE = 176,
    OPTICS_UI_ICON_PAPERCLIP = 177,
    OPTICS_UI_ICON_PAUSE_CIRCLE = 178,
    OPTICS_UI_ICON_PAUSE = 179,
    OPTICS_UI_ICON_PEN_TOOL = 180,
    OPTICS_UI_ICON_PERCENT = 181,
    OPTICS_UI_ICON_PHONE_CALL = 182,
    OPTICS_UI_ICON_PHONE_FORWARDED = 183,
    OPTICS_UI_ICON_PHONE_INCOMING = 184,
    OPTICS_UI_ICON_PHONE_MISSED = 185,
    OPTICS_UI_ICON_PHONE_OFF = 186,
    OPTICS_UI_ICON_PHONE_OUTGOING = 187,
    OPTICS_UI_ICON_PHONE = 188,
    OPTICS_UI_ICON_PIE_CHART = 189,
    OPTICS_UI_ICON_PLAY_CIRCLE = 190,
    OPTICS_UI_ICON_PLAY = 191,
    OPTICS_UI_ICON_PLUS_CIRCLE = 192,
    OPTICS_UI_ICON_PLUS_SQUARE = 193,
    OPTICS_UI_ICON_PLUS = 194,
    OPTICS_UI_ICON_POCKET = 195,
    OPTICS_UI_ICON_POWER = 196,
    OPTICS_UI_ICON_PRINTER = 197,
    OPTICS_UI_ICON_RADIO = 198,
    OPTICS_UI_ICON_REFRESH_CCW = 199,
    OPTICS_UI_ICON_REFRESH_CW = 200,
    OPTICS_UI_ICON_REPEAT = 201,
    OPTICS_UI_ICON_REWIND = 202,
    OPTICS_UI_ICON_ROTATE_CCW = 203,
    OPTICS_UI_ICON_ROTATE_CW = 204,
    OPTICS_UI_ICON_RSS = 205,
    OPTICS_UI_ICON_SAVE = 206,
    OPTICS_UI_ICON_SCISSORS = 207,
    OPTICS_UI_ICON_SEARCH = 208,
    OPTICS_UI_ICON_SEND = 209,
    OPTICS_UI_ICON_SERVER = 210,
    OPTICS_UI_ICON_SETTINGS = 211,
    OPTICS_UI_ICON_SHARE_2 = 212,
    OPTICS_UI_ICON_SHARE = 213,
    OPTICS_UI_ICON_SHIELD_OFF = 214,
    OPTICS_UI_ICON_SHIELD = 215,
    OPTICS_UI_ICON_SHOPPING_BAG = 216,
    OPTICS_UI_ICON_SHOPPING_CART = 217,
    OPTICS_UI_ICON_SHUFFLE = 218,
    OPTICS_UI_ICON_SIDEBAR = 219,
    OPTICS_UI_ICON_SKIP_BACK = 220,
    OPTICS_UI_ICON_SKIP_FORWARD = 221,
    OPTICS_UI_ICON_SLACK = 222,
    OPTICS_UI_ICON_SLASH = 223,
    OPTICS_UI_ICON_SLIDERS = 224,
    OPTICS_UI_ICON_SMARTPHONE = 225,
    OPTICS_UI_ICON_SMILE = 226,
    OPTICS_UI_ICON_SPEAKER = 227,
    OPTICS_UI_ICON_SQUARE = 228,
    OPTICS_UI_ICON_STAR = 229,
    OPTICS_UI_ICON_STOP_CIRCLE = 230,
    OPTICS_UI_ICON_SUN = 231,
    OPTICS_UI_ICON_SUNRISE = 232,
    OPTICS_UI_ICON_SUNSET = 233,
    OPTICS_UI_ICON_TABLE = 234,
    OPTICS_UI_ICON_TABLET = 235,
    OPTICS_UI_ICON_TAG = 236,
    OPTICS_UI_ICON_TARGET = 237,
    OPTICS_UI_ICON_TERMINAL = 238,
    OPTICS_UI_ICON_THERMOMETER = 239,
    OPTICS_UI_ICON_THUMBS_DOWN = 240,
    OPTICS_UI_ICON_THUMBS_UP = 241,
    OPTICS_UI_ICON_TOGGLE_LEFT = 242,
    OPTICS_UI_ICON_TOGGLE_RIGHT = 243,
    OPTICS_UI_ICON_TOOL = 244,
    OPTICS_UI_ICON_TRASH_2 = 245,
    OPTICS_UI_ICON_TRASH = 246,
    OPTICS_UI_ICON_TRELLO = 247,
    OPTICS_UI_ICON_TRENDING_DOWN = 248,
    OPTICS_UI_ICON_TRENDING_UP = 249,
    OPTICS_UI_ICON_TRIANGLE = 250,
    OPTICS_UI_ICON_TRUCK = 251,
    OPTICS_UI_ICON_TV = 252,
    OPTICS_UI_ICON_TWITCH = 253,
    OPTICS_UI_ICON_TWITTER = 254,
    OPTICS_UI_ICON_TYPE = 255,
    OPTICS_UI_ICON_UMBRELLA = 256,
    OPTICS_UI_ICON_UNDERLINE = 257,
    OPTICS_UI_ICON_UNLOCK = 258,
    OPTICS_UI_ICON_UPLOAD_CLOUD = 259,
    OPTICS_UI_ICON_UPLOAD = 260,
    OPTICS_UI_ICON_USER_CHECK = 261,
    OPTICS_UI_ICON_USER_MINUS = 262,
    OPTICS_UI_ICON_USER_PLUS = 263,
    OPTICS_UI_ICON_USER_X = 264,
    OPTICS_UI_ICON_USER = 265,
    OPTICS_UI_ICON_USERS = 266,
    OPTICS_UI_ICON_VIDEO_OFF = 267,
    OPTICS_UI_ICON_VIDEO = 268,
    OPTICS_UI_ICON_VOICEMAIL = 269,
    OPTICS_UI_ICON_VOLUME_1 = 270,
    OPTICS_UI_ICON_VOLUME_2 = 271,
    OPTICS_UI_ICON_VOLUME_X = 272,
    OPTICS_UI_ICON_VOLUME = 273,
    OPTICS_UI_ICON_WATCH = 274,
    OPTICS_UI_ICON_WIFI_OFF = 275,
    OPTICS_UI_ICON_WIFI = 276,
    OPTICS_UI_ICON_WIND = 277,
    OPTICS_UI_ICON_X_CIRCLE = 278,
    OPTICS_UI_ICON_X_OCTAGON = 279,
    OPTICS_UI_ICON_X_SQUARE = 280,
    OPTICS_UI_ICON_X = 281,
    OPTICS_UI_ICON_YOUTUBE = 282,
    OPTICS_UI_ICON_ZAP_OFF = 283,
    OPTICS_UI_ICON_ZAP = 284,
    OPTICS_UI_ICON_ZOOM_IN = 285,
    OPTICS_UI_ICON_ZOOM_OUT = 286,
    OPTICS_UI_ICON_STAR_ROUNDED_FILLED = 287,
    OPTICS_UI_ICON_STAR_ROUNDED = 288,
    OPTICS_UI_ICON_COUNT,
} optics_ui_icon_id;

typedef struct optics_ui_icon_cmd {
    uint8_t type;
    float params[6];
} optics_ui_icon_cmd;

/* A contiguous sub-range of a optics_ui_icon_desc's cmd stream painted with its
 * own colour and mode. `color` is a straight (non-premultiplied)
 * 0xRRGGBBAA value; `fill` selects fill_path vs stroke_path for the range.
 * Runtime SVG registration (optics_ui_icon_register_svg) produces one run per
 * source shape, preserving the SVG's paint colours; `runs == NULL` on a
 * desc means "single paint for the whole stream" (every built-in icon) and
 * the draw colour comes from the widget/theme as before. */
typedef struct optics_ui_icon_run {
    uint32_t first_cmd; /* index into cmds */
    uint32_t count;     /* number of cmds in this run */
    uint32_t color;     /* 0xRRGGBBAA, straight alpha */
    uint8_t fill;       /* 1 = fill, 0 = stroke */
    uint8_t reserved[3];
} optics_ui_icon_run;

typedef struct optics_ui_icon_desc {
    const optics_ui_icon_cmd *cmds;
    uint32_t count;
    const optics_ui_icon_run *runs; /* NULL = whole stream, one theme paint */
    uint32_t run_count;             /* 0 unless runs != NULL */
} optics_ui_icon_desc;

/* The built-in icon table is internal: optics_ui_icon_id values < OPTICS_UI_ICON_COUNT
 * index it, and optics_ui_icon_info() is the public read access. It is not part
 * of the installed API surface. */

/* Resolve an icon id — built-in (0 .. OPTICS_UI_ICON_COUNT-1) or runtime
 * (optics_ui_icon_register_svg, >= OPTICS_UI_ICON_COUNT) — to its command stream,
 * per-run paints, and render mode. Returns NULL for unknown ids. The
 * returned descriptor is owned by the registry and stays valid for the
 * process lifetime (ids are never reclaimed), so callers may cache it.
 * `mode` (0 = filled, 1 = outlined) describes the built-in table's render
 * style; runtime SVG icons always report 0 and carry their paints in
 * `runs`. This is the public read access to the icon table that replaced
 * exporting `optics_ui_icon_table` directly. */
OPTICS_UI_API const optics_ui_icon_desc *optics_ui_icon_info(optics_ui_icon_id icon, uint8_t *mode);

/* Failure sentinel of optics_ui_icon_register_svg; also usable as a "no icon"
 * value wherever a optics_ui_icon_id is optional. */
#define OPTICS_UI_ICON_INVALID ((optics_ui_icon_id) - 1)

/* Register an SVG string as a runtime icon. Returns its id (>= OPTICS_UI_ICON_COUNT)
 * or OPTICS_UI_ICON_INVALID on parse failure. Ids are process-global, never reclaimed;
 * call from the UI thread (registration is not thread-safe, like the rest of ui).
 *
 * Shapes painted `currentColor` (or with no paint attribute) follow the theme
 * colour exactly like built-ins. Shapes carrying an explicit colour keep it:
 * registration records one paint run per shape with its fill/stroke colour
 * (0xRRGGBBAA). Gradients degrade to the gradient's first stop colour.
 * The source strokeWidth is still not honoured — one weight keeps runtime
 * icons visually interchangeable with the built-in set; a shape with both
 * fill and stroke contributes its fill run (stroke-on-fill outlines can be
 * expressed by duplicating the shape). */
OPTICS_UI_API optics_ui_icon_id optics_ui_icon_register_svg(const char *svg_utf8);

#ifdef __cplusplus
}
#endif

#endif
