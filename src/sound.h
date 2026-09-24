#pragma once

// Routes EAX.DLL's DirectSound through DSOAL when its dsound.dll is in the game
// folder `gameDir` (with trailing backslash); see sound.cpp.
void Sound_Install(const char* gameDir);
