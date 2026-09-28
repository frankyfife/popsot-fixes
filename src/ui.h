#pragma once

// 2D elements (menus, texts, HUD) in 4:3 proportions and/or scaled around the
// screen centre ([ui] aspect / scale, see ui.cpp).
void Ui_Install(bool aspect, float scale);

// While `on`, quads from line y0 to line y1 (virtual 640x480) are drawn from
// line to0 to line to1 instead (a menu panel stretched over added rows).
void Ui_MoveQuadRows(bool on, float y0, float y1, float to0, float to1);
