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

**Menu pad (`menupad.cpp`):** reads XInput, builds navigation targets (interactive
elements, list rows found by probing the list's hit test, scroll arrows), moves the
UI cursor to the selected target (which triggers the normal hover highlight) and
clicks via the UI manager's own mouse entry points. Function prologues are verified
before use.
