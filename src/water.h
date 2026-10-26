#pragma once

// Water wave simulation at the Xbox rate (see water.cpp). Water_Toggle switches
// between that and the original once-per-frame step (Shift+F10).
void Water_Install(int stepHz);
void Water_Toggle();
