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

### The Xbox menus are back *(experimental)*
The PC port replaced the console front end with mouse-only menus – a controller does
nothing there, not even with GOG's input wrapper. The console menus are still inside
the PC executable, switched off in four places. This DLL switches them back on: you
get the Xbox menus (3D scene, fading text, controller hints) instead of the PC ones.

Known issues: some pages still show PC key hints, the load-game page does not list
PC save games yet, and vibration is not implemented (the PC port removed it).

### Native Xbox controller support
Any XInput controller (Xbox One/Series/Elite, …) works everywhere – menus, gameplay,
skipping videos – with the original Xbox layout. It does not depend on the game's PC
control settings or on GOG's DirectInput wrapper. Sticks are analog with a round dead
zone. The Back/View button is left unassigned (the PC port bound it to "quit game").

## Requirements

* GOG release of *Prince of Persia: The Sands of Time* (runs `gpp.exe`, engine v181)
* Windows 10/11
* An XInput controller (Xbox controllers work natively); keyboard still works

Other releases (retail/Ubisoft) are not supported yet. Every patch checks the game
code it touches before changing it and switches itself off on an unknown build.

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
