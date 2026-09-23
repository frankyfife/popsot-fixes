# Prince of Persia: The Sands of Time – PC fixes

Brings the PC (GOG) release of *Prince of Persia: The Sands of Time* closer to the
original Xbox version, without touching any game files.

The fixes are a single drop-in `dx.dll` that sits between the game and Direct3D 9.

## What it does

### Water that looks like the Xbox version
The PC port renders water with the same wave simulation and the same shaders as the
Xbox version, but two things made it look flat, milky and blocky:

| Problem on PC | Cause | Fix |
|---|---|---|
| Blocky, pixelated view through the water (and blocky motion blur / glow) at modern resolutions | Post effects are built from fixed 512×512 render targets and scaled back to the screen with point sampling | Full-screen downsamples get a full-resolution copy that is used instead; processed effect buffers are upscaled with linear filtering |
| Barely visible light refraction | The port lowered the refraction depth scale (`ZOFFSET`/`ZMAX` = 1/3 instead of the Xbox's 3/10), making the distortion about 3× weaker | Xbox values are restored |

Water colour and specular strength were already identical to the Xbox and are left alone.

### Controller support in the menus *(work in progress)*
The PC front-end menus only react to the mouse – a controller does nothing there,
not even with GOG's input wrapper. The DLL reads an XInput controller and drives
the menu's own mouse handling:

| Controller | Action |
|---|---|
| D-pad / left stick | Move between menu entries (the entry gets its normal hover highlight) |
| A | Activate the selected entry |
| B | Escape / back where the game supports it |

In-game controls are not affected.

## Requirements

* GOG release of *Prince of Persia: The Sands of Time* (runs `gpp.exe`, engine v181)
* Windows 10/11
* For menu navigation: an XInput controller (Xbox controllers work natively)

Other releases (retail/Ubisoft) are not supported yet. The menu code checks the game
functions it uses before calling them and switches itself off on an unknown build.

## Installation

1. In the game folder, rename GOG's `dx.dll` to `dx_gog.dll`.
2. Copy `dx.dll` from this project into the game folder.

GOG's own compatibility layer keeps working; this DLL forwards everything to it.

**Uninstall:** delete `dx.dll` and rename `dx_gog.dll` back to `dx.dll`.

**F10** toggles the graphics fixes while playing, for comparison.
Diagnostics are written to `popfix.log` in the game folder.

## Building

Requires the Visual Studio 2022 Build Tools (C++ x86). Run `build.bat`; the result is
`build\dx.dll`.

## How it works

See [docs/TECHNICAL.md](docs/TECHNICAL.md) for the reverse-engineering notes:
how the water is rendered on Xbox and PC, where the differences are, and how the
menu system handles input.

## Credits

* [xemu](https://xemu.app) – the RenderDoc capture of the Xbox version attached to
  [xemu issue #1864](https://github.com/xemu-project/xemu/issues/1864) made it possible
  to compare the Xbox water shaders and constants directly
* [ghidra-xbe](https://github.com/XboxDev/ghidra-xbe) – XBE loader for Ghidra
* [WineHooks](https://github.com/Daniel-Lobo/WineHooks) (formerly Peixoto's patch) –
  notes on the game's fog and post-effect behaviour

Prince of Persia is a trademark of Ubisoft Entertainment. This project is not affiliated
with Ubisoft or GOG and contains no game code or assets.

## License

MIT – see [LICENSE](LICENSE).
