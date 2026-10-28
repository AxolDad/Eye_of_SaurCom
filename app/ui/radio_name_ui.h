// Giving a radio your own name: a small window (opened from the pencil by the radio list or its right-click menu) and a one-line field
// for the radio settings. The name is stored by radio key in <data>/radios/names.json (state/radio_names.h).
#pragma once
struct App;

void radioNameRequest(int deviceIdx);   // open the window for this entry of the radio list on the next radioNameModal()
void radioNameModal(App& a);            // draw it; call every frame from the place that owns the radio list
// "Name" field with the shared-serial note, for the top of the radio settings. Commits on Enter or when the field loses focus.
void radioNameField(App& a, bool vertical);
