// Giving a radio your own name: a small window (opened from the pencil by the radio list or its right-click menu) and a one-line field
// for the radio settings. The name is stored by radio key in <data>/radios/names.json (state/radio_names.h).
#pragma once
#include <string>
struct App;

void radioNameRequest(int deviceIdx);   // open the window for this entry of the radio list on the next radioNameModal()
void radioNameModal(App& a);            // draw it; call every frame from the place that owns the radio list
// "Name" field with the shared-serial note, for the top of the radio settings. Commits on Enter or when the field loses focus.
void radioNameField(App& a, bool vertical);

// ImGui cuts a widget's visible text at "##" (the rest is its id), and a name the user typed may contain "##" or "###". These widgets take an
// id that does not depend on the text, and draw the text as it is, whole.
bool textSelectable(const char* id, const std::string& text, bool selected);        // a full-width row, as ImGui::Selectable
bool textRadioButton(const char* id, const std::string& text, bool active);         // as ImGui::RadioButton; the text is clickable too
// A combo whose closed box shows text as it is: textBeginCombo instead of BeginCombo (EndCombo as usual), then textComboPreview right after
// its BeginCombo/EndCombo block.
bool textBeginCombo(const char* id);
void textComboPreview(const std::string& text);
