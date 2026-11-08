// Composed accented letters for the 8x8 sokol-debugtext fonts (which are plain ASCII).
// UTF-8 text is converted to ASCII where letters like o-umlaut become a one-byte code
// (so strlen()/layout still count one cell per letter); textfx_puts() then draws the base
// letter plus small dots / accents shifted above it.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "sokol_debugtext.h"

enum {
    TFX_A_UML = 1, TFX_O_UML, TFX_U_UML, TFX_A_RING, TFX_E_ACUTE,
    TFX_CA_UML, TFX_CO_UML, TFX_CA_RING, TFX_CU_UML,
};

// returns the code for a 2-byte UTF-8 sequence starting with 0xC3, or 0
static inline int textfx_code(unsigned char second) {
    switch (second) {
        case 0xA4: return TFX_A_UML;
        case 0xB6: return TFX_O_UML;
        case 0xBC: return TFX_U_UML;
        case 0xA5: return TFX_A_RING;
        case 0xA9: return TFX_E_ACUTE;
        case 0x84: return TFX_CA_UML;
        case 0x96: return TFX_CO_UML;
        case 0x85: return TFX_CA_RING;
        case 0x9C: return TFX_CU_UML;
        default: return 0;
    }
}

// UTF-8 -> ASCII + composed-letter codes (other non-ASCII characters become '?')
static inline void textfx_from_utf8(const char* in, char* out, size_t cap) {
    size_t o = 0;
    const unsigned char* p = (const unsigned char*)in;
    while (*p && o + 1 < cap) {
        if (*p < 0x80) {
            out[o++] = (char)*p++;
            continue;
        }
        int len = (*p >= 0xF0) ? 4 : (*p >= 0xE0) ? 3 : 2;
        int code = (p[0] == 0xC3 && p[1]) ? textfx_code(p[1]) : 0;
        out[o++] = code ? (char)code : '?';
        for (int i = 0; i < len && *p; i++) p++;
    }
    out[o] = 0;
}

// draws a small dot ('.') with its centre offset by (dx, dy) cells from the current cell
static inline void textfx_dot(float dx, float dy) {
    sdtx_move_x(-1.0f + dx);
    sdtx_move_y(dy);
    sdtx_putc('.');
    sdtx_move_x(-dx);
    sdtx_move_y(-dy);
}

static inline void textfx_putc(char ch) {
    int c = (unsigned char)ch;
    if (c == 0 || c > TFX_CU_UML) {
        sdtx_putc(ch);
        return;
    }
    static const char base[] = { 0, 'a', 'o', 'u', 'a', 'e', 'A', 'O', 'A', 'U' };
    bool upper = c >= TFX_CA_UML;
    sdtx_putc(base[c]);
    float up = upper ? -0.95f : -0.75f;   // lift the '.' glyph above the letter
    switch (c) {
        case TFX_A_RING: case TFX_CA_RING:
            textfx_dot(0.0f, up);
            break;
        case TFX_E_ACUTE:
            sdtx_move_x(-1.0f + 0.12f);
            sdtx_move_y(-0.30f);
            sdtx_putc('\'');
            sdtx_move_x(-0.12f);
            sdtx_move_y(0.30f);
            break;
        default:
            textfx_dot(-0.22f, up);
            textfx_dot(0.22f, up);
            break;
    }
}

static inline void textfx_puts(const char* s) {
    for (; *s; s++) {
        textfx_putc(*s);
    }
}
