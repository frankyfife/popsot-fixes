# Technical notes

Reverse-engineering notes behind the fixes. Addresses refer to the GOG executable
(`gpp.exe`, identical code to `POP.EXE` v181, image base `0x400000`, no ASLR) and to
the Xbox `default.xbe` (USA). The Xbox side was analysed with Ghidra +
[ghidra-xbe](https://github.com/XboxDev/ghidra-xbe) and an xemu RenderDoc capture.

## Water: what is the same on both platforms

The Jade engine water is a CPU heightfield simulation. The PC executable contains
all of it; the code is line-for-line equivalent to the Xbox build, including every
constant (damping, thresholds, a random "rain drop" tick every 1/6 s, etc.).

| Part | PC | Xbox |
|---|---|---|
| Per-patch simulation step | `0x6cf280` | `0x51c00` |
| Impulses from characters / objects | `0x6cee60` | `0x51800` |
| Normals from heights (scale 4.0) | `0x6cf100` | `0x51a70` |
| Heights → vertex buffer | `0x6d0970` | `0x53190` |
| Tile system update / edge exchange | `0x6cfcb0` | – |

Rendering is a screen-space refraction:

1. The scene is copied to a texture.
2. A water mask is rendered into that texture's alpha channel.
3. The water patches are drawn with a vertex shader that offsets the screen-space
   texture coordinates along the refracted view vector, and a pixel shader that
   blends the refracted image, a water colour and a specular term.

The **vertex shader** is the same program on both platforms (the Xbox builds it at
runtime from source text in the XBE; the PC version assembles an equivalent `vs_1_1`).
None of the 243 precompiled Xbox vertex shaders in `shaders.bin` displaces vertices –
the waves are purely CPU-side.

The **pixel shader** is the same too: the xemu capture shows the Xbox register-combiner
setup is exactly the PC `ps_1_1` (`lrp` chain with water colour 0.086 and specular
0.102 on Xbox vs. 0.085 / 0.1 on PC).

## Water: what differs

### 1. Refraction source resolution and filtering
On the Xbox, texture stages 1–3 of the water draw sample a full 640×480 copy of the
frame with linear filtering.

On the PC, the chain is:

1. The back buffer is `StretchRect`'d into a **512×512** texture.
2. That texture is drawn back to a full-screen render target with **point** filtering
   (scene RGB) and the water mask is added in alpha.
3. That target is `StretchRect`'d into another 512×512 texture (`0xae16f0`).
4. The water shader samples it with **point** filtering (sampler states set in `0x663ff0`).

At 4K this turns the view through the water into large blocks. The same 512×512
buffers (plus a 256…8 mip-like chain and several 256×256 R5G6B5 targets) feed motion
blur and glow, which are blocky for the same reason.

**Fix (`proxy.cpp`):** every `StretchRect` that downsamples a full-screen render target
into a texture gets a full-resolution "shadow" copy. When a small texture that still
holds exactly that copy (the game has not rendered into it since) is sampled while
drawing to a larger target, the shadow is bound instead. Any other upscaled
render-target texture is switched from point to linear filtering. Original texture
and sampler state are restored after each draw.

### 2. Refraction strength
The water vertex shader constants are uploaded from static data at `0x7f8a8c`:

```
c12 = REFRACT  (ratio, ratio², x scale, y scale)
c13 = (0.5, 0, ZOFFSET, ZMAX)
```

The screen-space offset is multiplied by `min(eye_z, ZMAX) + ZOFFSET`.

| | REFRACT | ZOFFSET | ZMAX | max. factor |
|---|---|---|---|---|
| Xbox (`c[-66]`, `c[-65]`) | 0.6, 0.36, 28 px, 4.5 px | 3 | 10 | 13 |
| PC | 0.6, 0.36, 28/640, 4.5/480 | 1 | 3 | 4 |

`REFRACT` was correctly converted from Xbox pixel units to normalised coordinates, but
the depth scale was lowered, making the refraction about 3.25× weaker. The proxy
restores 3 / 10 when the game uploads these constants.

### Water option gate
The water pass only runs when the video option *Water* is on (config struct
`*(0x80c0c4) + 0xd0`) and the device reports vertex/pixel shader ≥ 1.1
(`+0xf0` bits 0/1, filled from `D3DCAPS9` in `0x662e60`). Otherwise a flat fallback
mesh is drawn.

## Motion blur

The zoom/speed blur (render method `0x670710`, effect vtable `0x7b18e4`) copies the back
buffer into 512×512 `A8R8G8B8` render targets (`StretchRect`), blurs half of that area
with the blur pass `0x66b540` and composites the result over the screen. The blur pass
(thiscall `dst, dstW, dstH, src, srcW, srcH, kernel, taps, ?, uExtent, vExtent`) draws
a quad of `dstW × dstH` pixels through the quad helper `0x66b300` (`x0, y0, x1, y1` in
target pixels); `uExtent` / `vExtent` are the texture-space fractions it samples, and
the tap step is `extent / srcSize`.

**Fix (`proxy.cpp`):** those render targets are created at up to 4× their size
(`[post] blur_resolution`, limited by the back-buffer height). Pixel sizes that refer to
an enlarged target are scaled to match – `StretchRect` rectangles, the quad helper's
coordinates and the blur pass's destination size – while the texture-space extents stay
unchanged, so the blur keeps its radius and only gains resolution. `[post] blur=0`
turns the render method into a `ret`.

## Menus and input

Gameplay input is `InputManagerPC` (`0x41f620`): DirectInput 8 keyboard, mouse and game
controllers, with known pads listed in `gamepads.dat`.

The front-end menus (`PCMenuManager`, `MNU_*` pages loaded from `*.MGM` packages) do not
read DirectInput controllers at all. They get:

* **mouse** events from the DirectInput mouse poll (`0x4225a0`), forwarded to the UI
  manager at `*(0x80bbd8) + 0xc` in a virtual 640×480 space:
  `0x7125f0` move, `0x712500` button down, `0x712530` button up, `0x712560` double click
  (thiscall, argument `{button, x | y << 16}`);
* **keyboard** characters from `WM_KEYUP` (`0x421d20`), only used for text entry
  (profile names) and a few Escape actions.

UI manager: page stack at `+4` (8-byte entries), page count at `+0x10`.
Page: element vector at `+0x28/+0x2c`, focused element `+0x40`.
Element: widget at `+0x2c` (vtable `+0x10` = get rect `{x0, x1, y0, y1}`), interactive
flag bit 3 of the byte at `+100`, visibility check `0x711e40`.

`MNU_List` widgets (e.g. the main menu entries) hold their items as rows: rows rect
`+0x6c`, visible row count `+0x7c`, first visible item `+0x80`, current item `+0x30`,
scroll arrows at `+0x64` / `+0x74` (present if `+0x14` / `+0x20` are set). Row hit test
`0x714e70`; a list is recognised by its mouse-up handler `0x7150c0` in vtable slot `0x24`.

The UI manager also has key entry points, `0x7124c0` key down and `0x7124e0` key up
(thiscall, argument `{vk, char}`), which the keyboard path above uses. Pages route keys
to the focused element's widget (vtable `+0x18` down, `+0x1c` up); lists (vtable
`0x7b7ce0`) move their current row with the arrow keys, sliders (`0x7b7d90`) change
their value. After every key event the page gives focus to the element with focus flag
bit 3 (`0x716c50`), and the hover highlight follows the UI cursor (manager `+0x1a`).
Element: shown byte `+0x24`, enabled flag bit 1 of `+100`, focus / unfocus in vtable
slots 3 / 4, neighbour links from `+0x54` (all empty in the PC page data).

**Menu pad (`menupad.cpp`):** reads XInput and feeds the page through the key entry
points: D-pad / left stick = arrow keys (with key repeat), A = Enter, B / Y = Escape.
Because the PC pages have no neighbour links, arrow keys that the focused widget does
not use (it changed neither focus nor row) are resolved spatially: the nearest shown,
enabled, interactive element in that direction (by widget rect) gets the focus. The UI
cursor is then moved onto a point of that element that the page's own hit test
(`0x716d90`) confirms, so the normal hover highlight follows. On a list, A double-clicks
the current row (found with the row hit test `0x714e70`, scrolling via `+0x80` if
needed), because the profile and save lists select on double click.

**Level select:** the PC main menu still contains the developer page `P_SpecialLoad`
(page 15, a list of every level), but the button is created hidden (`push 0` at
`0x4094fb`) and the click handler (`0x409180`) has no case for it. The patch shows the
button and opens the page through the PC page opener `0x409870`.

## Console (Xbox) menus

The PC build still contains the complete console front end: Jade AI scripts, compiled
to C, build pages of fading text, button hints and 3D objects, driven by the engine's
menu manager (PC `*(0xaf2414)`, Xbox `*(0x7584f0)`, same layout on both). Comparing
the two executables function by function shows the page class (PC vtables `0x7b1fdc` /
`0x7b1ff4`, Xbox `0x41f4f8`) and all its logic are unchanged. The port disabled the
console menus in four places (restored only with `[menus] console=1`; saving and loading
from the console pages is still incomplete):

| # | PC change | Xbox original | Fix (`consolemenu.cpp`) |
|---|---|---|---|
| 1 | "Open menu" (`0x467b70`) activates the console page, then always calls a PC hook (`0x402760`) that pushes a mouse page on top | no hook (`0xe53f0`) | hook becomes `ret` |
| 2 | Menu manager tick (`0x672310`) only ticks the top page and animations | tick (`0x17d10`) also lets every menu control (input action, manager `+0x14..+0x18`) poll its input and closes pages that finished fading out | Xbox tick rebuilt from functions still present on PC (`0x6979f0`, `0x697cc0`, `0x6d26c0`) |
| 3 | Pad state builder (`0x41fa10`) clears all pad bits while a PC front-end flag (`*(0x80bc44)+0x1a`) is set | – | branch at `0x41fcdb` made unconditional |
| 4 | Element show/hide (`0x69a840`) never reads its argument: it returns unless the element is visible, then hides it | `0x38e20` stores and applies the new state | function replaced with the Xbox behaviour, calling the unchanged body logic |

Menu manager: open page count `+0xf0`, page stack `+0x6c + i*4` (1-based), page
state `+0x84` (observed: 0 inactive, 1 fading in, 2 active; 4 = fade-out finished,
which the tick turns into 5 via `0x697cc0`; 6 while another page opens on top).

## Controller

The engine is an Xbox engine: every input action is an Xbox pad control, and the
Xbox build maps its pad in `0xdc160`:

| Action / pad bit | Xbox control | Action / pad bit | Xbox control |
|---|---|---|---|
| 0 | A | 8 | Back |
| 1 | B | 9 | Start |
| 2 | X | 10 | right thumb |
| 3 | Y | 11 | left thumb |
| 4 | Black | 12 | D-pad up |
| 5 | White | 13 | D-pad right |
| 6 | L trigger | 14 | D-pad down |
| 7 | R trigger | 15 | D-pad left |

On PC, `InputManagerPC::GetActionValue` (`0x41d7b0`, thiscall `float(action)`) returns
the highest value of up to three bindings per action; the digital pad bits
(`0x80d074`, previous frame `0x80d070`) are built from actions 0–15 every frame. The
PC key bindings leave the D-pad actions 12–15 unbound, which is why the console menus
cannot be navigated with arrow keys. Sticks come from `0x41fd20` (cdecl
`void(int pad, float* out, int stick)`): movement from actions `0x25`–`0x28`, camera
from the mouse plus actions `0x29`–`0x2c`, gated by the mask at `0x7f1578`.

**Fix (`gamepad.cpp`):** `GetActionValue` returns the maximum of the original value and
an XInput pad mapped as above for the face buttons, D-pad, Start and stick clicks. Shoulders
and triggers follow the PS2 layout of the game, since the original Xbox pad had no shoulder
buttons: LB = L trigger (rewind), RB = R trigger (special action), LT = White (alternate
view), RT = Black (look). Back is left unmapped because the
PC port bound action 8 to "quit game"). The stick query is filled straight from the pad
with a round dead zone.

### Vibration

The PC build has no force feedback. On Xbox, `0xdc700` (pad in EAX, strength 0–255 in
EDX, large motor) and `0xdc6a0` (pad in ESI, on/off in ECX, small motor) drive the
motors for a number of frames, and `0xdbac0` is the "Vibration on/off" option.

The AI script functions that start the motors are still present on PC and are found
through the AI function table, whose ids match between the builds:

| Id | Xbox | PC | Arguments |
|---|---|---|---|
| `0x1b63` | `0x2dff20` | `0x498800` | pad, frames (small motor) |
| `0x1b64` | `0x2dff70` | `0x498890` | pad, strength, frames (large motor) |
| `0x1b68` | `0x2e0070` | `0x4989c0` | enabled (option; PC stores it at `0x7f157c`) |

On PC these functions and the engine code that drives rumble pop their arguments
and then call `0x563530`, a bare `ret` that the compiler shares between ~1000 stripped
call sites. **Fix (`gamepad.cpp`):** only the motor calls are redirected, after checking
that each one still calls the empty function:

| PC function | Xbox | Rumble | Small motor call | Large motor call |
|---|---|---|---|---|
| `0x558f10` | `0x18e510` | the Prince (hits, landings, …) | `0x558f84` | `0x558fd3` |
| `0x5e7fb0` | `0x20c730` | rumble generators (quakes, machinery) | – | `0x5e8193` |
| `0x578420` | `0x12e000` | generator start / stop | `0x578489` | `0x578473` |
| `0x5789a0` | `0x12e5d0` | generator start / stop | `0x578a6a` | `0x578a14` |
| `0x498800` / `0x498890` | `0x2dff20` / `0x2dff70` | script functions | `0x498867`, `0x49887f` | `0x49892e` |

The replacements run the XInput motors for the given number of game frames (converted
to time at 30 Hz, as the Xbox counts them down per game frame), with strength
`s/255 × 65535` like the Xbox, honouring the Vibration option and only while the game
window is in the foreground. Stop calls (0 frames, e.g. when a cutscene starts) are not
needed because every rumble ends by itself.
