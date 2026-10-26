#pragma once

// Water wave simulation at the Xbox rate (see water.cpp). Water_OnPresent runs
// once per rendered frame; `keys` is true while the game window has the keyboard.
void Water_Install(int stepHz);
void Water_OnPresent(bool keys);
