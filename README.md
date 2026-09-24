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
The refraction strength can be raised further in `popfix.ini` (default 1.5 × Xbox).

### Sharp motion blur
The zoom/speed blur (e.g. when the Prince runs along walls or uses the dagger) is also
built from 512×512 render targets. Those targets are enlarged up to 4× (limited by the
screen height), so the blur stays smooth instead of pixelated at 1440p/4K. The blur can
also be switched off completely in `popfix.ini`.

### Full controller support
Any XInput controller (Xbox One/Series/Elite, …) works everywhere – menus, gameplay,
skipping videos. It does not depend on the game's PC control settings or on GOG's
DirectInput wrapper.

| Button | Gameplay | Menus |
|---|---|---|
| A | action / jump | confirm, select list entry |
| B | cancel | back |
| X | attack | |
| Y | dagger | back |
| LB | rewind time | |
| RB | special action | |
| LT | alternate view | |
| RT | look | |
| Start | pause | |
| D-pad / left stick | move | navigate |

Face buttons are laid out like the Xbox version; shoulder buttons and triggers follow
the PS2 version, because the original Xbox pad had no shoulder buttons. Sticks are
analog with a round dead zone. The Back/View button is left unassigned (the PC port
bound it to "quit game").

The PC menus are mouse menus. The controller drives them directly: the highlight moves
from element to element, lists (profiles, save games) scroll and are selected with A,
sliders are changed with left/right.

Tutorial and menu hints name the controller buttons while a controller is connected
("Use Move [LS] to control the Prince", "A : Select") instead of the keyboard keys.

### Free camera
Back (View) or F9 detaches the camera: left stick / WASD move, right stick / arrow keys
look, LT / RT or Q / E move down / up, LB / Ctrl slow down, RB / Shift speed up. The game
gets no input while the free camera is on (Start still pauses).

### Vibration
The PC port removed force feedback completely, although the game still triggers it.
Vibration is back – hits, landings, earthquakes, machinery – with the same
strength and duration as on Xbox. It follows the in-game Vibration option and stops
when the game window loses focus.

### Main menu on wide screens
The main menu's 3D scene was built for 4:3; on wide screens its edges show. Like the
PS3 HD version, the menu camera is moved back and up (adjustable, F6 / F7 and
Shift+F6 / F7 in the main menu). When a new game starts, the camera flies to the
Prince without a jump.

### Videos
The videos are shown with their correct aspect ratio instead of stretched. Upscaled
versions can be added without converting them back to Bink: put `<name>.mp4` next to
`Video\<name>.int`, and the game shows its picture while the original file still
provides sound (all languages), timing and skipping. `tools\export_videos.ps1` exports
the originals as ProRes for an upscaler such as Topaz Video AI. Any size up to
4096×4096 and any frame rate work; H.264 always, H.265/AV1 with the Windows codec
extensions.

### EAX and surround sound
The game's EAX 2 reverb and 3D sound need hardware DirectSound3D, which Windows no
longer has. With [DSOAL](https://github.com/kcat/dsoal) in the game folder (`dsound.dll`,
`dsoal-aldrv.dll`, optional `alsoft.ini`), the fix routes the game's sound through it:
EAX is emulated and output follows the Windows speaker setup (stereo, headphones,
5.1, 7.1). 3D audio and EAX are switched on automatically (`[sound] eax`). DSOAL is not
included in this project.

### Level select
The main menu gets the developer's **Special Load** entry, which is hidden in the PC
release: it lists every level of the game and loads it directly.

### Xbox menus *(experimental, off by default)*
The console front end (3D scene, fading text, controller hints) is still inside the PC
executable. `[menus] console=1` switches it back on. Saving and loading from those
pages is incomplete on PC, so the PC menus stay the default.

## Settings

All settings are optional. Copy `popfix.ini` next to `dx.dll`; every value in the sample
file is the default.

| Setting | Default | Meaning |
|---|---|---|
| `[water] refraction` | `1.5` | refraction strength, 1.0 = Xbox |
| `[post] blur_resolution` | `4` | size of the blur targets, 1 = original 512×512, up to 4× |
| `[post] blur` | `1` | 0 disables the zoom/speed blur |
| `[menus] console` | `0` | 1 = Xbox console menus (experimental) |
| `[menus] camera_back` / `camera_up` | `3.5` / `6.25` | main-menu camera offset, 0 = original |
| `[controller] prompts` | `auto` | button names in hints: `auto`, `controller` or `keyboard` |
| `[video] keep_aspect` | `1` | 0 stretches the videos like the original |
| `[sound] eax` | `1` | switch 3D audio and EAX on when DSOAL is installed |
| `[debug] verbose` | `0` | 1 = detailed diagnostics in `popfix.log` |

## Requirements

* GOG release of *Prince of Persia: The Sands of Time* (runs `gpp.exe`, engine v181)
* Windows 10/11
* An XInput controller (Xbox controllers work natively); keyboard still works

Other releases (retail/Ubisoft) are not supported yet. Every patch checks the game
code it touches before changing it and switches itself off on an unknown build.

## Installation

1. In the game folder, rename GOG's `dx.dll` to `dx_gog.dll`.
2. Copy `dx.dll` from this project into the game folder.
3. Optional: copy `popfix.ini` next to it to change the settings.

GOG's own compatibility layer keeps working; this DLL forwards everything to it.

**Uninstall:** delete `dx.dll` and rename `dx_gog.dll` back to `dx.dll`.

**F10** toggles the graphics fixes while playing, for comparison. **F9** toggles the free
camera.
Diagnostics (and a crash report, if the game crashes) are written to `popfix.log` in the
game folder.

## Building

Requires the Visual Studio 2022 Build Tools (C++ x86). Run `build.bat`; the result is
`build\dx.dll`.

## How it works

See [docs/TECHNICAL.md](docs/TECHNICAL.md) for the reverse-engineering notes:
how the water and post effects are rendered on Xbox and PC, where the differences are,
how the menu system handles input, where the vibration went, and how the button hints,
cameras, videos and sound are hooked.

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
