#pragma once

// Moves the main-menu camera back by `back` world units (see menucam.cpp).
void MenuCam_Install(float back);
// Handles the tuning keys (F6/F7) while the game window has the focus.
void MenuCam_OnPresent(bool keys);
