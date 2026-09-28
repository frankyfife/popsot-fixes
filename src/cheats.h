#pragma once

// Cheats (see cheats.cpp). Cheats_OnPresent runs once per rendered frame;
// `keys` is true while the game window has the keyboard.
void Cheats_Install(const char* iniPath);
void Cheats_OnPresent(bool keys);
