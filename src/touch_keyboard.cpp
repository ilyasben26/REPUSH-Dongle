#include "touch_keyboard.h"
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>

// Hardware pins — must match physical wiring (same as working_touch_game.ino)
#define TKB_TFT_CS   10
#define TKB_TFT_DC    9
#define TKB_TFT_RST   8
#define TKB_TS_CS     7
#define TKB_ROTATION  3   // landscape

static Adafruit_ILI9341  _tft(TKB_TFT_CS, TKB_TFT_DC, TKB_TFT_RST);
static XPT2046_Touchscreen _ts(TKB_TS_CS);

// Touch calibration coefficients (raw → screen pixel)
static float _xM = 1.0f, _xC = 0.0f;
static float _yM = 1.0f, _yC = 0.0f;

// Screen dimensions (set after tft.begin())
static int16_t _sw = 320;
static int16_t _sh = 240;

// ── Colors ────────────────────────────────────────────────────────────────
static const uint16_t C_BG        = ILI9341_BLACK;
static const uint16_t C_KEY_NRM   = 0x2945;   // dark slate-blue
static const uint16_t C_KEY_DEL   = 0xA000;   // dark red
static const uint16_t C_KEY_CANCEL= 0xD000;   // orange-red
static const uint16_t C_KEY_DONE  = 0x0640;   // dark green
static const uint16_t C_KEY_SPACE = 0x4208;   // medium grey
static const uint16_t C_KEY_PRESS = ILI9341_WHITE;
static const uint16_t C_KEY_TXT   = ILI9341_WHITE;
static const uint16_t C_KEY_PTXT  = ILI9341_BLACK;
static const uint16_t C_INPUT_BG  = 0x0841;   // very dark navy
static const uint16_t C_INPUT_BDR = ILI9341_CYAN;
static const uint16_t C_TITLE     = ILI9341_YELLOW;

// ── Key layout ────────────────────────────────────────────────────────────
//
// Screen 320×240 (landscape)
//
//  y=0..51   — header: title label + input box
//  y=55..88  — row 1: Q W E R T Y U I O P
//  y=94..127 — row 2: A S D F G H J K L
//  y=133..166— row 3: Z X C V B N M [DEL]
//  y=172..205— row 4: [  SPACE  ] [CANCEL] [ DONE ]

static const int16_t KEY_W  = 28;   // normal key width
static const int16_t KEY_H  = 34;   // all key heights
static const int16_t KEY_G  = 4;    // gap between keys
static const int16_t DEL_W  = 44;   // DEL key width
static const int16_t SP_W   = 128;  // SPACE key width
static const int16_t CA_W   = 70;   // CANCEL key width
static const int16_t DN_W   = 90;   // DONE key width

static const int16_t ROW_Y[4] = { 55, 94, 133, 172 };

// Input area
static const int16_t IN_BOX_X = 4;
static const int16_t IN_BOX_Y = 16;
static const int16_t IN_BOX_W = 312;
static const int16_t IN_BOX_H = 34;

enum SpecialKey : uint8_t { KEY_NORM, KEY_DEL_T, KEY_CANCEL_T, KEY_DONE_T, KEY_SPACE_T };

struct Key {
    int16_t    x, y, w, h;
    char       ch;
    SpecialKey sp;
};

static Key    s_keys[32];
static uint8_t s_nkeys = 0;

static const char ROW1[] = "QWERTYUIOP";
static const char ROW2[] = "ASDFGHJKL";
static const char ROW3[] = "ZXCVBNM";

static void build_keys()
{
    s_nkeys = 0;

    // Row 1 — 10 keys, centered
    {
        const int    n  = 10;
        const int16_t tw = n * KEY_W + (n - 1) * KEY_G;
        const int16_t sx = (_sw - tw) / 2;
        for (int i = 0; i < n; i++)
            s_keys[s_nkeys++] = { (int16_t)(sx + i * (KEY_W + KEY_G)), ROW_Y[0],
                                  KEY_W, KEY_H, ROW1[i], KEY_NORM };
    }

    // Row 2 — 9 keys, centered
    {
        const int    n  = 9;
        const int16_t tw = n * KEY_W + (n - 1) * KEY_G;
        const int16_t sx = (_sw - tw) / 2;
        for (int i = 0; i < n; i++)
            s_keys[s_nkeys++] = { (int16_t)(sx + i * (KEY_W + KEY_G)), ROW_Y[1],
                                  KEY_W, KEY_H, ROW2[i], KEY_NORM };
    }

    // Row 3 — 7 letter keys + wide DEL key, centered as a unit
    {
        const int    n  = 7;
        // total = 7 keys + 6 inner gaps + 1 gap before DEL + DEL
        const int16_t tw = n * KEY_W + (n - 1) * KEY_G + KEY_G + DEL_W;
        const int16_t sx = (_sw - tw) / 2;
        for (int i = 0; i < n; i++)
            s_keys[s_nkeys++] = { (int16_t)(sx + i * (KEY_W + KEY_G)), ROW_Y[2],
                                  KEY_W, KEY_H, ROW3[i], KEY_NORM };
        // DEL immediately after M with one gap
        const int16_t del_x = sx + n * (KEY_W + KEY_G);
        s_keys[s_nkeys++] = { del_x, ROW_Y[2], DEL_W, KEY_H, 0, KEY_DEL_T };
    }

    // Row 4 — SPACE + CANCEL + DONE, centered
    {
        const int16_t tw = SP_W + KEY_G + CA_W + KEY_G + DN_W;
        const int16_t sx = (_sw - tw) / 2;
        s_keys[s_nkeys++] = { sx,                               ROW_Y[3], SP_W, KEY_H, 0, KEY_SPACE_T  };
        s_keys[s_nkeys++] = { (int16_t)(sx + SP_W + KEY_G),    ROW_Y[3], CA_W, KEY_H, 0, KEY_CANCEL_T };
        s_keys[s_nkeys++] = { (int16_t)(sx + SP_W + KEY_G + CA_W + KEY_G), ROW_Y[3], DN_W, KEY_H, 0, KEY_DONE_T };
    }
}

// ── Drawing helpers ───────────────────────────────────────────────────────

static uint16_t key_bg(const Key &k, bool pressed)
{
    if (pressed) return C_KEY_PRESS;
    switch (k.sp) {
        case KEY_DEL_T:    return C_KEY_DEL;
        case KEY_CANCEL_T: return C_KEY_CANCEL;
        case KEY_DONE_T:   return C_KEY_DONE;
        case KEY_SPACE_T:  return C_KEY_SPACE;
        default:           return C_KEY_NRM;
    }
}

static void draw_key(uint8_t i, bool pressed)
{
    const Key &k  = s_keys[i];
    uint16_t   bg = key_bg(k, pressed);
    uint16_t   fg = pressed ? C_KEY_PTXT : C_KEY_TXT;

    _tft.fillRoundRect(k.x, k.y, k.w, k.h, 4, bg);
    _tft.drawRoundRect(k.x, k.y, k.w, k.h, 4, ILI9341_WHITE);
    _tft.setTextColor(fg);

    if (k.sp == KEY_NORM) {
        // Single letter at textSize 2 (char 12×16), centered in key
        _tft.setTextSize(2);
        _tft.setCursor(k.x + (k.w - 12) / 2, k.y + (k.h - 16) / 2);
        _tft.print(k.ch);
    } else {
        // Multi-char label at textSize 1 (char 6×8), centered in key
        const char *lbl =
            (k.sp == KEY_DEL_T)    ? "DEL"    :
            (k.sp == KEY_CANCEL_T) ? "CANCEL" :
            (k.sp == KEY_DONE_T)   ? "DONE"   : "SPACE";
        int16_t lw = (int16_t)(strlen(lbl) * 6);
        _tft.setTextSize(1);
        _tft.setCursor(k.x + (k.w - lw) / 2, k.y + (k.h - 8) / 2);
        _tft.print(lbl);
    }
}

static void draw_all_keys()
{
    for (uint8_t i = 0; i < s_nkeys; i++)
        draw_key(i, false);
}

static void draw_header(bool is_pw, const char *buf)
{
    // Clear only the header area to avoid flickering the keyboard
    _tft.fillRect(0, 0, _sw, ROW_Y[0] - 3, C_BG);

    _tft.setCursor(IN_BOX_X, 4);
    _tft.setTextSize(1);
    _tft.setTextColor(C_TITLE);
    _tft.print(is_pw ? "ENTER PASSWORD:" : "ENTER USERNAME:");

    _tft.fillRect(IN_BOX_X, IN_BOX_Y, IN_BOX_W, IN_BOX_H, C_INPUT_BG);
    _tft.drawRect(IN_BOX_X, IN_BOX_Y, IN_BOX_W, IN_BOX_H, C_INPUT_BDR);

    _tft.setTextSize(2);
    _tft.setTextColor(ILI9341_WHITE);
    // Center text vertically in the box (charH=16 at size 2)
    _tft.setCursor(IN_BOX_X + 4, IN_BOX_Y + (IN_BOX_H - 16) / 2);

    const size_t len = strlen(buf);
    if (is_pw) {
        for (size_t j = 0; j < len; j++) _tft.print('*');
    } else {
        _tft.print(buf);
    }
    _tft.print('_');  // static cursor
}

// ── Touch helpers ─────────────────────────────────────────────────────────

static bool touch_xy(int16_t &sx, int16_t &sy)
{
    if (!_ts.touched()) return false;
    TS_Point p = _ts.getPoint();
    sx = (int16_t)((float)p.x * _xM + _xC);
    sy = (int16_t)((float)p.y * _yM + _yC);
    if (sx < 0)    sx = 0;
    if (sx >= _sw) sx = _sw - 1;
    if (sy < 0)    sy = 0;
    if (sy >= _sh) sy = _sh - 1;
    return true;
}

static int8_t hit_test(int16_t sx, int16_t sy)
{
    for (uint8_t i = 0; i < s_nkeys; i++) {
        const Key &k = s_keys[i];
        if (sx >= k.x && sx < k.x + k.w && sy >= k.y && sy < k.y + k.h)
            return (int8_t)i;
    }
    return -1;
}

// ── Single-field prompt loop ──────────────────────────────────────────────

// Runs the keyboard for one field (username or password).
// Returns true on DONE, false on CANCEL.
static bool run_field(bool is_pw, char *buf, size_t maxlen)
{
    size_t len = 0;
    buf[0] = '\0';

    draw_header(is_pw, buf);
    draw_all_keys();

    bool prev_touched = false;

    for (;;) {
        int16_t sx, sy;
        bool now_touched = touch_xy(sx, sy);

        // Only act on the rising edge of a touch
        if (now_touched && !prev_touched) {
            int8_t ki = hit_test(sx, sy);
            if (ki >= 0) {
                const Key &k = s_keys[ki];

                // Brief visual press feedback
                draw_key((uint8_t)ki, true);
                delay(80);
                draw_key((uint8_t)ki, false);

                switch (k.sp) {
                    case KEY_DONE_T:
                        return true;

                    case KEY_CANCEL_T:
                        return false;

                    case KEY_DEL_T:
                        if (len > 0) { buf[--len] = '\0'; draw_header(is_pw, buf); }
                        break;

                    case KEY_SPACE_T:
                        if (len + 1 < maxlen) { buf[len++] = ' '; buf[len] = '\0'; draw_header(is_pw, buf); }
                        break;

                    default: // KEY_NORM
                        if (len + 1 < maxlen) { buf[len++] = k.ch; buf[len] = '\0'; draw_header(is_pw, buf); }
                        break;
                }
            }
        }

        prev_touched = now_touched;
        delay(20);
    }
}

// ── Public API ────────────────────────────────────────────────────────────

void touch_kb_setup()
{
    // Drive CS lines high before begin() to avoid SPI bus contention
    pinMode(TKB_TS_CS,  OUTPUT); digitalWrite(TKB_TS_CS,  HIGH);
    pinMode(TKB_TFT_CS, OUTPUT); digitalWrite(TKB_TFT_CS, HIGH);

    _tft.begin();
    _tft.setRotation(TKB_ROTATION);
    _tft.fillScreen(C_BG);
    _sw = _tft.width();
    _sh = _tft.height();

    _ts.begin();
    _ts.setRotation(TKB_ROTATION);

    build_keys();
}

void touch_kb_calibrate()
{
    TS_Point p;
    int16_t  x1, y1, x2, y2;

    _tft.fillScreen(ILI9341_BLACK);
    while (_ts.touched());  // wait for no touch

    // ── Crosshair 1: top-left ────────────────────────────────────────────
    _tft.drawFastHLine(10, 20, 20, ILI9341_RED);
    _tft.drawFastVLine(20, 10, 20, ILI9341_RED);
    _tft.setCursor(38, 14);
    _tft.setTextSize(1);
    _tft.setTextColor(ILI9341_WHITE);
    _tft.print("Touch crosshair to calibrate");
    while (!_ts.touched());
    p = _ts.getPoint(); x1 = p.x; y1 = p.y;
    _tft.fillScreen(ILI9341_BLACK);
    delay(500);
    while (_ts.touched());

    // ── Crosshair 2: bottom-right ─────────────────────────────────────────
    _tft.drawFastHLine(_sw - 30, _sh - 20, 20, ILI9341_RED);
    _tft.drawFastVLine(_sw - 20, _sh - 30, 20, ILI9341_RED);
    _tft.setCursor(_sw - 190, _sh - 35);
    _tft.setTextSize(1);
    _tft.setTextColor(ILI9341_WHITE);
    _tft.print("Touch crosshair to calibrate");
    while (!_ts.touched());
    p = _ts.getPoint(); x2 = p.x; y2 = p.y;
    _tft.fillScreen(ILI9341_BLACK);
    delay(500);
    while (_ts.touched());

    // Compute linear calibration coefficients: screen_coord = raw * M + C
    _xM = (float)(_sw - 40) / (float)(x2 - x1);
    _xC = 20.0f - (float)x1 * _xM;
    _yM = (float)(_sh - 40) / (float)(y2 - y1);
    _yC = 20.0f - (float)y1 * _yM;
}

bool touch_kb_prompt_credentials(char *username, size_t username_max,
                                  char *password, size_t password_max)
{
    _tft.fillScreen(C_BG);

    if (!run_field(false, username, username_max)) {
        _tft.fillScreen(C_BG);
        return false;
    }

    _tft.fillScreen(C_BG);

    if (!run_field(true, password, password_max)) {
        _tft.fillScreen(C_BG);
        return false;
    }

    // Brief confirmation screen
    _tft.fillScreen(C_BG);
    _tft.setTextColor(ILI9341_GREEN);
    _tft.setTextSize(2);
    _tft.setCursor(20, 108);
    _tft.print("Credentials received!");
    delay(2000);
    _tft.fillScreen(C_BG);

    return true;
}
