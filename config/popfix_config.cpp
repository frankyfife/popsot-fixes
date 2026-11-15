// popfix_config - settings for popfix.ini (Prince of Persia: The Sands of Time PC fixes)
//
// A plain Win32 program (static CRT, only Windows' own DLLs) with a dark look of its own. It edits the
// popfix.ini next to it, or one picked with the file dialog. Values are changed in place, so comments, order
// and unknown keys stay as they are; a key that is missing is added to its section. Every change is saved at
// once and applies at the next start of the game. dx.dll writes to the same file itself (cheats switched in
// the game, the menu camera placed with the free camera), so the file is read again before every change and
// whenever the window is activated. Before the first change the INI as it was opened is kept as
// popfix.ini.bak.
#define WIN32_LEAN_AND_MEAN
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static const char kTitle[] = "Sands of Time PC fixes";

// ---------------------------------------------------------------- options

enum Kind { BOOL_OPT, CHOICE, RANGE };

struct Option {
    const char* section;
    const char* key;
    Kind kind;
    const char* text;
    const char* help;  // starting with '!': a warning, shown in red
    const char* def;
    // CHOICE: "value|Text;value|Text;..."   RANGE: min, max, step, decimals
    const char* choices;
    double lo, hi, step;
    int decimals;
    HWND label, ctrl, value;  // label, control, value label (RANGE)
};

static Option kOptions[] = {
    // Graphics
    { "water", "refraction", RANGE, "Water refraction", "Strength of the light refraction through the water, relative to the Xbox version (1.0x).", "1.5", 0, 0.5, 3, 0.1, 1 },
    { "water", "step_rate", CHOICE, "Wave simulation", "The Xbox steps its water 30 times a second. Once per frame (the original PC behaviour) damps the waves the more, the higher the frame rate: the surface goes flat. Shift+F10 in the game compares.", "30", "30|30 Hz, like the Xbox;60|60 Hz;0|Every frame (original PC)" },
    { "post", "blur", BOOL_OPT, "Soft focus", "The game's soft-focus blur over the picture. Ctrl+F10 in the game switches it.", "1" },
    { "post", "blur_resolution", CHOICE, "  Blur resolution", "Size of the blur targets. The original 512x512 is pixelated at modern resolutions; larger sizes are limited by the screen height.", "4", "4|Up to 4x;3|Up to 3x;2|Up to 2x;1|Original 512x512" },
    { "post", "blur_radius", CHOICE, "  Blur width", "Original: a wide halo around everything, which reads as ghosting. Sharp: as an upscaling emulator renders it.", "auto", "auto|Sharp, like an emulator;1|Original (wide halo)" },
    { "post", "blur_after_glow", BOOL_OPT, "  Blur includes the glow", "The original copies the frame before the glow, which leaves a grey ghost inside every glowing area. Needs a blur resolution of 2x or more.", "1" },
    { "video", "hdr", BOOL_OPT, "HDR correction", "Only with Windows' HDR mode on (\"Use HDR\"): the game's SDR picture gets its own white level and the dark tones of a gamma 2.2 display, instead of Windows' sRGB curve at the SDR content brightness, which makes light sources glare.", "1" },
    { "video", "hdr_white", RANGE, "  HDR white", "Brightness of the game's white in Windows' HDR mode, in nits. It cannot go above Windows' SDR content brightness (Settings, Display, HDR).", "200", 0, 80, 480, 10, 0 },
    { "video", "keep_aspect", BOOL_OPT, "Videos in their aspect ratio", "Black bars where needed. Off = stretched over the whole screen like the original.", "1" },
    // Interface
    { "ui", "aspect", BOOL_OPT, "Menus and HUD in 4:3", "Menus, texts and HUD keep their 4:3 proportions, centred on wide screens of any aspect ratio. Off = stretched like the original.", "1" },
    { "ui", "scale", RANGE, "Menu and HUD size", "Size of menus, texts and HUD around the screen centre.", "1", 0, 0.5, 1.5, 0.05, 2 },
    { "ui", "dpi_aware", BOOL_OPT, "Ignore Windows display scaling", "With display scaling above 100 % (125 %, 150 %...) Windows would enlarge the game's window past the screen edges: the HUD is cut off and the menus are off centre.", "1" },
    { "menus", "console", BOOL_OPT, "Xbox menus (experimental)", "!The Xbox console menus instead of the PC menus. Saving and loading from these pages is incomplete on PC.", "0" },
    { "menus", "camera_start", CHOICE, "Main menu picture", "Placed in the game with the free camera (Back or F9, then X or F6). Default = the game's view, moved by the offsets below.", "", "|Default" },
    { "menus", "camera_end", CHOICE, "End of the new-game flight", "Placed in the game with the free camera (Back or F9, then B or F7). Default = the game's own end of the flight.", "", "|Default" },
    { "menus", "camera_forward", RANGE, "  Menu camera forward", "Moves the main menu camera forward so wide screens do not show the edges of the set. 0 = the original view. No effect while a menu picture is placed.", "3.5", 0, 0, 10, 0.25, 2 },
    { "menus", "camera_up", RANGE, "  Menu camera up", "Moves the main menu camera up. 0 = the original view.", "6.25", 0, 0, 12, 0.25, 2 },
    { "menus", "camera_side", RANGE, "  Menu camera sideways", "Moves the main menu camera to the left (below 0) or right. 0 = the original view.", "0", 0, -5, 5, 0.25, 2 },
    // Controller
    { "controller", "layout", CHOICE, "Shoulder buttons and triggers", "PS2: LB rewind, RB special action, LT alternate view, RT look. Xbox: the other way round, LT rewind, RT special action.", "ps2", "ps2|PS2 (LB rewind, RB action);xbox|Xbox (LT rewind, RT action)" },
    { "controller", "prompts", CHOICE, "Button names in hints", "Which buttons the tutorial and menu hints name.", "auto", "auto|Controller while connected;controller|Always the controller;keyboard|Always the keyboard" },
    { "controller", "invert_x", BOOL_OPT, "Invert camera left / right", "Turns the horizontal camera and look direction round (stick, mouse and keys), on top of the game's own option.", "0" },
    { "controller", "invert_y", BOOL_OPT, "Invert camera up / down", "Turns the vertical camera and look direction round (stick, mouse and keys). The game itself has no option for it.", "0" },
    // Textures
    { "textures", "pack", CHOICE, "HD texture pack", "Evgeshajk's HD texture pack (PoP Texture Studio): its d3d9.dll renamed to poptex_d3d9.dll, and Evgesha.JK, in the game folder.", "poptex_d3d9.dll", "poptex_d3d9.dll|Load poptex_d3d9.dll;|Off" },
    { "textures", "only", CHOICE, "  Textures used", "HUD only: health bar, its frame, sand tanks and ring. The rest of the pack does not fit the game's look everywhere.", "hud", "hud|HUD only;all|The whole pack" },
    { "textures", "brightness", RANGE, "  Brightness", "The pack's textures are darker than the originals (about 0.8); 1.0x = as in the pack.", "1.25", 0, 0.5, 2, 0.05, 2 },
    { "textures", "bloom", BOOL_OPT, "  Keep the game's bloom", "The HD pack would remove the glow around bright light.", "1" },
    { "textures", "skip", CHOICE, "  Font", "The pack's font has half the original resolution and blurs every text.", "0B0041BB", "0B0041BB|The game's own;|The pack's" },
    // Cheats
    { "cheats", "invulnerable", BOOL_OPT, "Invulnerable", "The prince's life stays at its maximum. Also in the game options and with Ctrl+F1.", "0" },
    { "cheats", "infinite_sand", BOOL_OPT, "Infinite sand", "The sand tanks stay full. Also in the game options and with Ctrl+F2.", "0" },
    { "cheats", "one_hit_kills", BOOL_OPT, "One-hit kills", "Every hit on an enemy kills it. Also in the game options and with Ctrl+F3.", "0" },
    // Other
    { "sound", "eax", BOOL_OPT, "EAX and 3D sound", "Switched on automatically when DSOAL (dsound.dll, dsoal-aldrv.dll) is in the game folder.", "1" },
    { "debug", "verbose", BOOL_OPT, "Detailed log", "More diagnostics in popfix.log, for bug reports.", "0" },
};
static const int kOptionCount = sizeof(kOptions) / sizeof(kOptions[0]);
static const char* const kPages[] = { "Graphics", "Interface", "Controller", "Textures", "Cheats", "Other" };
static const int kPageCount = sizeof(kPages) / sizeof(kPages[0]);
static const char* const kPageText[] = {
    "Water, soft focus, HDR and videos.",
    "Menus, HUD and the main menu camera.",
    "Button layout, hints and camera direction.",
    "Evgeshajk's HD texture pack.",
    "Also in the game's options (Options, Game Options).",
    "Sound and diagnostics.",
};
static_assert(sizeof(kPageText) / sizeof(kPageText[0]) == sizeof(kPages) / sizeof(kPages[0]), "a text per page");

static int PageOf(const Option& o)
{
    static const char* const sections[][3] = {
        { "water", "post", "video" }, { "ui", "menus", "" }, { "controller", "", "" },
        { "textures", "", "" }, { "cheats", "", "" }, { "sound", "debug", "" },
    };
    for (int p = 0; p < kPageCount; p++)
        for (const char* s : sections[p])
            if (*s && !_stricmp(s, o.section)) return p;
    return kPageCount - 1;
}

// ---------------------------------------------------------------- INI file, edited in place

static char s_path[MAX_PATH];
static std::vector<std::string> s_lines;
static bool s_crlf = true;

static std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static bool LoadIni()
{
    s_lines.clear();
    FILE* f = fopen(s_path, "rb");
    if (!f) return false;
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
    fclose(f);
    if (all.size() >= 3 && !memcmp(all.data(), "\xEF\xBB\xBF", 3)) all.erase(0, 3);  // UTF-8 BOM
    s_crlf = all.find("\r\n") != std::string::npos || all.find('\n') == std::string::npos;
    size_t start = 0;
    while (start <= all.size()) {
        size_t end = all.find('\n', start);
        std::string line = all.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (end == std::string::npos) {
            if (!line.empty()) s_lines.push_back(line);
            break;
        }
        s_lines.push_back(line);
        start = end + 1;
    }
    return true;
}

// popfix.ini.bak: the INI as it was when the program opened it, written before the first change
static std::vector<std::string> s_opened;
static bool s_backedUp;

static std::string BackupPath() { return std::string(s_path) + ".bak"; }

static bool SaveLines(const char* path, const std::vector<std::string>& lines)
{
    // written to a temporary file first and then put in place, so the game never reads a half written INI
    std::string tmp = std::string(path) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = true;
    for (const std::string& l : lines) {
        ok = fputs(l.c_str(), f) >= 0 && ok;
        ok = fputs(s_crlf ? "\r\n" : "\n", f) >= 0 && ok;
    }
    ok = fclose(f) == 0 && ok;
    if (ok && MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileA(tmp.c_str());
    return false;
}

static bool SaveIni()
{
    if (!s_backedUp && !s_opened.empty()) s_backedUp = SaveLines(BackupPath().c_str(), s_opened);
    return SaveLines(s_path, s_lines);
}

// line index of key in section, -1 if missing; *sectionEnd gets the line after the section's last key
static int FindKey(const char* section, const char* key, int* sectionEnd)
{
    std::string current;
    int found = -1, end = -1;
    for (int i = 0; i < (int)s_lines.size(); i++) {
        std::string t = Trim(s_lines[i]);
        if (t.size() > 2 && t[0] == '[' && t.back() == ']') {
            current = t.substr(1, t.size() - 2);
            continue;
        }
        if (_stricmp(current.c_str(), section)) continue;
        if (t.empty() || t[0] == ';') continue;
        end = i + 1;
        size_t eq = t.find('=');
        // the first one, as Windows reads it
        if (found < 0 && eq != std::string::npos && !_stricmp(Trim(t.substr(0, eq)).c_str(), key)) found = i;
    }
    if (sectionEnd) *sectionEnd = end;
    return found;
}

static std::string ValueAt(int line)
{
    const std::string& l = s_lines[line];
    std::string v = Trim(l.substr(l.find('=') + 1));
    if (v.size() >= 2 && v[0] == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);  // as Windows reads it
    return v;
}

// as dx.dll reads it: a missing key means the default; an empty value too, except where empty is a choice
// of its own (no texture pack, no menu camera pose, no skipped texture)
static std::string GetValue(const Option& o)
{
    int i = FindKey(o.section, o.key, NULL);
    if (i < 0) return o.def;
    std::string v = ValueAt(i);
    if (v.empty() && o.kind == RANGE) return o.def;
    return v;
}

static void SetValue(const Option& o, const std::string& v)
{
    int end;
    int i = FindKey(o.section, o.key, &end);
    std::string line = std::string(o.key) + "=" + v;
    if (i >= 0) {
        s_lines[i] = line;
        return;
    }
    if (end < 0) {  // no such section yet
        if (!s_lines.empty() && !Trim(s_lines.back()).empty()) s_lines.push_back("");
        s_lines.push_back(std::string("[") + o.section + "]");
        s_lines.push_back(line);
        return;
    }
    s_lines.insert(s_lines.begin() + end, line);
}

// ---------------------------------------------------------------- look

// Dark, warm colours of sand and old gold; the controls are drawn here, the lists open as popup menus in
// Windows' dark menu style.
static const COLORREF kBack = RGB(23, 19, 16), kSide = RGB(33, 27, 22), kSideSel = RGB(51, 42, 33),
                      kField = RGB(54, 45, 36), kBorder = RGB(88, 73, 56), kText = RGB(242, 231, 211),
                      kDim = RGB(172, 156, 132), kOff = RGB(104, 92, 76), kAccent = RGB(224, 172, 86),
                      kAccentDim = RGB(120, 94, 56), kWarn = RGB(236, 104, 78);
static HBRUSH s_backBrush, s_sideBrush;
static HFONT s_font, s_fontBold, s_fontTitle, s_fontBrand, s_fontSmall;

enum Role { R_NONE, R_LABEL, R_VALUE, R_DIM, R_TITLE, R_BRAND, R_SIDEDIM, R_NAV, R_PRESET, R_ACTION, R_TOGGLE, R_SELECT };

static bool s_infoWarn;  // the help text shown is a warning
static HWND s_wnd, s_status, s_info, s_title, s_subtitle, s_nav[kPageCount];
static int s_dpi = 96;
static int s_page;
static bool s_loading;
static bool s_toggle[kOptionCount];        // BOOL_OPT state by option index
static int s_choice[kOptionCount];         // CHOICE: selected item by option index, -1 = a value set by hand
static std::string s_custom[kOptionCount];  // the text shown for that value
static bool s_dim[kOptionCount];  // greyed out: only the control is disabled; a disabled static control would be
                                  // drawn embossed by Windows, ignoring the text colour, so its label is just dimmed
static const char kSliderClass[] = "popfix_slider";  // the own slider control, see SliderProc

static int S(int px) { return MulDiv(px, s_dpi, 96); }

static HFONT MakeFont(int pt, int weight, const wchar_t* face)
{
    return CreateFontW(-MulDiv(pt, s_dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

static void MakeFonts()
{
    HFONT old[] = { s_font, s_fontBold, s_fontSmall, s_fontTitle, s_fontBrand };
    s_font = MakeFont(10, FW_NORMAL, L"Segoe UI");
    s_fontBold = MakeFont(10, FW_SEMIBOLD, L"Segoe UI");
    s_fontSmall = MakeFont(9, FW_NORMAL, L"Segoe UI");
    s_fontTitle = MakeFont(19, FW_NORMAL, L"Palatino Linotype");  // a classic serif, part of Windows
    s_fontBrand = MakeFont(17, FW_BOLD, L"Palatino Linotype");
    for (HFONT f : old)
        if (f) DeleteObject(f);
}

// Per-monitor DPI (v2): the layout and fonts follow the monitor the window is on. Older Windows versions
// fall back to the system DPI.
static void DpiAware()
{
    HMODULE user = GetModuleHandleA("user32.dll");
    typedef BOOL(WINAPI * SetContext_t)(HANDLE);
    SetContext_t set = (SetContext_t)GetProcAddress(user, "SetProcessDpiAwarenessContext");
    if (!set || !set((HANDLE)-4)) SetProcessDPIAware();  // -4 = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
}

static int WindowDpi(HWND wnd)
{
    typedef UINT(WINAPI * GetDpi_t)(HWND);
    GetDpi_t get = (GetDpi_t)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow");
    if (get && wnd) return (int)get(wnd);
    HDC dc = GetDC(NULL);
    int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    return dpi;
}

// Layout, in 96 dpi pixels. The height follows from the longest page and from the sidebar, so nothing is
// cut off when options are added.
static const int kWidth = 900, kSideW = 214, kContentX = 244, kLabelW = 300, kRowH = 34, kRowTop = 104,
                 kNavTop = 104, kNavH = 42, kInfoH = 150;
static const int kPresetCount = 2;

static int MaxRows()
{
    int rows[kPageCount] = {}, most = 0;
    for (const Option& o : kOptions) {
        int n = ++rows[PageOf(o)];
        if (n > most) most = n;
    }
    return most;
}

static int PresetTop() { return kNavTop + kPageCount * kNavH + 28; }

static int Height()
{
    int content = kRowTop + MaxRows() * kRowH + 16 + kInfoH;
    int side = PresetTop() + 24 + kPresetCount * 40 + 24;
    return content > side ? content : side;
}

// the outer window size for the client area at the current DPI
static SIZE WindowSize(DWORD style)
{
    RECT r = { 0, 0, S(kWidth), S(Height()) };
    typedef BOOL(WINAPI * Adjust_t)(RECT*, DWORD, BOOL, DWORD, UINT);
    Adjust_t adjust = (Adjust_t)GetProcAddress(GetModuleHandleA("user32.dll"), "AdjustWindowRectExForDpi");
    if (adjust) adjust(&r, style, FALSE, 0, (UINT)s_dpi);
    else AdjustWindowRect(&r, style, FALSE);
    SIZE size = { r.right - r.left, r.bottom - r.top };
    return size;
}

static BOOL CALLBACK DestroyChild(HWND child, LPARAM)
{
    DestroyWindow(child);
    return TRUE;
}

static void RoundRect(HDC dc, RECT r, int radius, COLORREF fill, COLORREF line)
{
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, line);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    ::RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

// Windows' dark style for the popup menus (uxtheme ordinals, Windows 10 1903 and later)
static void EnableDarkMenus()
{
    HMODULE ux = LoadLibraryA("uxtheme.dll");
    if (!ux) return;
    typedef int(WINAPI * SetPreferredAppMode_t)(int);
    typedef void(WINAPI * FlushMenuThemes_t)();
    SetPreferredAppMode_t setMode = (SetPreferredAppMode_t)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    FlushMenuThemes_t flush = (FlushMenuThemes_t)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(2);  // force dark
    if (flush) flush();
}

static void DarkTitleBar(HWND wnd)
{
    HMODULE dwm = LoadLibraryA("dwmapi.dll");
    if (!dwm) return;
    typedef HRESULT(WINAPI * DwmSet_t)(HWND, DWORD, LPCVOID, DWORD);
    DwmSet_t set = (DwmSet_t)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (!set) return;
    BOOL on = TRUE;
    if (FAILED(set(wnd, 20, &on, sizeof(on)))) set(wnd, 19, &on, sizeof(on));  // immersive dark mode
    COLORREF caption = kSide;
    set(wnd, 35, &caption, sizeof(caption));  // caption colour, Windows 11
}

// ---------------------------------------------------------------- options on screen

static Option* Find(const char* key)
{
    for (Option& o : kOptions)
        if (!strcmp(o.key, key)) return &o;
    return NULL;
}

static int IndexOf(const Option& o) { return (int)(&o - kOptions); }

static std::vector<std::pair<std::string, std::string>> Choices(const Option& o)
{
    std::vector<std::pair<std::string, std::string>> out;
    std::string s = o.choices;
    size_t p = 0;
    while (p < s.size()) {
        size_t semi = s.find(';', p);
        std::string item = s.substr(p, semi == std::string::npos ? std::string::npos : semi - p);
        size_t bar = item.find('|');
        out.push_back({ item.substr(0, bar), item.substr(bar + 1) });
        if (semi == std::string::npos) break;
        p = semi + 1;
    }
    return out;
}

static std::string FormatRange(const Option& o, double v)
{
    char buf[32];
    sprintf(buf, "%.*f", o.decimals, v);
    if (!strcmp(buf, "-0.00") || !strcmp(buf, "-0.0") || !strcmp(buf, "-0")) strcpy(buf, buf + 1);
    return buf;
}

static int RangeSteps(const Option& o) { return (int)((o.hi - o.lo) / o.step + 0.5); }

static void ShowValue(Option& o, double v)
{
    std::string text = FormatRange(o, v);
    if (!strcmp(o.key, "scale")) text = std::to_string((int)(v * 100 + 0.5)) + " %";
    else if (!strcmp(o.key, "hdr_white")) text += " nits";
    else if (!strcmp(o.key, "refraction") || !strcmp(o.key, "brightness")) text += "x";
    else if (!strncmp(o.key, "camera_", 7)) text += v == 0 ? "  (original)" : "";
    SetWindowTextA(o.value, text.c_str());
}

static bool IsOn(const char* key)
{
    Option* o = Find(key);
    return o && s_toggle[IndexOf(*o)];
}

static std::string Current(const char* key)
{
    Option* o = Find(key);
    return o ? GetValue(*o) : "";
}

// options that only matter with another one are greyed out
static void UpdateEnables()
{
    bool blur = IsOn("blur"), pack = !Current("pack").empty(), pose = !Current("camera_start").empty();
    bool big = atoi(Current("blur_resolution").c_str()) >= 2;
    struct { const char* key; bool on; } rules[] = {
        { "blur_resolution", blur }, { "blur_radius", blur }, { "blur_after_glow", blur && big },
        { "hdr_white", IsOn("hdr") }, { "only", pack }, { "brightness", pack }, { "bloom", pack }, { "skip", pack },
        { "camera_forward", !pose }, { "camera_up", !pose }, { "camera_side", !pose },
    };
    for (auto& r : rules) {
        Option* o = Find(r.key);
        if (!o || !o->ctrl) continue;
        EnableWindow(o->ctrl, r.on);
        s_dim[IndexOf(*o)] = !r.on;
        InvalidateRect(o->label, NULL, TRUE);
        if (o->value) InvalidateRect(o->value, NULL, TRUE);
    }
}

static void Status(const char* text) { SetWindowTextA(s_status, text); }

// equal as text, or as numbers when both are numbers ("30" and "30.0")
static bool SameValue(const std::string& a, const std::string& b)
{
    if (!_stricmp(a.c_str(), b.c_str())) return true;
    char* end1;
    char* end2;
    double x = strtod(a.c_str(), &end1), y = strtod(b.c_str(), &end2);
    return !*end1 && !*end2 && end1 != a.c_str() && end2 != b.c_str() && fabs(x - y) < 1e-6;
}

static void LoadControls()
{
    s_loading = true;
    for (Option& o : kOptions) {
        std::string v = GetValue(o);
        int index = IndexOf(o);
        if (o.kind == BOOL_OPT) {
            s_toggle[index] = atoi(v.c_str()) != 0;
            InvalidateRect(o.ctrl, NULL, TRUE);
        } else if (o.kind == CHOICE) {
            auto items = Choices(o);
            int sel = -1;
            for (int i = 0; i < (int)items.size() && sel < 0; i++)
                if (SameValue(items[i].first, v)) sel = i;
            s_choice[index] = sel;
            if (!strncmp(o.key, "camera_", 7)) s_custom[index] = "Placed with the free camera";
            else s_custom[index] = v + " (set by hand)";
            InvalidateRect(o.ctrl, NULL, TRUE);
        } else {
            double d = atof(v.c_str());
            if (d < o.lo) d = o.lo;
            if (d > o.hi) d = o.hi;
            SendMessageA(o.ctrl, TBM_SETPOS, TRUE, (LPARAM)(int)((d - o.lo) / o.step + 0.5));
            InvalidateRect(o.ctrl, NULL, TRUE);
            ShowValue(o, atof(v.c_str()));
        }
    }
    s_loading = false;
    UpdateEnables();
}

struct Preset {
    const char* name;
    const char* help;
    const char* values;  // "key=value;..."; every other option gets its default
};
static const Preset kPresets[kPresetCount] = {
    { "Recommended", "Every option at its default: the look of the Xbox version, sharper.", "" },
    { "Close to the original", "The PC version's own look and layout where an option allows it (F10 in the game compares the rest).",
      "refraction=1.0;step_rate=0;blur_resolution=1;blur_radius=1;blur_after_glow=0;keep_aspect=0;aspect=0;"
      "camera_forward=0;camera_up=0;camera_side=0;pack=;hdr=0" },
};
static HWND s_preset[kPresetCount];

// the options a preset sets: all of them except the cheats, the menu camera poses and the log
static bool InPreset(const Option& o)
{
    return _stricmp(o.section, "cheats") && _stricmp(o.section, "debug") && strcmp(o.key, "camera_start") &&
           strcmp(o.key, "camera_end");
}

// the value a preset gives an option: its own, else the option's default
static std::string PresetValue(const Preset& p, const Option& o)
{
    std::string list = std::string(";") + p.values + ";", pat = std::string(";") + o.key + "=";
    size_t at = list.find(pat);
    if (at == std::string::npos) return o.def;
    size_t s = at + pat.size();
    return list.substr(s, list.find(';', s) - s);
}

static bool PresetMatches(const Preset& p)
{
    for (const Option& o : kOptions)
        if (InPreset(o) && !SameValue(PresetValue(p, o), GetValue(o))) return false;
    return true;
}

static void ShowPage(int page)
{
    s_page = page;
    SetWindowTextA(s_title, kPages[page]);
    SetWindowTextA(s_subtitle, kPageText[page]);
    for (Option& o : kOptions) {
        int cmd = PageOf(o) == page ? SW_SHOW : SW_HIDE;
        ShowWindow(o.label, cmd);
        ShowWindow(o.ctrl, cmd);
        if (o.value) ShowWindow(o.value, cmd);
    }
    s_infoWarn = false;
    SetWindowTextA(s_info, "Point at an option to see what it does. Every change is saved at once and applies at the "
                           "next start of the game.");
    InvalidateRect(s_info, NULL, TRUE);
    for (int i = 0; i < kPageCount; i++) InvalidateRect(s_nav[i], NULL, TRUE);
}

// The file on disk is read again first: dx.dll may have written to it in the meantime.
static void Changed(Option& o)
{
    if (s_loading) return;
    std::string v;
    int index = IndexOf(o);
    if (o.kind == BOOL_OPT) {
        v = s_toggle[index] ? "1" : "0";
    } else if (o.kind == CHOICE) {
        int sel = s_choice[index];
        auto items = Choices(o);
        if (sel < 0 || sel >= (int)items.size()) return;  // a value set by hand stays as it is
        v = items[sel].first;
    } else {
        double d = o.lo + SendMessageA(o.ctrl, TBM_GETPOS, 0, 0) * o.step;
        ShowValue(o, d);
        v = FormatRange(o, d);
    }
    LoadIni();
    SetValue(o, v);
    char msg[300];
    if (SaveIni()) sprintf(msg, "Saved  [%s] %s = %s", o.section, o.key, v.c_str());
    else sprintf(msg, "Could not write %s", s_path);
    Status(msg);
    UpdateEnables();
    for (HWND b : s_preset) InvalidateRect(b, NULL, TRUE);
}

static void ApplyPreset(const Preset& p)
{
    LoadIni();
    for (Option& o : kOptions)
        if (InPreset(o)) SetValue(o, PresetValue(p, o));
    char msg[160];
    sprintf(msg, SaveIni() ? "Preset \"%s\" saved" : "Could not write the INI", p.name);
    LoadControls();
    for (HWND b : s_preset) InvalidateRect(b, NULL, TRUE);
    Status(msg);
}

static HWND Child(const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int id, Role role,
                  HFONT font)
{
    HWND h_ = CreateWindowExA(0, cls, text, WS_CHILD | style, x, y, w, h, s_wnd, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageA(h_, WM_SETFONT, (WPARAM)font, TRUE);
    SetWindowLongPtrA(h_, GWLP_USERDATA, role);
    return h_;
}

enum { ID_NAV0 = 10, ID_PRESET0 = 30, ID_OPEN = 40, ID_CLOSE = 41, ID_RESTORE = 42, ID_OPTION0 = 100 };

static void CreateControls()
{
    const int ctrlX = kContentX + kLabelW, height = Height();
    Child("STATIC", "Sands of Time", WS_VISIBLE | SS_LEFT, S(22), S(24), S(190), S(32), 0, R_BRAND, s_fontBrand);
    Child("STATIC", "PC fixes  \xB7  settings", WS_VISIBLE | SS_LEFT, S(23), S(58), S(190), S(20), 0, R_SIDEDIM, s_fontSmall);
    for (int i = 0; i < kPageCount; i++)
        s_nav[i] = Child("BUTTON", kPages[i], WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, S(kNavTop) + i * S(kNavH), S(kSideW),
                         S(kNavH - 2), ID_NAV0 + i, R_NAV, s_font);
    Child("STATIC", "PRESETS", WS_VISIBLE | SS_LEFT, S(22), S(PresetTop()), S(160), S(18), 0, R_SIDEDIM, s_fontSmall);
    for (int i = 0; i < kPresetCount; i++)
        s_preset[i] = Child("BUTTON", kPresets[i].name, WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, S(18),
                            S(PresetTop() + 24) + i * S(40), S(kSideW - 36), S(32), ID_PRESET0 + i, R_PRESET, s_font);

    s_title = Child("STATIC", "", WS_VISIBLE | SS_LEFT, S(kContentX), S(26), S(600), S(40), 0, R_TITLE, s_fontTitle);
    s_subtitle = Child("STATIC", "", WS_VISIBLE | SS_LEFT, S(kContentX), S(68), S(620), S(22), 0, R_DIM, s_font);

    int row[kPageCount] = {};
    for (int i = 0; i < kOptionCount; i++) {
        Option& o = kOptions[i];
        int y = S(kRowTop) + row[PageOf(o)]++ * S(kRowH);
        int id = ID_OPTION0 + i;
        const char* text = o.text;
        int indent = 0;
        while (*text == ' ') text++, indent += S(9);
        o.label = Child("STATIC", text, SS_LEFT | SS_CENTERIMAGE | SS_NOTIFY | SS_ENDELLIPSIS, S(kContentX) + indent, y,
                        S(kLabelW) - indent - S(8), S(28), 0, R_LABEL, s_font);
        if (o.kind == BOOL_OPT) {
            o.ctrl = Child("BUTTON", "", WS_TABSTOP | BS_OWNERDRAW, S(ctrlX), y + S(3), S(46), S(22), id, R_TOGGLE, s_font);
        } else if (o.kind == CHOICE) {
            o.ctrl = Child("BUTTON", "", WS_TABSTOP | BS_OWNERDRAW, S(ctrlX), y, S(300), S(28), id, R_SELECT, s_font);
        } else {
            o.ctrl = Child(kSliderClass, "", WS_TABSTOP, S(ctrlX) - S(6), y + S(1), S(210), S(26), id, R_NONE, s_font);
            SendMessageA(o.ctrl, TBM_SETRANGE, TRUE, MAKELPARAM(0, RangeSteps(o)));
            o.value = Child("STATIC", "", SS_LEFT | SS_CENTERIMAGE, S(ctrlX) + S(212), y, S(kWidth - ctrlX - 236), S(28), 0,
                            R_VALUE, s_font);
        }
    }
    s_info = Child("STATIC", "", WS_VISIBLE | SS_LEFT, S(kContentX), S(height - kInfoH + 22), S(kWidth - kContentX - 24),
                   S(66), 0, R_DIM, s_font);
    s_status = Child("STATIC", "", WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS | SS_CENTERIMAGE, S(kContentX), S(height - 46),
                     S(kWidth - 404 - kContentX), S(30), 0, R_DIM, s_fontSmall);
    Child("BUTTON", "Restore backup", WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, S(kWidth - 390), S(height - 48), S(140),
          S(32), ID_RESTORE, R_ACTION, s_font);
    Child("BUTTON", "Open INI...", WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, S(kWidth - 240), S(height - 48), S(106), S(32),
          ID_OPEN, R_ACTION, s_font);
    Child("BUTTON", "Close", WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, S(kWidth - 124), S(height - 48), S(106), S(32),
          ID_CLOSE, R_ACTION, s_font);
}

static bool PickIni(HWND owner)
{
    char file[MAX_PATH];
    strcpy(file, s_path);
    OPENFILENAMEA ofn = { sizeof(ofn) };
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = "popfix.ini\0popfix.ini\0INI files\0*.ini\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "popfix.ini in the Prince of Persia - The Sands of Time folder";
    ofn.Flags = OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameA(&ofn)) return false;
    strcpy(s_path, file);
    return true;
}

static void Open()
{
    s_backedUp = false;
    s_opened.clear();
    if (LoadIni()) s_opened = s_lines;
    if (s_opened.empty()) {
        s_lines.clear();
        Status("No popfix.ini here yet: it is created with the first change.");
    } else {
        std::string msg = std::string("Editing ") + s_path;
        Status(msg.c_str());
    }
    LoadControls();
    for (HWND b : s_preset) InvalidateRect(b, NULL, TRUE);
}

// puts the backup back and reloads it
static void RestoreBackup(HWND wnd)
{
    std::string bak = BackupPath();
    if (GetFileAttributesA(bak.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(wnd, "There is no backup yet. It is made before the first change.", kTitle, MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::string q = "Replace the settings with the backup?\n\n" + bak;
    if (MessageBoxA(wnd, q.c_str(), kTitle, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    if (!CopyFileA(bak.c_str(), s_path, FALSE)) {
        Status("Could not restore the backup");
        return;
    }
    Open();
    s_backedUp = true;  // keep the backup as it is
    Status("Backup restored");
}

// ---------------------------------------------------------------- drawing

static void DrawButton(const DRAWITEMSTRUCT* di)
{
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    Role role = (Role)GetWindowLongPtrA(di->hwndItem, GWLP_USERDATA);
    bool pressed = (di->itemState & ODS_SELECTED) != 0, disabled = (di->itemState & ODS_DISABLED) != 0;
    bool focus = (di->itemState & ODS_FOCUS) != 0 && !(di->itemState & ODS_NOFOCUSRECT);
    char text[64];
    GetWindowTextA(di->hwndItem, text, sizeof(text));
    SetBkMode(dc, TRANSPARENT);

    if (role == R_TOGGLE) {
        int index = GetDlgCtrlID(di->hwndItem) - ID_OPTION0;
        bool on = s_toggle[index];
        FillRect(dc, &r, s_backBrush);
        RECT track = { r.left + S(1), r.top + S(1), r.left + S(43), r.bottom - S(1) };
        COLORREF fill = on ? (disabled ? kAccentDim : kAccent) : kField;
        RoundRect(dc, track, S(20), fill, focus ? kText : (on ? fill : kBorder));
        int d = track.bottom - track.top - S(6);
        int x = on ? track.right - S(3) - d : track.left + S(3);
        RECT knob = { x, track.top + S(3), x + d, track.top + S(3) + d };
        COLORREF k = disabled ? kOff : (on ? kBack : kDim);
        RoundRect(dc, knob, d, k, k);
        return;
    }
    if (role == R_NAV) {
        int page = GetDlgCtrlID(di->hwndItem) - ID_NAV0;
        bool sel = page == s_page;
        HBRUSH b = CreateSolidBrush(sel ? kSideSel : kSide);
        FillRect(dc, &r, b);
        DeleteObject(b);
        if (sel) {
            RECT bar = { r.left, r.top + S(8), r.left + S(4), r.bottom - S(8) };
            HBRUSH a = CreateSolidBrush(kAccent);
            FillRect(dc, &bar, a);
            DeleteObject(a);
        }
        SelectObject(dc, sel ? s_fontBold : s_font);
        SetTextColor(dc, sel ? kText : kDim);
        RECT t = r;
        t.left += S(24);
        DrawTextA(dc, text, -1, &t, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        if (focus) {
            RECT f = { r.left + S(10), r.top + S(4), r.right - S(10), r.bottom - S(4) };
            RoundRect(dc, f, S(8), sel ? kSideSel : kSide, kBorder);
            DrawTextA(dc, text, -1, &t, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        }
        return;
    }
    if (role == R_SELECT) {  // a list: the current item and a chevron
        int index = GetDlgCtrlID(di->hwndItem) - ID_OPTION0;
        Option& o = kOptions[index];
        auto items = Choices(o);
        int sel = s_choice[index];
        std::string value = sel >= 0 && sel < (int)items.size() ? items[sel].second : s_custom[index];
        FillRect(dc, &r, s_backBrush);
        RoundRect(dc, r, S(8), pressed ? kSideSel : kField, focus ? kText : (disabled ? kField : kBorder));
        RECT t = r;
        t.left += S(12);
        t.right -= S(30);
        SelectObject(dc, s_font);
        SetTextColor(dc, disabled ? kOff : kText);
        DrawTextA(dc, value.c_str(), -1, &t, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
        HPEN pen = CreatePen(PS_SOLID, S(2), disabled ? kOff : kAccent);
        HGDIOBJ op = SelectObject(dc, pen);
        int cx = r.right - S(17), cy = (r.top + r.bottom) / 2;
        MoveToEx(dc, cx - S(5), cy - S(2), NULL);
        LineTo(dc, cx, cy + S(3));
        LineTo(dc, cx + S(5), cy - S(2));
        SelectObject(dc, op);
        DeleteObject(pen);
        return;
    }
    // presets and actions: outlined pills, filled while pressed; the preset that matches is outlined in gold
    HBRUSH b = CreateSolidBrush(role == R_PRESET ? kSide : kBack);
    FillRect(dc, &r, b);
    DeleteObject(b);
    bool current = role == R_PRESET && PresetMatches(kPresets[GetDlgCtrlID(di->hwndItem) - ID_PRESET0]);
    COLORREF line = focus ? kText : (current ? kAccent : kBorder);
    RoundRect(dc, r, S(10), pressed ? kSideSel : (role == R_PRESET ? kSide : kField), line);
    SelectObject(dc, s_font);
    SetTextColor(dc, current ? kAccent : kText);
    DrawTextA(dc, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
}

// The slider: an own control instead of the common trackbar, which only repaints the area of its own
// (narrow) thumb and leaves parts of a round one behind. It understands the trackbar messages used here
// (TBM_SETRANGE, TBM_SETPOS, TBM_GETPOS), reports changes with WM_HSCROLL like a trackbar and draws the
// whole control, double buffered, on every change. Mouse: click or drag; keys: arrows, Page Up/Down,
// Home, End. While the mouse drags, only the shown value follows; the INI is written when it is released,
// so a drag is one change, not dozens.
static int SliderPos(HWND h) { return (int)GetWindowLongPtrA(h, 0); }
static int SliderMax(HWND h) { return (int)GetWindowLongPtrA(h, sizeof(LONG_PTR)); }

static void SliderTrack(HWND h, int* x0, int* x1)
{
    RECT r;
    GetClientRect(h, &r);
    int radius = S(8) + 1;
    *x0 = r.left + radius;
    *x1 = r.right - radius;
}

static void SliderSet(HWND h, int pos, int code)
{
    int max = SliderMax(h);
    if (pos < 0) pos = 0;
    if (pos > max) pos = max;
    bool moved = pos != SliderPos(h);
    SetWindowLongPtrA(h, 0, pos);
    if (moved) InvalidateRect(h, NULL, FALSE);
    if (code >= 0 && (moved || code == TB_ENDTRACK))
        SendMessageA(GetParent(h), WM_HSCROLL, MAKEWPARAM(code, pos), (LPARAM)h);
}

static int SliderFromMouse(HWND h, int x)
{
    int x0, x1, max = SliderMax(h);
    SliderTrack(h, &x0, &x1);
    if (x1 <= x0 || max <= 0) return 0;
    return (int)(((double)(x - x0) / (x1 - x0)) * max + 0.5);
}

static void SliderPaint(HWND h)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT r;
    GetClientRect(h, &r);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, r.right, r.bottom);
    HGDIOBJ old = SelectObject(dc, bmp);
    FillRect(dc, &r, s_backBrush);

    bool enabled = IsWindowEnabled(h) != 0, focus = GetFocus() == h;
    int x0, x1, max = SliderMax(h), pos = SliderPos(h);
    SliderTrack(h, &x0, &x1);
    int cy = (r.top + r.bottom) / 2;
    int x = max > 0 ? x0 + (int)((double)pos / max * (x1 - x0) + 0.5) : x0;
    RECT all = { x0, cy - S(2), x1, cy + S(2) };
    RoundRect(dc, all, S(4), kField, kField);
    RECT done = { x0, cy - S(2), x, cy + S(2) };
    COLORREF accent = enabled ? kAccent : kOff;
    if (x > x0) RoundRect(dc, done, S(4), accent, accent);
    int radius = S(8);
    RECT knob = { x - radius, cy - radius, x + radius, cy + radius };
    COLORREF fill = enabled ? kText : kOff;
    RoundRect(dc, knob, radius * 2, fill, focus && enabled ? kAccent : fill);

    BitBlt(wdc, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

static LRESULT CALLBACK SliderProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case TBM_SETRANGE:
        SetWindowLongPtrA(h, sizeof(LONG_PTR), HIWORD(lp));
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case TBM_SETPOS:
        SliderSet(h, (int)lp, -1);
        return 0;
    case TBM_GETPOS:
        return SliderPos(h);
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        SliderPaint(h);
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(h);
        SetCapture(h);
        SliderSet(h, SliderFromMouse(h, (short)LOWORD(lp)), TB_THUMBTRACK);
        return 0;
    case WM_MOUSEMOVE:
        if (GetCapture() == h) SliderSet(h, SliderFromMouse(h, (short)LOWORD(lp)), TB_THUMBTRACK);
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (msg == WM_LBUTTONUP && GetCapture() == h) {
            ReleaseCapture();  // sends WM_CAPTURECHANGED, which ends the drag
            return 0;
        }
        if (msg == WM_CAPTURECHANGED) SliderSet(h, SliderPos(h), TB_ENDTRACK);
        return 0;
    case WM_KEYDOWN: {
        int pos = SliderPos(h), max = SliderMax(h), page = max / 10 > 1 ? max / 10 : 1;
        switch (wp) {
        case VK_LEFT: case VK_DOWN: pos -= 1; break;
        case VK_RIGHT: case VK_UP: pos += 1; break;
        case VK_PRIOR: pos += page; break;
        case VK_NEXT: pos -= page; break;
        case VK_HOME: pos = 0; break;
        case VK_END: pos = max; break;
        default: return DefWindowProcA(h, msg, wp, lp);
        }
        SliderSet(h, pos, TB_ENDTRACK);
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static void RegisterSlider(HINSTANCE inst)
{
    WNDCLASSA wc = {};
    wc.lpfnWndProc = SliderProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_HAND);
    wc.cbWndExtra = 2 * sizeof(LONG_PTR);  // position, maximum
    wc.lpszClassName = kSliderClass;
    RegisterClassA(&wc);
}

static void ShowHelp(const char* help)
{
    s_infoWarn = help[0] == '!';
    SetWindowTextA(s_info, help + s_infoWarn);
    InvalidateRect(s_info, NULL, TRUE);
}

static void ShowHelpFor(HWND hwnd)
{
    static HWND last;
    if (hwnd == last) return;
    last = hwnd;
    for (Option& o : kOptions)
        if (hwnd == o.ctrl || hwnd == o.label || hwnd == o.value) {
            ShowHelp(o.help);
            return;
        }
    for (int i = 0; i < kPresetCount; i++)
        if (hwnd == s_preset[i]) ShowHelp(kPresets[i].help);
}

static LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(wnd, &r);
        FillRect((HDC)wp, &r, s_backBrush);
        RECT side = { 0, 0, S(kSideW), r.bottom };
        FillRect((HDC)wp, &side, s_sideBrush);
        int height = Height();
        RECT sep = { S(kContentX), S(height - kInfoH + 8), r.right - S(24), S(height - kInfoH + 9) };
        HBRUSH b = CreateSolidBrush(kBorder);
        FillRect((HDC)wp, &sep, b);
        DeleteObject(b);
        return 1;
    }
    case WM_DRAWITEM:
        DrawButton((DRAWITEMSTRUCT*)lp);
        return TRUE;
    case WM_SETCURSOR:
        ShowHelpFor((HWND)wp);
        break;
    case WM_ACTIVATE:
        // dx.dll may have written to the INI while the window was in the background
        if (LOWORD(wp) != WA_INACTIVE && s_wnd) {
            LoadIni();
            LoadControls();
            for (HWND b : s_preset) InvalidateRect(b, NULL, TRUE);
        }
        break;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id >= ID_OPTION0 && id < ID_OPTION0 + kOptionCount) {
            Option& o = kOptions[id - ID_OPTION0];
            if (o.kind == BOOL_OPT && code == BN_CLICKED) {
                s_toggle[id - ID_OPTION0] = !s_toggle[id - ID_OPTION0];
                InvalidateRect(o.ctrl, NULL, TRUE);
                Changed(o);
            } else if (o.kind == CHOICE && code == BN_CLICKED) {
                // the items as a popup menu below the field
                auto items = Choices(o);
                HMENU menu = CreatePopupMenu();
                for (int i = 0; i < (int)items.size(); i++)
                    AppendMenuA(menu, MF_STRING | (i == s_choice[id - ID_OPTION0] ? MF_CHECKED : 0), i + 1,
                                items[i].second.c_str());
                RECT r;
                GetWindowRect(o.ctrl, &r);
                int pick = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom + S(2), 0,
                                          wnd, NULL);
                DestroyMenu(menu);
                if (pick > 0) {
                    s_choice[id - ID_OPTION0] = pick - 1;
                    InvalidateRect(o.ctrl, NULL, TRUE);
                    Changed(o);
                }
            }
            if (code == BN_SETFOCUS) ShowHelpFor(o.ctrl);
        } else if (id >= ID_NAV0 && id < ID_NAV0 + kPageCount) {
            ShowPage(id - ID_NAV0);
        } else if (id >= ID_PRESET0 && id < ID_PRESET0 + kPresetCount) {
            if (code == BN_SETFOCUS) {
                ShowHelpFor((HWND)lp);
            } else if (code == BN_CLICKED) {
                char q[200];
                sprintf(q, "Set every option to \"%s\"?\n\nCheats and the placed menu camera stay as they are.",
                        kPresets[id - ID_PRESET0].name);
                if (MessageBoxA(wnd, q, kTitle, MB_YESNO | MB_ICONQUESTION) == IDYES) ApplyPreset(kPresets[id - ID_PRESET0]);
            }
        } else if (id == ID_RESTORE) {
            RestoreBackup(wnd);
        } else if (id == ID_OPEN) {
            if (PickIni(wnd)) Open();
        } else if (id == ID_CLOSE) {
            DestroyWindow(wnd);
        }
        return 0;
    }
    case WM_HSCROLL:
        for (Option& o : kOptions)
            if (o.ctrl == (HWND)lp) {
                if (LOWORD(wp) == TB_ENDTRACK) Changed(o);
                else ShowValue(o, o.lo + HIWORD(wp) * o.step);  // dragging: the value only
                ShowHelpFor(o.ctrl);
            }
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND c = (HWND)lp;
        Role role = (Role)GetWindowLongPtrA(c, GWLP_USERDATA);
        bool side = role == R_BRAND || role == R_SIDEDIM;
        SetBkMode(dc, TRANSPARENT);
        COLORREF col = kText;
        if (role == R_BRAND) col = kAccent;
        else if (c == s_info && s_infoWarn) col = kWarn;
        else if (role == R_DIM || role == R_SIDEDIM) col = kDim;
        else if (role == R_VALUE || role == R_LABEL) {
            bool dim = false;
            for (const Option& o : kOptions)
                if (o.label == c || o.value == c) dim = s_dim[IndexOf(o)];
            col = dim ? kOff : (role == R_VALUE ? kAccent : kText);
        }
        SetTextColor(dc, col);
        return (LRESULT)(side ? s_sideBrush : s_backBrush);
    }
    case WM_DPICHANGED: {
        s_dpi = HIWORD(wp);
        MakeFonts();
        EnumChildWindows(wnd, DestroyChild, 0);
        for (Option& o : kOptions) o.label = o.ctrl = o.value = NULL;
        CreateControls();
        LoadControls();
        ShowPage(s_page);
        const RECT* r = (const RECT*)lp;
        SetWindowPos(wnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(wnd, NULL, TRUE);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(wnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show)
{
    DpiAware();
    s_dpi = WindowDpi(NULL);
    EnableDarkMenus();
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    RegisterSlider(inst);
    s_backBrush = CreateSolidBrush(kBack);
    s_sideBrush = CreateSolidBrush(kSide);

    // the INI next to this program; if neither it nor the game is there, ask
    GetModuleFileNameA(NULL, s_path, MAX_PATH);
    char* slash = strrchr(s_path, '\\');
    strcpy(slash ? slash + 1 : s_path, "popfix.ini");
    if (GetFileAttributesA(s_path) == INVALID_FILE_ATTRIBUTES) {
        char exe[MAX_PATH];
        strcpy(exe, s_path);
        strcpy(strrchr(exe, '\\') + 1, "gpp.exe");
        if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES && !PickIni(NULL)) return 0;
    }

    WNDCLASSEXA wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.hIcon = (HICON)LoadImageA(inst, MAKEINTRESOURCEA(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                 GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageA(inst, MAKEINTRESOURCEA(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = "popfix_config";
    RegisterClassExA(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    SIZE size = WindowSize(style);
    s_wnd = CreateWindowExA(0, wc.lpszClassName, "Sands of Time PC fixes - settings", style, CW_USEDEFAULT, CW_USEDEFAULT,
                            size.cx, size.cy, NULL, NULL, inst, NULL);
    int dpi = WindowDpi(s_wnd);  // the monitor the window opened on
    if (dpi != s_dpi) {
        s_dpi = dpi;
        size = WindowSize(style);
        SetWindowPos(s_wnd, NULL, 0, 0, size.cx, size.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    MakeFonts();
    DarkTitleBar(s_wnd);
    CreateControls();
    Open();
    ShowPage(0);
    ShowWindow(s_wnd, show);

    MSG m;
    while (GetMessageA(&m, NULL, 0, 0)) {
        if (IsDialogMessageA(s_wnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return 0;
}
