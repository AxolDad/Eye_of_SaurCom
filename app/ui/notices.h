// A small stack of notices in the bottom-right corner, each with buttons ("New radio: ...", Name it / Use in new receiver tab / Dismiss).
// Text is shown as text only (never as a format string), so a radio's name can be put in it as it is.
#pragma once
#include <functional>
#include <string>
#include <vector>

struct NoticeButton {
    std::string label;                  // already translated
    std::function<void()> onClick;      // may be empty (a plain Dismiss)
    bool dismiss = true;                // the notice goes away after the click
};

// Show a notice. seconds 0 = until a button is pressed; otherwise it also goes away by itself. With no buttons it gets an OK button.
// The newest are kept (at most 5 at a time). Call from the UI thread.
void noticePush(const std::string& text, std::vector<NoticeButton> buttons = {}, double seconds = 0);
// Draw them; once per frame, after the main windows. bottomInset: the height of the strip at the bottom of the window that they stay above.
void noticesDraw(float bottomInset);
// How many are showing (tests and the status of the stack).
int noticeCount();
