#pragma once
#include <windows.h>

// Native Xbox controller support for all game actions, plus vibration (see gamepad.cpp).
void Gamepad_Install();
void Gamepad_OnFrame(HWND gameWindow);  // once per rendered frame
void Gamepad_Shutdown();                // stop the motors
