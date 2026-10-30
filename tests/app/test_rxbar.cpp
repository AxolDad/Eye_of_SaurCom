// Receiver strip helpers: label, UTF-8 safe ellipsis, chip width, Ctrl+digit, scroll-into-view.
#include "../../app/state/rxbar.h"
#include <cstdio>
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
int main() {
    check(rxbar::label(1, "ADS-B", 1090.0) == "1 \xC2\xB7 ADS-B \xC2\xB7 1090.000 MHz", "label");
    check(rxbar::ellipsize("abcdef", 6) == "abcdef", "fits");
    check(rxbar::ellipsize("abcdef", 4) == "abc\xE2\x80\xA6", "cut");
    check(rxbar::ellipsize("abcdef", 1) == "\xE2\x80\xA6", "one char");
    check(rxbar::ellipsize("abc", 0).empty(), "zero");
    check(rxbar::ellipsize("\xC3\xA4\xC3\xB6\xC3\xBC\xC3\x9F", 3) == "\xC3\xA4\xC3\xB6\xE2\x80\xA6", "multibyte not split");
    check(rxbar::chipWidth(10, 5, 40, 100) == 40 && rxbar::chipWidth(200, 5, 40, 100) == 100 && rxbar::chipWidth(50, 5, 40, 100) == 55, "chip width clamp");
    check(rxbar::indexForDigit(1, 3) == 0 && rxbar::indexForDigit(3, 3) == 2 && rxbar::indexForDigit(4, 3) == -1 && rxbar::indexForDigit(0, 3) == -1, "digit map");
    check(rxbar::scrollToShow(0, 0, 50, 100) == 0 && rxbar::scrollToShow(0, 80, 150, 100) == 50 && rxbar::scrollToShow(100, 20, 60, 100) == 20, "scroll");
    {   // tab title with the radio, shortened so the frequency stays
        auto chars = [](const std::string& x) { return (float)rxbar::utf8Chars(x); };
        const std::string full = rxbar::label(2, "NESDR SMArt v5", "General receiver", 145.5);
        check(full == "2 \xC2\xB7 NESDR SMArt v5 \xC2\xB7 General receiver \xC2\xB7 145.500 MHz", "label with radio");
        check(rxbar::label(1, "", "FT8", 14.074) == "1 \xC2\xB7 FT8 \xC2\xB7 14.074 MHz", "no radio part");
        check(rxbar::fitLabel(2, "NESDR SMArt v5", "General receiver", 145.5, 1000.f, chars) == full, "fits whole");
        const std::string r = rxbar::fitLabel(2, "NESDR SMArt v5", "General receiver", 145.5, (float)rxbar::utf8Chars(full) - 3, chars);
        check(r == "2 \xC2\xB7 NESDR SMAr\xE2\x80\xA6 \xC2\xB7 General receiver \xC2\xB7 145.500 MHz", "radio gives way first");
        const std::string m = rxbar::fitLabel(2, "NESDR SMArt v5", "General receiver", 145.5, 40.f, chars);
        check(rxbar::utf8Chars(m) <= 40 && m.find("NESDR S") != std::string::npos && m.find("145.500 MHz") != std::string::npos, "then the mode");
        const std::string f = rxbar::fitLabel(2, "NESDR SMArt v5", "General receiver", 145.5, 25.f, chars);
        check(rxbar::utf8Chars(f) <= 25 && f.find("NESDR") == std::string::npos && f.find("145.500 MHz") != std::string::npos, "radio dropped");
        check(rxbar::fitLabel(2, "NESDR SMArt v5", "General receiver", 145.5, 3.f, chars) == "2 \xC2\xB7 145.500 MHz", "only number and frequency");
    }
    check(rxbar::zeroDigits("2 \xC2\xB7 RTL-SDR #2 \xC2\xB7 145.500 MHz") == "0 \xC2\xB7 RTL-SDR #0 \xC2\xB7 000.000 MHz", "digits measured as zeros");
    {   // a frequency the radio can tune
        check(rxbar::tuneInRange(145.5, 24, 1766) == 145.5, "rtl covers the default");
        check(rxbar::tuneInRange(145.5, 0, 0) == 145.5, "range unknown");
        check(rxbar::tuneInRange(2400, 24, 1766) == 100.0, "default out of range: FM broadcast");
        const double hf = rxbar::tuneInRange(145.5, 0.01, 30);
        check(hf > 0.01 && hf < 30, "an HF radio stays inside its range");
    }
    if (!fails) printf("ok\n");
    return fails;
}
