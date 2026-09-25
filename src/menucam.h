#pragma once

// Moves the main-menu camera forward along its viewing direction by `forward`,
// up by `up` and to the right by `side` world units (see menucam.cpp).
void MenuCam_Install(float forward, float up, float side);
// Handles the tuning keys (F6/F7 with Shift / Ctrl) while the game window has the focus.
void MenuCam_OnPresent(bool keys);
