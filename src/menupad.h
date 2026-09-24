#pragma once
#include <windows.h>

// Controller navigation for the mouse-only PC front-end menus (see menupad.cpp).
void MenuPad_SetWindow(HWND hwnd);
void MenuPad_OnPresent();
void MenuPad_Install();  // PC menu patches (level select button), before the menus load
