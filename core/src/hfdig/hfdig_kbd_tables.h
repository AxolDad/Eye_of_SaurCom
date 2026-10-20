// Tables of the keyboard modes (hfdig_kbd_tables.cpp, generated from fldigi's sources).
#pragma once
namespace dect2 {
namespace kbd {
extern const char* const kMfskVaricode[256];            // IZ8BLY MFSK varicode: the bit string of each character
extern const unsigned char kDominoVaricode[256][3];     // DominoEX primary alphabet: nibbles, a following nibble has bit 3 set (0 = none)
extern const unsigned short kHellFont[95][14];          // ' ' .. '~'
// The 14 pixel rows of a Unicode code point: ASCII is the Feld font unchanged (fldigi compatible); Latin-1 letters with accents are the base
// letter with an accent drawn in the two blank top rows; anything else is '?'. Returns false when the code point had no glyph.
bool hellGlyph(int cp, unsigned short out[14]);
} // namespace kbd
} // namespace dect2
