#include "touch_keyboard.h"
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>
#include <FlashStorage.h>

struct CalibData
{
    uint32_t magic;
    float xM, xC, yM, yC;
};
static const uint32_t CALIB_MAGIC = 0xCA11B0U;
FlashStorage(calib_flash, CalibData);

// Hardware pins — must match physical wiring (same as working_touch_game.ino)
#define TKB_TFT_CS 10
#define TKB_TFT_DC 9
#define TKB_TFT_RST 8
#define TKB_TS_CS 7
#define TKB_ROTATION 2 // (240 wide × 320 tall)

static Adafruit_ILI9341 _tft(TKB_TFT_CS, TKB_TFT_DC, TKB_TFT_RST);
static XPT2046_Touchscreen _ts(TKB_TS_CS);

static float _xM = 1.0f, _xC = 0.0f;
static float _yM = 1.0f, _yC = 0.0f;
static int16_t _sw = 240;
static int16_t _sh = 320;

// ── Colors ───────────────────────────────────────────────────────────────────
static const uint16_t C_BG = ILI9341_BLACK;
static const uint16_t C_KEY_NRM = 0x2945;     // dark slate-blue
static const uint16_t C_KEY_SYMB = 0x39E7;    // slightly lighter — symbol/number keys
static const uint16_t C_KEY_DEL = 0xA000;     // dark red
static const uint16_t C_KEY_CANCEL = 0xD000;  // orange-red
static const uint16_t C_KEY_DONE = 0x0640;    // dark green
static const uint16_t C_KEY_SPACE = 0x4208;   // medium grey
static const uint16_t C_KEY_SHIFT_A = 0x041F; // bright blue — shift active (uppercase ON)
static const uint16_t C_KEY_PG = 0x528A;      // slate — page-switch keys
static const uint16_t C_KEY_PRESS = ILI9341_WHITE;
static const uint16_t C_KEY_TXT = ILI9341_WHITE;
static const uint16_t C_KEY_PTXT = ILI9341_BLACK;
static const uint16_t C_INPUT_BG = 0x0841; // very dark navy
static const uint16_t C_INPUT_BDR = ILI9341_CYAN;
static const uint16_t C_TITLE = ILI9341_YELLOW;

// ── Layout constants (portrait 240×320) ──────────────────────────────────────
//
//  y =  0..65   header:  title label (y=4) + input box (y=16, h=46)
//  y = 70..121  row 1:   10 keys
//  y = 126..177 row 2:   10 or 9 keys (ABC uses 9)
//  y = 182..233 row 3:   [SIDE] 7 chars [DEL]
//  y = 238..289 row 4:   [PAGE] [SPACE] [CANCEL] [DONE]

static const int16_t SW = 240;
static const int16_t SH = 320;

static const int16_t KEY_W = 21;   // normal key width
static const int16_t KEY_H = 52;   // all key heights
static const int16_t KEY_G = 3;    // horizontal gap between keys
static const int16_t ROW_GAP = 4;  // vertical gap between rows
static const int16_t SIDE_W = 33;  // row-3 side keys: SHIFT / #+= / 123
static const int16_t DEL_W = 33;   // DEL key (same width as SIDE)
static const int16_t R4_PG_W = 44; // row-4 page-switch key width
static const int16_t R4_SP_W = 93; // row-4 SPACE key width
static const int16_t R4_CA_W = 44; // row-4 CANCEL key width
static const int16_t R4_DN_W = 48; // row-4 DONE key width

static const int16_t IN_BOX_X = 4;
static const int16_t IN_BOX_Y = 16;
static const int16_t IN_BOX_W = 232; // full width (username)
static const int16_t IN_BOX_H = 46;
// Password field: text box is narrower to leave room for the SHOW/HIDE button
static const int16_t IN_PW_BOX_W = 183;
static const int16_t IN_SHOW_X = 192; // SHOW/HIDE button left edge
static const int16_t IN_SHOW_W = 44;  // SHOW/HIDE button width (192+44=236 ≤ 240)

static const int16_t ROW1_Y = 70;
static const int16_t ROW2_Y = ROW1_Y + KEY_H + ROW_GAP; // 126
static const int16_t ROW3_Y = ROW2_Y + KEY_H + ROW_GAP; // 182
static const int16_t ROW4_Y = ROW3_Y + KEY_H + ROW_GAP; // 238

// ── Key data ─────────────────────────────────────────────────────────────────
enum Page : uint8_t
{
    PAGE_ABC,
    PAGE_123,
    PAGE_PLUS
};

enum SpecialKey : uint8_t
{
    KEY_NORM,
    KEY_DEL_T,
    KEY_SPACE_T,
    KEY_DONE_T,
    KEY_CANCEL_T,
    KEY_SHIFT, // toggle upper/lowercase (ABC page)
    KEY_P123,  // switch to numbers/symbols page
    KEY_PPLUS, // switch to extended symbols page
    KEY_PABC,  // switch back to alphabet page
};

struct Key
{
    int16_t x, y, w, h;
    char ch;
    SpecialKey sp;
};

static Key s_keys[40];
static uint8_t s_nkeys = 0;
static bool s_is_upper = true; // uppercase when true

// ── Character sets ────────────────────────────────────────────────────────────
static const char ABC_R1U[] = "QWERTYUIOP";
static const char ABC_R1L[] = "qwertyuiop";
static const char ABC_R2U[] = "ASDFGHJKL";
static const char ABC_R2L[] = "asdfghjkl";
static const char ABC_R3U[] = "ZXCVBNM";
static const char ABC_R3L[] = "zxcvbnm";

static const char P123_R1[] = "1234567890";
static const char P123_R2[] = "!@#$%^&*()";
// Row 3 inner chars (7): period comma question exclamation apostrophe backtick underscore
static const char P123_R3[8] = {'.', ',', '?', '!', '\'', '`', '_', '\0'};

static const char PLUS_R1[11] = {'[', ']', '{', '}', '#', '%', '^', '*', '+', '=', '\0'};
static const char PLUS_R2[11] = {'_', '\\', '|', '~', '<', '>', '(', ')', ';', ':', '\0'};
// Row 3 inner chars (7): period comma question exclamation double-quote hyphen at-sign
static const char PLUS_R3[8] = {'.', ',', '?', '!', '"', '-', '@', '\0'};

// ── Key-table builder helpers ─────────────────────────────────────────────────

static void add_row10(const char *chars, int16_t ry)
{
    const int16_t total = 10 * KEY_W + 9 * KEY_G; // 237
    const int16_t sx = (SW - total) / 2;          // 1
    for (int i = 0; i < 10; i++)
        s_keys[s_nkeys++] = {(int16_t)(sx + i * (KEY_W + KEY_G)), ry,
                             KEY_W, KEY_H, chars[i], KEY_NORM};
}

static void add_row9(const char *chars, int16_t ry)
{
    const int16_t total = 9 * KEY_W + 8 * KEY_G; // 213
    const int16_t sx = (SW - total) / 2;         // 13
    for (int i = 0; i < 9; i++)
        s_keys[s_nkeys++] = {(int16_t)(sx + i * (KEY_W + KEY_G)), ry,
                             KEY_W, KEY_H, chars[i], KEY_NORM};
}

// Row 3: [SIDE_W] + n×normal keys + [DEL_W], all centered
static void add_row3(SpecialKey left_sp, const char *chars, int n, int16_t ry)
{
    const int16_t total = SIDE_W + KEY_G + n * KEY_W + (n - 1) * KEY_G + KEY_G + DEL_W; // 237
    const int16_t sx = (SW - total) / 2;                                                // 1
    // Left side key
    s_keys[s_nkeys++] = {sx, ry, SIDE_W, KEY_H, 0, left_sp};
    // Middle character keys
    const int16_t chars_x = sx + SIDE_W + KEY_G;
    for (int i = 0; i < n; i++)
        s_keys[s_nkeys++] = {(int16_t)(chars_x + i * (KEY_W + KEY_G)), ry,
                             KEY_W, KEY_H, chars[i], KEY_NORM};
    // DEL key
    const int16_t del_x = chars_x + n * KEY_W + (n - 1) * KEY_G + KEY_G;
    s_keys[s_nkeys++] = {del_x, ry, DEL_W, KEY_H, 0, KEY_DEL_T};
}

// Row 4: [PAGE_SWITCH] [SPACE] [CANCEL] [DONE], all centered
static void add_row4(SpecialKey pg_sp)
{
    const int16_t total = R4_PG_W + KEY_G + R4_SP_W + KEY_G + R4_CA_W + KEY_G + R4_DN_W; // 238
    const int16_t sx = (SW - total) / 2;                                                 // 1
    s_keys[s_nkeys++] = {sx, ROW4_Y, R4_PG_W, KEY_H, 0, pg_sp};
    s_keys[s_nkeys++] = {(int16_t)(sx + R4_PG_W + KEY_G), ROW4_Y, R4_SP_W, KEY_H, 0, KEY_SPACE_T};
    s_keys[s_nkeys++] = {(int16_t)(sx + R4_PG_W + KEY_G + R4_SP_W + KEY_G), ROW4_Y, R4_CA_W, KEY_H, 0, KEY_CANCEL_T};
    s_keys[s_nkeys++] = {(int16_t)(sx + R4_PG_W + KEY_G + R4_SP_W + KEY_G + R4_CA_W + KEY_G), ROW4_Y, R4_DN_W, KEY_H, 0, KEY_DONE_T};
}

static void build_keys(Page page)
{
    s_nkeys = 0;
    switch (page)
    {
    case PAGE_ABC:
        add_row10(s_is_upper ? ABC_R1U : ABC_R1L, ROW1_Y);
        add_row9(s_is_upper ? ABC_R2U : ABC_R2L, ROW2_Y);
        add_row3(KEY_SHIFT, s_is_upper ? ABC_R3U : ABC_R3L, 7, ROW3_Y);
        add_row4(KEY_P123);
        break;
    case PAGE_123:
        add_row10(P123_R1, ROW1_Y);
        add_row10(P123_R2, ROW2_Y);
        add_row3(KEY_PPLUS, P123_R3, 7, ROW3_Y);
        add_row4(KEY_PABC);
        break;
    case PAGE_PLUS:
        add_row10(PLUS_R1, ROW1_Y);
        add_row10(PLUS_R2, ROW2_Y);
        add_row3(KEY_P123, PLUS_R3, 7, ROW3_Y);
        add_row4(KEY_PABC);
        break;
    }
}

// ── Drawing helpers ───────────────────────────────────────────────────────────

static uint16_t key_bg(const Key &k, bool pressed)
{
    if (pressed)
        return C_KEY_PRESS;
    switch (k.sp)
    {
    case KEY_DEL_T:
        return C_KEY_DEL;
    case KEY_CANCEL_T:
        return C_KEY_CANCEL;
    case KEY_DONE_T:
        return C_KEY_DONE;
    case KEY_SPACE_T:
        return C_KEY_SPACE;
    case KEY_SHIFT:
        return s_is_upper ? C_KEY_SHIFT_A : C_KEY_NRM;
    case KEY_P123:
    case KEY_PPLUS:
    case KEY_PABC:
        return C_KEY_PG;
    default:
        return C_KEY_NRM;
    }
}

static void draw_key(uint8_t i, bool pressed)
{
    const Key &k = s_keys[i];
    uint16_t bg = key_bg(k, pressed);
    uint16_t fg = pressed ? C_KEY_PTXT : C_KEY_TXT;

    _tft.fillRoundRect(k.x, k.y, k.w, k.h, 4, bg);
    _tft.drawRoundRect(k.x, k.y, k.w, k.h, 4, ILI9341_WHITE);
    _tft.setTextColor(fg);

    if (k.sp == KEY_NORM)
    {
        // Single char, textSize 2 (12×16 px), centered
        _tft.setTextSize(2);
        _tft.setCursor(k.x + (k.w - 12) / 2, k.y + (k.h - 16) / 2);
        _tft.print(k.ch);
    }
    else
    {
        // Label string, textSize 1 (6×8 px per char), centered
        const char *lbl;
        switch (k.sp)
        {
        case KEY_DEL_T:
            lbl = "DEL";
            break;
        case KEY_CANCEL_T:
            lbl = "CANCEL";
            break;
        case KEY_DONE_T:
            lbl = "DONE";
            break;
        case KEY_SPACE_T:
            lbl = "SPACE";
            break;
        case KEY_SHIFT:
            lbl = "SH";
            break; // background color shows state
        case KEY_P123:
            lbl = "123";
            break;
        case KEY_PPLUS:
            lbl = "#+=";
            break;
        case KEY_PABC:
            lbl = "ABC";
            break;
        default:
            lbl = "?";
            break;
        }
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

// show_pw: only relevant when is_pw=true; true = display plaintext, false = asterisks
static void draw_header(bool is_pw, const char *buf, bool show_pw)
{
    // Repaint only the header area to avoid flickering the keyboard
    _tft.fillRect(0, 0, SW, ROW1_Y - 3, C_BG);

    _tft.setCursor(IN_BOX_X, 4);
    _tft.setTextSize(1);
    _tft.setTextColor(C_TITLE);
    _tft.print(is_pw ? "ENTER PASSWORD:" : "ENTER USERNAME:");

    // For password fields the text box is narrower (SHOW/HIDE button fills the gap)
    int16_t box_w = (is_pw) ? IN_PW_BOX_W : IN_BOX_W;
    _tft.fillRect(IN_BOX_X, IN_BOX_Y, box_w, IN_BOX_H, C_INPUT_BG);
    _tft.drawRect(IN_BOX_X, IN_BOX_Y, box_w, IN_BOX_H, C_INPUT_BDR);

    // Text: vertically centered in box; textSize 2 (charH=16)
    _tft.setTextSize(2);
    _tft.setTextColor(ILI9341_WHITE);
    _tft.setCursor(IN_BOX_X + 4, IN_BOX_Y + (IN_BOX_H - 16) / 2);

    const size_t len = strlen(buf);
    if (is_pw && !show_pw)
    {
        for (size_t j = 0; j < len; j++)
            _tft.print('*');
    }
    else
    {
        _tft.print(buf);
    }
    _tft.print('_'); // static cursor marker

    // SHOW / HIDE toggle button (password field only)
    if (is_pw)
    {
        uint16_t btn_bg = show_pw ? C_KEY_SHIFT_A : C_KEY_NRM;
        const char *lbl = show_pw ? "HIDE" : "SHOW";
        _tft.fillRoundRect(IN_SHOW_X, IN_BOX_Y, IN_SHOW_W, IN_BOX_H, 4, btn_bg);
        _tft.drawRoundRect(IN_SHOW_X, IN_BOX_Y, IN_SHOW_W, IN_BOX_H, 4, C_INPUT_BDR);
        int16_t lw = (int16_t)(strlen(lbl) * 6);
        _tft.setTextSize(1);
        _tft.setTextColor(ILI9341_WHITE);
        _tft.setCursor(IN_SHOW_X + (IN_SHOW_W - lw) / 2, IN_BOX_Y + (IN_BOX_H - 8) / 2);
        _tft.print(lbl);
    }
}

// ── Touch helpers ─────────────────────────────────────────────────────────────

static bool touch_xy(int16_t &sx, int16_t &sy)
{
    if (!_ts.touched())
        return false;
    TS_Point p = _ts.getPoint();
    sx = (int16_t)((float)p.x * _xM + _xC);
    sy = (int16_t)((float)p.y * _yM + _yC);
    if (sx < 0)
        sx = 0;
    if (sx >= _sw)
        sx = _sw - 1;
    if (sy < 0)
        sy = 0;
    if (sy >= _sh)
        sy = _sh - 1;
    return true;
}

static int8_t hit_test(int16_t sx, int16_t sy)
{
    for (uint8_t i = 0; i < s_nkeys; i++)
    {
        const Key &k = s_keys[i];
        if (sx >= k.x && sx < k.x + k.w && sy >= k.y && sy < k.y + k.h)
            return (int8_t)i;
    }
    return -1;
}

// ── Single-field prompt ───────────────────────────────────────────────────────

// Runs the keyboard for one field. Returns true (DONE) or false (CANCEL).
static bool run_field(bool is_pw, char *buf, size_t maxlen)
{
    size_t len = 0;
    buf[0] = '\0';
    Page cur_page = PAGE_ABC;
    s_is_upper = true;    // start each field in uppercase
    bool show_pw = false; // password visibility (ignored for username field)

    build_keys(cur_page);
    draw_header(is_pw, buf, show_pw);
    draw_all_keys();

    bool prev_touched = false;

    for (;;)
    {
        int16_t sx, sy;
        bool now_touched = touch_xy(sx, sy);

        // Act only on the rising edge of a touch
        if (now_touched && !prev_touched)
        {
            // Check SHOW/HIDE button first (password fields only)
            if (is_pw &&
                sx >= IN_SHOW_X && sx < IN_SHOW_X + IN_SHOW_W &&
                sy >= IN_BOX_Y && sy < IN_BOX_Y + IN_BOX_H)
            {
                show_pw = !show_pw;
                draw_header(is_pw, buf, show_pw);
            }
            else
            {
                int8_t ki = hit_test(sx, sy);
                if (ki >= 0)
                {
                    const Key &k = s_keys[ki];

                    // Visual press feedback
                    draw_key((uint8_t)ki, true);
                    delay(80);
                    draw_key((uint8_t)ki, false);

                    bool need_rebuild = false;

                    switch (k.sp)
                    {
                    case KEY_DONE_T:
                        return true;

                    case KEY_CANCEL_T:
                        return false;

                    case KEY_DEL_T:
                        if (len > 0)
                        {
                            buf[--len] = '\0';
                            draw_header(is_pw, buf, show_pw);
                        }
                        break;

                    case KEY_SPACE_T:
                        if (len + 1 < maxlen)
                        {
                            buf[len++] = ' ';
                            buf[len] = '\0';
                            draw_header(is_pw, buf, show_pw);
                        }
                        break;

                    case KEY_SHIFT:
                        s_is_upper = !s_is_upper;
                        need_rebuild = true;
                        break;

                    case KEY_P123:
                        cur_page = PAGE_123;
                        need_rebuild = true;
                        break;

                    case KEY_PPLUS:
                        cur_page = PAGE_PLUS;
                        need_rebuild = true;
                        break;

                    case KEY_PABC:
                        cur_page = PAGE_ABC;
                        need_rebuild = true;
                        break;

                    default: // KEY_NORM — regular character
                        if (len + 1 < maxlen)
                        {
                            buf[len++] = k.ch;
                            buf[len] = '\0';
                            draw_header(is_pw, buf, show_pw);
                        }
                        break;
                    }

                    if (need_rebuild)
                    {
                        // Clear the full keyboard area first so leftover keys from
                        // rows with different key counts don't bleed through.
                        _tft.fillRect(0, ROW1_Y, _sw, ROW4_Y + KEY_H - ROW1_Y, C_BG);
                        build_keys(cur_page);
                        draw_all_keys();
                    }
                }
            }
        }

        prev_touched = now_touched;
        delay(20);
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void touch_kb_setup()
{
    // De-assert all SPI CS lines before calling begin() to avoid bus contention
    pinMode(TKB_TS_CS, OUTPUT);
    digitalWrite(TKB_TS_CS, HIGH);
    pinMode(TKB_TFT_CS, OUTPUT);
    digitalWrite(TKB_TFT_CS, HIGH);

    _tft.begin();
    _tft.setRotation(TKB_ROTATION);
    _tft.fillScreen(C_BG);
    _sw = _tft.width();  // 240 in portrait
    _sh = _tft.height(); // 320 in portrait

    _ts.begin();
    _ts.setRotation(TKB_ROTATION);
}

static void run_calibration_sequence()
{
    TS_Point p;
    int16_t x1, y1, x2, y2;

    _tft.fillScreen(ILI9341_BLACK);
    while (_ts.touched())
        ;

    // ── Crosshair 1: top-left corner ─────────────────────────────────────────
    _tft.drawFastHLine(10, 20, 20, ILI9341_RED);
    _tft.drawFastVLine(20, 10, 20, ILI9341_RED);
    _tft.setCursor(38, 14);
    _tft.setTextSize(1);
    _tft.setTextColor(ILI9341_WHITE);
    _tft.print("Touch to calibrate");
    while (!_ts.touched())
        ;
    p = _ts.getPoint();
    x1 = p.x;
    y1 = p.y;
    _tft.fillScreen(ILI9341_BLACK);
    delay(500);
    while (_ts.touched())
        ;

    // ── Crosshair 2: bottom-right corner ─────────────────────────────────────
    _tft.drawFastHLine(_sw - 30, _sh - 20, 20, ILI9341_RED);
    _tft.drawFastVLine(_sw - 20, _sh - 30, 20, ILI9341_RED);
    _tft.setCursor(4, _sh - 35);
    _tft.setTextSize(1);
    _tft.setTextColor(ILI9341_WHITE);
    _tft.print("Touch to calibrate");
    while (!_ts.touched())
        ;
    p = _ts.getPoint();
    x2 = p.x;
    y2 = p.y;
    _tft.fillScreen(ILI9341_BLACK);
    delay(500);
    while (_ts.touched())
        ;

    // Compute linear map: screen_coord = raw * M + C
    _xM = (float)(_sw - 40) / (float)(x2 - x1);
    _xC = 20.0f - (float)x1 * _xM;
    _yM = (float)(_sh - 40) / (float)(y2 - y1);
    _yC = 20.0f - (float)y1 * _yM;

    CalibData d = {CALIB_MAGIC, _xM, _xC, _yM, _yC};
    calib_flash.write(d);
}

void touch_kb_calibrate()
{
    CalibData d = calib_flash.read();
    if (d.magic == CALIB_MAGIC &&
        d.xM != 0.0f && d.yM != 0.0f &&
        d.xM == d.xM && d.yM == d.yM) // NaN check
    {
        _xM = d.xM;
        _xC = d.xC;
        _yM = d.yM;
        _yC = d.yC;
        return;
    }
    run_calibration_sequence();
}

void touch_kb_force_calibrate()
{
    run_calibration_sequence();
}

bool touch_kb_confirm_login(const char *domain)
{
    _tft.fillScreen(C_BG);

    // ── Title ─────────────────────────────────────────────────────────────────
    _tft.setTextColor(C_TITLE);
    _tft.setTextSize(2);
    // "LOGIN REQUEST" = 13 chars × 12 px = 156 px  →  center x = (240-156)/2 = 42
    _tft.setCursor(42, 10);
    _tft.print("LOGIN REQUEST");
    _tft.drawFastHLine(0, 30, _sw, 0x4208); // subtle divider

    // ── Subtitle ──────────────────────────────────────────────────────────────
    _tft.setTextColor(ILI9341_WHITE);
    _tft.setTextSize(1);
    _tft.setCursor(10, 38);
    _tft.print("Login request from:");

    // ── Domain box ────────────────────────────────────────────────────────────
    _tft.fillRect(10, 50, 220, 46, C_INPUT_BG);
    _tft.drawRect(10, 50, 220, 46, C_INPUT_BDR);

    const char *disp = (strlen(domain) > 0) ? domain : "<unknown>";
    size_t dlen = strlen(disp);
    _tft.setTextColor(ILI9341_CYAN);
    if (dlen <= 17)
    {
        // Fits at textSize 2 (max 17 × 12 = 204 px in 212 px usable)
        _tft.setTextSize(2);
        int16_t dx = 10 + (220 - (int16_t)(dlen * 12)) / 2;
        _tft.setCursor(dx, 50 + (46 - 16) / 2);
    }
    else
    {
        // Fall back to textSize 1 (max 35 × 6 = 210 px; cert domain ≤ 32 chars)
        _tft.setTextSize(1);
        int16_t dx = 10 + (220 - (int16_t)(dlen * 6)) / 2;
        if (dx < 14)
            dx = 14;
        _tft.setCursor(dx, 50 + (46 - 8) / 2);
    }
    _tft.print(disp);

    // ── Warning text ──────────────────────────────────────────────────────────
    _tft.setTextColor(ILI9341_WHITE);
    _tft.setTextSize(1);
    _tft.setCursor(10, 108);
    _tft.print("Verify the domain above.");
    _tft.setCursor(10, 120);
    _tft.print("Reject if you did not");
    _tft.setCursor(10, 132);
    _tft.print("initiate this request.");
    _tft.drawFastHLine(0, 150, _sw, 0x4208); // subtle divider

    // ── Buttons ───────────────────────────────────────────────────────────────
    const int16_t BTN_X = 20;
    const int16_t BTN_W = 200;
    const int16_t BTN_H = 62;
    const int16_t ACCEPT_Y = 162;
    const int16_t REJECT_Y = 234;
    // "ACCEPT" / "REJECT" = 6 chars × 12 px = 72 px at textSize 2
    const int16_t LBL_OFFSET = (BTN_W - 72) / 2; // 64

    _tft.fillRoundRect(BTN_X, ACCEPT_Y, BTN_W, BTN_H, 8, C_KEY_DONE);
    _tft.drawRoundRect(BTN_X, ACCEPT_Y, BTN_W, BTN_H, 8, ILI9341_WHITE);
    _tft.setTextColor(ILI9341_WHITE);
    _tft.setTextSize(2);
    _tft.setCursor(BTN_X + LBL_OFFSET, ACCEPT_Y + (BTN_H - 16) / 2);
    _tft.print("ACCEPT");

    _tft.fillRoundRect(BTN_X, REJECT_Y, BTN_W, BTN_H, 8, C_KEY_DEL);
    _tft.drawRoundRect(BTN_X, REJECT_Y, BTN_W, BTN_H, 8, ILI9341_WHITE);
    _tft.setTextColor(ILI9341_WHITE);
    _tft.setTextSize(2);
    _tft.setCursor(BTN_X + LBL_OFFSET, REJECT_Y + (BTN_H - 16) / 2);
    _tft.print("REJECT");

    // ── Wait for decision ─────────────────────────────────────────────────────
    while (_ts.touched())
        ; // flush any residual touch

    bool prev_touched = false;
    for (;;)
    {
        int16_t sx, sy;
        bool now_touched = touch_xy(sx, sy);

        if (now_touched && !prev_touched && sx >= BTN_X && sx < BTN_X + BTN_W)
        {
            bool accepted = false;
            bool hit = false;

            if (sy >= ACCEPT_Y && sy < ACCEPT_Y + BTN_H)
            {
                accepted = true;
                hit = true;
            }
            if (sy >= REJECT_Y && sy < REJECT_Y + BTN_H)
            {
                accepted = false;
                hit = true;
            }

            if (hit)
            {
                // Flash the pressed button white
                int16_t fy = accepted ? ACCEPT_Y : REJECT_Y;
                _tft.fillRoundRect(BTN_X, fy, BTN_W, BTN_H, 8, C_KEY_PRESS);
                delay(80);
                while (_ts.touched())
                    ;
                _tft.fillScreen(C_BG);
                return accepted;
            }
        }

        prev_touched = now_touched;
        delay(20);
    }
}

bool touch_kb_confirm_sensitive(const char *domain, const char *description)
{
    _tft.fillScreen(C_BG);

    // ── Title ─────────────────────────────────────────────────────────────────
    // "SENSITIVE REQUEST" = 17 chars × 12 px = 204 px → x = (240-204)/2 = 18
    _tft.setTextColor(ILI9341_ORANGE);
    _tft.setTextSize(2);
    _tft.setCursor(18, 4);
    _tft.print("SENSITIVE REQUEST");
    _tft.drawFastHLine(0, 24, _sw, 0x4208);

    // ── Domain ────────────────────────────────────────────────────────────────
    // Print "From: " in muted grey then the domain in cyan on the same line.
    // textSize 1: 6 px/char, max 30 domain chars before hitting screen edge.
    _tft.setTextSize(1);
    _tft.setCursor(10, 30);
    _tft.setTextColor(0x8410); // mid-grey
    _tft.print("From: ");
    _tft.setTextColor(ILI9341_CYAN);
    const char *dom = (domain && strlen(domain) > 0) ? domain : "unknown";
    // Truncate to 30 chars so "From: " + domain stays within 220 px
    size_t domlen = strlen(dom);
    if (domlen > 30) domlen = 30;
    for (size_t i = 0; i < domlen; i++) _tft.print(dom[i]);

    // ── Subtitle ──────────────────────────────────────────────────────────────
    _tft.setTextColor(ILI9341_WHITE);
    _tft.setTextSize(1);
    _tft.setCursor(10, 42);
    _tft.print("Approve this action?");

    // ── Description box — 108 px tall to maximise readability ─────────────────
    const int16_t BOX_X = 10, BOX_Y = 54, BOX_W = 220, BOX_H = 108;
    _tft.fillRect(BOX_X, BOX_Y, BOX_W, BOX_H, C_INPUT_BG);
    _tft.drawRect(BOX_X, BOX_Y, BOX_W, BOX_H, C_INPUT_BDR);

    const char *disp = (description && strlen(description) > 0) ? description : "<no description>";
    const int16_t dlen       = (int16_t)strlen(disp);
    const int16_t CHARS_LINE = 17;   // 17 × 12 px = 204 px fits inside 212 px usable
    const int16_t CHAR_W     = 12;   // textSize 2: 6-px base × 2
    const int16_t CHAR_H     = 16;   // textSize 2: 8-px base × 2
    const int16_t LINE_GAP   = 6;

    _tft.setTextColor(ILI9341_CYAN);
    _tft.setTextSize(2); // always large — box is now tall enough

    if (dlen <= CHARS_LINE)
    {
        // Single line, vertically centred in the box
        int16_t tx = BOX_X + (BOX_W - dlen * CHAR_W) / 2;
        int16_t ty = BOX_Y + (BOX_H - CHAR_H) / 2;
        _tft.setCursor(tx, ty);
        _tft.print(disp);
    }
    else
    {
        // Two lines, centred as a group
        int16_t l2len  = dlen - CHARS_LINE;
        if (l2len > CHARS_LINE) l2len = CHARS_LINE; // cap at 17
        int16_t total_h = CHAR_H + LINE_GAP + CHAR_H;
        int16_t ty1    = BOX_Y + (BOX_H - total_h) / 2;
        int16_t ty2    = ty1 + CHAR_H + LINE_GAP;

        // Line 1 (always full 17 chars)
        int16_t tx1 = BOX_X + (BOX_W - CHARS_LINE * CHAR_W) / 2;
        _tft.setCursor(tx1, ty1);
        for (int16_t i = 0; i < CHARS_LINE; i++)
            _tft.print(disp[i]);

        // Line 2
        int16_t tx2 = BOX_X + (BOX_W - l2len * CHAR_W) / 2;
        _tft.setCursor(tx2, ty2);
        for (int16_t i = 0; i < l2len; i++)
            _tft.print(disp[CHARS_LINE + i]);
    }

    // ── Warning (single compact line) ─────────────────────────────────────────
    // Box bottom = BOX_Y(54) + BOX_H(108) = 162
    _tft.setTextColor(ILI9341_YELLOW);
    _tft.setTextSize(1);
    _tft.setCursor(10, 166);
    _tft.print("Only approve what you requested.");
    _tft.drawFastHLine(0, 178, _sw, 0x4208);

    // ── Buttons — 44 px tall (down from 62) ──────────────────────────────────
    const int16_t BTN_X    = 20;
    const int16_t BTN_W    = 200;
    const int16_t BTN_H    = 44;
    const int16_t APPROVE_Y = 186;
    const int16_t REJECT_Y  = 234; // 186 + 44 + 4 gap

    _tft.fillRoundRect(BTN_X, APPROVE_Y, BTN_W, BTN_H, 6, C_KEY_DONE);
    _tft.drawRoundRect(BTN_X, APPROVE_Y, BTN_W, BTN_H, 6, ILI9341_WHITE);
    _tft.setTextColor(ILI9341_WHITE);
    _tft.setTextSize(2);
    // "APPROVE" = 7 × 12 = 84 px
    _tft.setCursor(BTN_X + (BTN_W - 84) / 2, APPROVE_Y + (BTN_H - 16) / 2);
    _tft.print("APPROVE");

    _tft.fillRoundRect(BTN_X, REJECT_Y, BTN_W, BTN_H, 6, C_KEY_DEL);
    _tft.drawRoundRect(BTN_X, REJECT_Y, BTN_W, BTN_H, 6, ILI9341_WHITE);
    _tft.setTextColor(ILI9341_WHITE);
    _tft.setTextSize(2);
    // "REJECT" = 6 × 12 = 72 px
    _tft.setCursor(BTN_X + (BTN_W - 72) / 2, REJECT_Y + (BTN_H - 16) / 2);
    _tft.print("REJECT");

    // ── Wait for decision ─────────────────────────────────────────────────────
    while (_ts.touched())
        ;

    bool prev_touched = false;
    for (;;)
    {
        int16_t sx, sy;
        bool now_touched = touch_xy(sx, sy);

        if (now_touched && !prev_touched && sx >= BTN_X && sx < BTN_X + BTN_W)
        {
            bool approved = false;
            bool hit = false;

            if (sy >= APPROVE_Y && sy < APPROVE_Y + BTN_H) { approved = true;  hit = true; }
            if (sy >= REJECT_Y  && sy < REJECT_Y  + BTN_H) { approved = false; hit = true; }

            if (hit)
            {
                int16_t fy = approved ? APPROVE_Y : REJECT_Y;
                _tft.fillRoundRect(BTN_X, fy, BTN_W, BTN_H, 6, C_KEY_PRESS);
                delay(80);
                while (_ts.touched())
                    ;
                _tft.fillScreen(C_BG);
                return approved;
            }
        }

        prev_touched = now_touched;
        delay(20);
    }
}

bool touch_kb_confirm_reconf(const char *domain)
{
    _tft.fillScreen(C_BG);

    // ── Title ─────────────────────────────────────────────────────────────────
    // "SESSION RENEWAL" = 15 chars × 12 px = 180 px → x = (240-180)/2 = 30
    _tft.setTextColor(ILI9341_YELLOW);
    _tft.setTextSize(2);
    _tft.setCursor(30, 10);
    _tft.print("SESSION RENEWAL");
    _tft.drawFastHLine(0, 30, _sw, 0x4208);

    // ── Prompt ────────────────────────────────────────────────────────────────
    _tft.setTextColor(ILI9341_WHITE);
    _tft.setTextSize(1);
    _tft.setCursor(10, 38);
    _tft.print("Extend session with:");

    // ── Domain box ────────────────────────────────────────────────────────────
    _tft.fillRect(10, 50, 220, 46, C_INPUT_BG);
    _tft.drawRect(10, 50, 220, 46, C_INPUT_BDR);

    const char *disp = (domain && strlen(domain) > 0) ? domain : "<unknown>";
    size_t dlen = strlen(disp);
    _tft.setTextColor(ILI9341_CYAN);
    if (dlen <= 17)
    {
        _tft.setTextSize(2);
        int16_t dx = 10 + (220 - (int16_t)(dlen * 12)) / 2;
        _tft.setCursor(dx, 50 + (46 - 16) / 2);
    }
    else
    {
        _tft.setTextSize(1);
        int16_t dx = 10 + (220 - (int16_t)(dlen * 6)) / 2;
        if (dx < 14) dx = 14;
        _tft.setCursor(dx, 50 + (46 - 8) / 2);
    }
    _tft.print(disp);

    // ── Info text ─────────────────────────────────────────────────────────────
    _tft.setTextColor(ILI9341_WHITE);
    _tft.setTextSize(1);
    _tft.setCursor(10, 108);
    _tft.print("A new session key will be");
    _tft.setCursor(10, 120);
    _tft.print("generated for this domain.");
    _tft.setCursor(10, 136);
    _tft.setTextColor(ILI9341_YELLOW);
    _tft.print("Reject if unexpected.");
    _tft.drawFastHLine(0, 150, _sw, 0x4208);

    // ── Buttons ───────────────────────────────────────────────────────────────
    const int16_t BTN_X = 20, BTN_W = 200, BTN_H = 62;
    const int16_t APPROVE_Y = 162, REJECT_Y = 234;

    _tft.fillRoundRect(BTN_X, APPROVE_Y, BTN_W, BTN_H, 8, C_KEY_DONE);
    _tft.drawRoundRect(BTN_X, APPROVE_Y, BTN_W, BTN_H, 8, ILI9341_WHITE);
    _tft.setTextColor(ILI9341_WHITE);
    _tft.setTextSize(2);
    // "APPROVE" = 7 × 12 = 84 px
    _tft.setCursor(BTN_X + (BTN_W - 84) / 2, APPROVE_Y + (BTN_H - 16) / 2);
    _tft.print("APPROVE");

    _tft.fillRoundRect(BTN_X, REJECT_Y, BTN_W, BTN_H, 8, C_KEY_DEL);
    _tft.drawRoundRect(BTN_X, REJECT_Y, BTN_W, BTN_H, 8, ILI9341_WHITE);
    _tft.setTextColor(ILI9341_WHITE);
    _tft.setTextSize(2);
    // "REJECT" = 6 × 12 = 72 px
    _tft.setCursor(BTN_X + (BTN_W - 72) / 2, REJECT_Y + (BTN_H - 16) / 2);
    _tft.print("REJECT");

    // ── Wait for decision ─────────────────────────────────────────────────────
    while (_ts.touched())
        ;

    bool prev_touched = false;
    for (;;)
    {
        int16_t sx, sy;
        bool now_touched = touch_xy(sx, sy);

        if (now_touched && !prev_touched && sx >= BTN_X && sx < BTN_X + BTN_W)
        {
            bool approved = false;
            bool hit = false;

            if (sy >= APPROVE_Y && sy < APPROVE_Y + BTN_H) { approved = true;  hit = true; }
            if (sy >= REJECT_Y  && sy < REJECT_Y  + BTN_H) { approved = false; hit = true; }

            if (hit)
            {
                int16_t fy = approved ? APPROVE_Y : REJECT_Y;
                _tft.fillRoundRect(BTN_X, fy, BTN_W, BTN_H, 8, C_KEY_PRESS);
                delay(80);
                while (_ts.touched())
                    ;
                _tft.fillScreen(C_BG);
                return approved;
            }
        }

        prev_touched = now_touched;
        delay(20);
    }
}

bool touch_kb_prompt_credentials(char *username, size_t username_max,
                                 char *password, size_t password_max)
{
    _tft.fillScreen(C_BG);

    if (!run_field(false, username, username_max))
    {
        _tft.fillScreen(C_BG);
        return false;
    }

    _tft.fillScreen(C_BG);

    if (!run_field(true, password, password_max))
    {
        _tft.fillScreen(C_BG);
        return false;
    }

    // Brief confirmation screen
    _tft.fillScreen(C_BG);
    _tft.setTextColor(ILI9341_GREEN);
    _tft.setTextSize(2);
    int16_t tx = 20, ty = (_sh - 16) / 2;
    _tft.setCursor(tx, ty);
    _tft.print("Credentials received!");
    delay(2000);
    _tft.fillScreen(C_BG);

    return true;
}
