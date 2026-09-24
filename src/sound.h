#pragma once

// Routes EAX.DLL's DirectSound through DSOAL when its dsound.dll is in the game
// folder `gameDir` (with trailing backslash); see sound.cpp.
void Sound_Install(const char* gameDir);
// [sound] eax=1: switches 3D audio and EAX on once DSOAL provides them.
void Sound_SetForceEax(bool on);
void Sound_OnFrame();
