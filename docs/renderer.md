# Renderer

`NebulaRender.dll` draws the photo cloud. It is C++20 on Direct3D 11, presenting through a composition swap
chain into the app's WinUI 3 `SwapChainPanel`. There are no per-photo XAML elements. The app talks to it
through a C API (`NebulaRender.h`) wrapped by `src/Nebula.Windows/Services/NativeRenderer.cs`.

## Build

```powershell
.\src\Nebula.Render\build.ps1 -Architecture ARM64 -Configuration Debug -SmokeTest
```

The app project and `build.ps1` in the repository root call this script for you. It finds the MSVC toolset
with `vswhere`, compiles the HLSL to DXBC at build time and embeds the bytecode in the DLL, links the C++
runtime statically and enables Control Flow Guard. It needs the WinUI interop header
`microsoft.ui.xaml.media.dxinterop.h` from the version of the Windows App SDK the app restored, and checks that
the header declares the WinUI 3 `ISwapChainPanelNative` (IID `63aad0b8-7c24-40ff-85a8-640d944cc325`), not the
UWP one.

| Option | Effect |
|---|---|
| `-Architecture ARM64\|x64`, `-Configuration Debug\|Release` | What to build |
| `-SmokeTest` | Also build and run the GPU smoke tests (see [testing.md](testing.md)) |
| `-StressOnly`, `-FootprintOnly` | Run only the 100,000-photo stress test, or only the footprint tests |
| `-LibraryPath <library.json>` | Add a real collection to the tests (`-RealLibraryOnly`, `-DecodeAuditOnly` narrow them) |
| `-OutputDirectory`, `-WinUIIncludeDirectory` | Override the output folder or the header location |

## Source files

| File | Contents |
|---|---|
| `NebulaRender.h` / `.cpp` | The C API, the renderer, its render thread and decode workers |
| `Shaders.h`, `CompileShaders.cpp` | HLSL for photos, glass, lines and depth effects; the build-time shader compiler |
| `LodPolicy.h`, `RenderLimits.h` | Level-of-detail rules and fixed budgets |
| `MipCache.h` | The persistent BC7 thumbnail cache |
| `OriginalLoader.h`, `ThumbnailDecoder.h`, `MosaicAverages.h` | WIC decoding, colour management and colour mosaics |
| `PickGrid.h`, `BoundedQueue.h`, `Highlight.h`, `GlassMaterial.h`, `DepthEffects.h`, `RenderRecords.h` | Picking, work queues, highlight easing, glass and depth-effect parameters, GPU record layouts |
| `smoke.cpp`, `*Tests.h` | The smoke test executable |
| `vendor/bc7enc` | The BC7 encoder ([bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo), MIT) |

## C API and threading

All functions use the standard Windows calling convention and return an `HRESULT`. C++ exceptions never cross
the boundary. `nb_get_error` copies the latest operational error.

- Call `nb_create` and `nb_destroy` on the panel's UI thread, passing the WinUI 3 `SwapChainPanel` as
  `IInspectable`. The renderer queries and holds `ISwapChainPanelNative` until it is destroyed.
- Every other call is thread-safe, copies its arguments before it returns, and does no file I/O. Success means
  the command was accepted; the render thread applies it on its next frame.
- Sizes and picking coordinates are physical pixels. Orbit and pan deltas are DIPs. A wheel notch is 120.
  A zero-size panel suspends presentation.
- Photo positions are in tile units: every photo's long edge is one unit. Layouts divide their coordinates by
  32 before they reach the renderer.
- Ids are unique, non-negative 32-bit integers; `-1` clears selection or hover.
- Versioned structs start with their size (`NbStats.size = 32`, `NbView.size = 88`, `NbLodStats.size = 128`)
  so the API can grow without breaking callers. Struct sizes are checked by the smoke test on both
  architectures.

| Function group | Purpose |
|---|---|
| `nb_set_scene_v2`, `nb_set_positions`, `nb_set_filter` | Photos, their thumbnail and original paths, positions, and a visibility mask. Positions animate over 500 ms. |
| `nb_orbit`, `nb_pan`, `nb_zoom`, `nb_fit`, `nb_focus`, `nb_focus_set`, `nb_back` | Camera. Fit frames the visible photos; focus flies to one photo; back restores the previous camera (32 levels). |
| `nb_select`, `nb_hover`, `nb_pick`, `nb_pick_lod` | Selection ring and lift, hover ring after a 100 ms dwell, CPU picking against the last presented frame |
| `nb_set_results`, `nb_clear_results` | Search: matches move to the supplied positions and scale by weight; the rest recede to faint dots. 280 ms transitions that can reverse mid-way. |
| `nb_set_highlight`, `nb_set_lines` | Similarity: weighted highlight rings and constellation lines between photos |
| `nb_set_glass_panels` | Up to eight frosted-glass rectangles behind the XAML cards |
| `nb_set_pixels`, `nb_set_theme`, `nb_set_reduced_motion`, `nb_set_depth_options` | Pixel art modes (photo, 1, 4, 16, 64, 128 cells), light/dark, reduced motion, optional atmosphere and focus depth of field |
| `nb_get_view`, `nb_get_stats`, `nb_get_lod_stats`, `nb_get_focus_stats`, `nb_get_frame_diagnostics` | Read-only state and metrics. Reading never wakes the render thread. |

`nb_get_view` returns the last presented view-projection matrix, stored row-major for column vectors. In C#,
project with `Vector4.Transform(world, Matrix4x4.Transpose(view.ViewProjection))`.

## Frames and pacing

The render thread waits on the DXGI frame-latency handle while anything is moving and sleeps when nothing is.
An idle renderer presents zero frames. Up to eight worker ranges prepare each frame in parallel: animation,
projection, level of detail and a per-range pick grid. Photos are sorted by depth with a radix sort of packed
keys, drawn after a conservative depth prepass, and blended back to front with anti-aliased rounded corners.

The camera eases with a ~95 ms response. Each wheel notch is released over ~70 ms, so consecutive notches
blend into one glide. The vertical field of view is 48° and pitch is limited to ±1.35 rad.

## Level of detail

A photo's projected size decides how it is drawn:

| Size on screen | Drawn as |
|---|---|
| Smallest | A dot of its mean colour, 1.4–5 px across |
| Small | A colour mosaic (1, 2×2, 4×4, 8×8 or 16×16 cells, averaged in linear light) |
| Medium | A 64 px thumbnail |
| Large | A 128, 256 or 512 px preview |
| Selected | The original file, at full resolution or as a visible region (96 MiB budget) |

The size at which dots become tiles adapts so about 3,000 photos are tiles at any time, which keeps 50,000
photos legible without filling the screen. Tiles grow continuously from dot size to their full size, and
content crossfades over about 180 ms, so nothing pops. Requests use ±15% hysteresis and a 12-frame dwell.
Photos fade only when they are about to cross the near plane or fill the window; the selected photo is exempt.

Distance lowers contrast: full within 15 tile widths, 0.42 beyond 75, with a smooth curve between.

## Textures and the cache

| Pool | Capacity | GPU memory |
|---|---|---:|
| Colour mosaics | Every photo (1,364 bytes each) | 136 MB at 100,000 photos |
| 64 px thumbnails, BC7 with mips | 8,192 | 45 MB |
| 128 / 256 / 512 px previews, BC7 with mips | 1,024 / 512 / 128 | 112 MB |
| Selected original | One | ≤ 96 MiB |

Textures are uploaded within an 8 MiB and 1 ms budget per frame, and least-recently-used slots are replaced.

Encoded thumbnails persist in `%LOCALAPPDATA%\Nebula\Cache\Render-v<N>` as a pack file, an append-only journal
and an atomically published manifest. Entries are keyed by the source file's SHA-256 and checked by CRC before
use. A corrupt entry is decoded again from the source; a truncated journal keeps its valid prefix.

## Glass

XAML acrylic can't see the swap chain, so the renderer draws the panels' frosted glass:

1. Render the photos into an offscreen sRGB scene.
2. Downsample 8× with a full 8×8 prefilter, then blur with three separable nine-tap Gaussian passes.
3. Upsample with a cubic B-spline and lift chroma 1.35×.
4. Tint (dark `#1C1C1F`, light `#F5F5F3`), add fixed grain, a one-pixel rim and a soft exterior shadow.

The effective blur radius is about 30 DIPs at any display scale. The glass textures cost about 4.1 bytes per
pixel and are released when no panel is visible. Panel bounds and opacity come from the app on every
animation frame, so the glass moves with its XAML card.

## Errors and limits

- Each photo fails on its own: a corrupt or missing file keeps the best image already loaded (or its mean
  colour) and is counted in `nb_get_stats`.
- Scenes hold up to 100,000 photos (the smoke test runs 100,000 at full scale).
- If the GPU device is removed or reset, the renderer reports the error and stops drawing; the app must be
  restarted. Recreating the device is not implemented yet.
