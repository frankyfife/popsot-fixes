#pragma once

// Moves the main-menu camera forward along its viewing direction by `forward`
// and up by `up` world units (see menucam.cpp).
void MenuCam_Install(float forward, float up);
// Handles the tuning keys (F6/F7, Shift+F6/F7) while the game window has the focus.
void MenuCam_OnPresent(bool keys);
