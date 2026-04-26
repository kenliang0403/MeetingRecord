#pragma once
/**
 * PixelFont.h — minimal 5×7 bitmap font for on-frame YUV annotation.
 *
 * Supports: '0'-'9', ':', ' ', A-Z (uppercase), '-', '.', '/'
 * Each glyph is 7 rows × 5 bits; bit 4 = leftmost pixel.
 *
 * Pure C++ header, no dependencies beyond <cstdint> <cstring> <ctime>.
 * Safe to include from both PTLib TUs and FFmpeg TUs.
 */

#include <cstdint>
#include <cstring>
#include <ctime>
#include <algorithm>

namespace pf {

// ── Glyph data ───────────────────────────────────────────────────────────────
// 5 pixels wide, 7 pixels tall.  Bit 4 = leftmost pixel.
struct Glyph { uint8_t rows[7]; };

// clang-format off
static const Glyph G_0  = {{0b01110,0b10001,0b10011,0b10101,0b11001,0b10001,0b01110}};
static const Glyph G_1  = {{0b00100,0b01100,0b00100,0b00100,0b00100,0b00100,0b01110}};
static const Glyph G_2  = {{0b01110,0b10001,0b00001,0b00110,0b01000,0b10000,0b11111}};
static const Glyph G_3  = {{0b01110,0b10001,0b00001,0b00110,0b00001,0b10001,0b01110}};
static const Glyph G_4  = {{0b00010,0b00110,0b01010,0b10010,0b11111,0b00010,0b00010}};
static const Glyph G_5  = {{0b11111,0b10000,0b11110,0b00001,0b00001,0b10001,0b01110}};
static const Glyph G_6  = {{0b00110,0b01000,0b10000,0b11110,0b10001,0b10001,0b01110}};
static const Glyph G_7  = {{0b11111,0b00001,0b00010,0b00100,0b01000,0b01000,0b01000}};
static const Glyph G_8  = {{0b01110,0b10001,0b10001,0b01110,0b10001,0b10001,0b01110}};
static const Glyph G_9  = {{0b01110,0b10001,0b10001,0b01111,0b00001,0b00010,0b01100}};
static const Glyph G_CO = {{0b00000,0b01100,0b01100,0b00000,0b01100,0b01100,0b00000}};  // ':'
static const Glyph G_SP = {{0,0,0,0,0,0,0}};                                             // ' '
static const Glyph G_A  = {{0b00100,0b01010,0b10001,0b11111,0b10001,0b10001,0b10001}};
static const Glyph G_C  = {{0b01110,0b10001,0b10000,0b10000,0b10000,0b10001,0b01110}};
static const Glyph G_E  = {{0b11111,0b10000,0b10000,0b11100,0b10000,0b10000,0b11111}};
static const Glyph G_G  = {{0b01110,0b10001,0b10000,0b10111,0b10001,0b10001,0b01110}};
static const Glyph G_I  = {{0b01110,0b00100,0b00100,0b00100,0b00100,0b00100,0b01110}};
static const Glyph G_L  = {{0b10000,0b10000,0b10000,0b10000,0b10000,0b10000,0b11111}};
static const Glyph G_N  = {{0b10001,0b11001,0b10101,0b10011,0b10001,0b10001,0b10001}};
static const Glyph G_R  = {{0b11110,0b10001,0b10001,0b11110,0b10100,0b10010,0b10001}};
static const Glyph G_S  = {{0b01110,0b10001,0b10000,0b01110,0b00001,0b10001,0b01110}};
static const Glyph G_U  = {{0b10001,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110}};
static const Glyph G_X  = {{0b10001,0b01010,0b00100,0b00100,0b00100,0b01010,0b10001}};
static const Glyph G_B  = {{0b11110,0b10001,0b10001,0b11110,0b10001,0b10001,0b11110}};
static const Glyph G_D  = {{0b11100,0b10010,0b10001,0b10001,0b10001,0b10010,0b11100}};
static const Glyph G_F  = {{0b11111,0b10000,0b10000,0b11100,0b10000,0b10000,0b10000}};
static const Glyph G_H  = {{0b10001,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001}};
static const Glyph G_J  = {{0b00111,0b00010,0b00010,0b00010,0b00010,0b10010,0b01100}};
static const Glyph G_K  = {{0b10001,0b10010,0b10100,0b11000,0b10100,0b10010,0b10001}};
static const Glyph G_M  = {{0b10001,0b11011,0b10101,0b10101,0b10001,0b10001,0b10001}};
static const Glyph G_O  = {{0b01110,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110}};
static const Glyph G_P  = {{0b11110,0b10001,0b10001,0b11110,0b10000,0b10000,0b10000}};
static const Glyph G_Q  = {{0b01110,0b10001,0b10001,0b10001,0b10101,0b01110,0b00001}};
static const Glyph G_T  = {{0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b00100}};
static const Glyph G_V  = {{0b10001,0b10001,0b10001,0b10001,0b01010,0b01010,0b00100}};
static const Glyph G_W  = {{0b10001,0b10001,0b10001,0b10101,0b10101,0b11011,0b10001}};
static const Glyph G_Y  = {{0b10001,0b10001,0b01010,0b00100,0b00100,0b00100,0b00100}};
static const Glyph G_Z  = {{0b11111,0b00001,0b00010,0b00100,0b01000,0b10000,0b11111}};
static const Glyph G_DA = {{0b00000,0b00000,0b00000,0b11111,0b00000,0b00000,0b00000}};  // '-'
static const Glyph G_DO = {{0b00000,0b00000,0b00000,0b00000,0b00000,0b01100,0b01100}};  // '.'
static const Glyph G_SL = {{0b00001,0b00010,0b00100,0b01000,0b10000,0b00000,0b00000}};  // '/'
// clang-format on

inline const Glyph* getGlyph(char c)
{
    switch (c) {
        case '0': return &G_0;
        case '1': return &G_1;
        case '2': return &G_2;
        case '3': return &G_3;
        case '4': return &G_4;
        case '5': return &G_5;
        case '6': return &G_6;
        case '7': return &G_7;
        case '8': return &G_8;
        case '9': return &G_9;
        case ':': return &G_CO;
        case ' ': return &G_SP;
        case 'A': return &G_A;
        case 'B': return &G_B;
        case 'C': return &G_C;
        case 'D': return &G_D;
        case 'E': return &G_E;
        case 'F': return &G_F;
        case 'G': return &G_G;
        case 'H': return &G_H;
        case 'I': return &G_I;
        case 'J': return &G_J;
        case 'K': return &G_K;
        case 'L': return &G_L;
        case 'M': return &G_M;
        case 'N': return &G_N;
        case 'O': return &G_O;
        case 'P': return &G_P;
        case 'Q': return &G_Q;
        case 'R': return &G_R;
        case 'S': return &G_S;
        case 'T': return &G_T;
        case 'U': return &G_U;
        case 'V': return &G_V;
        case 'W': return &G_W;
        case 'X': return &G_X;
        case 'Y': return &G_Y;
        case 'Z': return &G_Z;
        case '-': return &G_DA;
        case '.': return &G_DO;
        case '/': return &G_SL;
        default:  return &G_SP;
    }
}

// ── Drawing into a flat Y plane (stride == width) ────────────────────────────

/**
 * drawCharY — draw one character into Y plane.
 * @param Y        Y plane pointer (stride == frameW)
 * @param frameW/H frame dimensions (for bounds check)
 * @param x,y      top-left pixel position
 * @param scale    pixel magnification (1 = 5x7, 2 = 10x14, …)
 * @param luma     Y value for lit pixels
 */
inline void drawCharY(uint8_t* Y, int frameW, int frameH,
                      int x, int y, char c, int scale, uint8_t luma)
{
    const Glyph* g = getGlyph(c);
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if (g->rows[row] & (1u << (4 - col))) {
                for (int sy = 0; sy < scale; ++sy) {
                    for (int sx = 0; sx < scale; ++sx) {
                        int py = y + row * scale + sy;
                        int px = x + col * scale + sx;
                        if (py >= 0 && py < frameH && px >= 0 && px < frameW)
                            Y[py * frameW + px] = luma;
                    }
                }
            }
        }
    }
}

/**
 * drawStringY — draw a null-terminated ASCII string into Y plane.
 * Returns pixel width consumed (useful for centering).
 */
inline int drawStringY(uint8_t* Y, int frameW, int frameH,
                       int x, int y, const char* str, int scale, uint8_t luma)
{
    int cx = x;
    for (const char* p = str; *p; ++p) {
        drawCharY(Y, frameW, frameH, cx, y, *p, scale, luma);
        cx += 5 * scale + scale;  // glyph width + 1-pixel gap
    }
    return cx - x;
}

/** Measure the pixel width of a string at the given scale (no drawing). */
inline int measureString(const char* str, int scale)
{
    int len = 0;
    while (*str++) ++len;
    return len > 0 ? len * (5 * scale + scale) - scale : 0;
}

/** Fill a rectangular region of Y plane with a constant luma value. */
inline void fillRectY(uint8_t* Y, int frameW, int frameH,
                      int x, int y, int w, int h, uint8_t luma)
{
    for (int ry = 0; ry < h; ++ry) {
        int py = y + ry;
        if (py < 0 || py >= frameH) continue;
        for (int rx = 0; rx < w; ++rx) {
            int px = x + rx;
            if (px >= 0 && px < frameW)
                Y[py * frameW + px] = luma;
        }
    }
}

/** Draw current wall-clock time "HH:MM:SS" at (x,y). */
inline void drawClockY(uint8_t* Y, int frameW, int frameH,
                       int x, int y, int scale, uint8_t luma)
{
    time_t now = time(nullptr);
    struct tm* lt = localtime(&now);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
             lt->tm_hour, lt->tm_min, lt->tm_sec);
    drawStringY(Y, frameW, frameH, x, y, buf, scale, luma);
}

} // namespace pf
