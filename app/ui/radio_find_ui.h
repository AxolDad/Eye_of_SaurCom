// "Find this radio" (unplug a radio and plug it back in to see which entry it is).
#pragma once
struct App;
void radioFindModal(App& a);   // draw it; call every frame from the place that owns the radio list (next to radioNameModal)
