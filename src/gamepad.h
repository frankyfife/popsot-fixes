#pragma once
#include <windows.h>
#include <xinput.h>

// Native Xbox controller support for all game actions, plus vibration (see gamepad.cpp).
void Gamepad_Install();
void Gamepad_OnFrame(HWND gameWindow);  // once per rendered frame
void Gamepad_Shutdown();                // stop the motors
bool Gamepad_Read(XINPUT_GAMEPAD* pad);  // current controller state, false if none
void Gamepad_BlockGame(bool block);      // withhold all game input except Start (free camera)
void Gamepad_SetPromptMode(int mode);   // button prompts: 0 auto, 1 controller, 2 keyboard
