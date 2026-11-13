# Architecture

Nebula is one MSIX-packaged WinUI 3 app. A C++ Direct3D 11 renderer draws the photos; C# libraries
compute layouts and run the image model. There is no server and no background service.

```
┌──────────────────────────── Nebula.Windows (WinUI 3, C#) ────────────────────────────┐
│ MainWindow ─ MainPage (XAML chrome: sidebar, Explore, toolbar, photo card)            │
│   Services: LibraryService (import, thumbnails, library.json)                         │
│             NativeRenderer (P/Invoke to NebulaRender.dll)                             │
│             RawTouchpad (precision-touchpad orbit through Raw Input)                  │
│             PanelTransition, UiState                                                  │
└──────────┬──────────────────────────────┬────────────────────────────┬────────────────┘
           │                              │                            │
   Nebula.Core (C#)               Nebula.Embed (C#)             NebulaRender.dll (C++)
   photo model, layouts,          SigLIP 2 on Windows ML:       D3D11 swap chain in the
   PaCMAP, HDBSCAN,               embeddings, semantic          SwapChainPanel, LOD, glass,
   search volume                  search, visual groups         decode workers, BC7 cache
```

## Projects

| Project | What it does |
|---|---|
| `src/Nebula.Windows` | The app. `MainPage.xaml` is the whole interface: a full-window `SwapChainPanel` with floating glass cards. `MainPage.Semantic.cs` connects the AI layer and `MainPage.Similarity.cs` the visual-groups view. |
| `src/Nebula.Render` | `NebulaRender.dll`, a C API over a Direct3D 11 renderer. See [renderer.md](renderer.md). |
| `src/Nebula.Core` | Platform-independent .NET: the `Photo` and `Library` records, metadata search, the Cloud, Colour, Time and Grid layouts, the Similarity layout (PCA + PaCMAP), HDBSCAN and the search volume. |
| `src/Nebula.Embed` | The image model through ONNX Runtime: `EmbeddingIndex` (embedding, search, nearest photos) and `VisualGroups` (groups, categories and their names). See [local-ai.md](local-ai.md). |
| `tools/Nebula.Embed.Cli` | Builds an embedding index without the app, and runs the engine and memory benchmarks. |
| `tools/Nebula.Import` | Imports a folder without the app, using the app's own `LibraryService`, and checks that originals are unchanged. |
| `tests/Nebula.Core.Tests` | Deterministic checks for the layouts and algorithms. See [testing.md](testing.md). |

## How a collection opens

1. **Import.** `LibraryService.ImportAsync` walks the folder, skips cloud-only files without downloading them,
   decodes each image with `Windows.Graphics.Imaging`, applies orientation and colour management, and writes
   a 256-pixel thumbnail. It records size, capture time, mean colour (OKLab) and metadata in `library.json`.
   Identical files are imported once. A later import reuses every thumbnail whose source size and
   modification time are unchanged.
2. **Layout.** `Nebula.Core.Layout.Positions` computes the chosen arrangement on a worker thread. Results
   are cached per collection and arrangement.
3. **Scene.** The page sends one record per photo (id, position, thumbnail and original paths) to the
   renderer with `nb_set_scene_v2`. The renderer decodes thumbnails on its own workers, builds colour
   mosaics and BC7-compressed mip levels, and caches them on disk for the next launch.
4. **Understanding.** If the model pack is installed, `EmbeddingIndex` loads existing embeddings or embeds
   new photos in the background. Search then becomes semantic and Similarity becomes available.

Changing arrangement sends new positions; the renderer animates every photo to its new place over 500 ms.

## Threads

| Thread | Work |
|---|---|
| UI thread | XAML, input, `nb_create`/`nb_destroy`, small renderer commands |
| .NET thread pool | Import, layouts, embedding, search, visual groups. Embedding, search and group builds take turns through one gate, and a superseded index is disposed only while that gate is held. |
| Render thread (native) | Frame preparation, level of detail, drawing and presenting. It sleeps when nothing changes. |
| Decode workers (native) | Thumbnail and original decoding (WIC), mosaics, BC7 compression, cache writes |

Renderer calls copy their arguments and return at once; the render thread applies them on its next frame.
Work started for a superseded collection, query or arrangement is cancelled, and its results are dropped.

## Glass over the 3D scene

XAML acrylic can't sample a `SwapChainPanel`, so the renderer draws the frosted glass itself. The page sends
the bounds, corner radius, tint and opacity of each card (`nb_set_glass_panels`), and the renderer blurs the
photos behind those rectangles. The XAML cards above them are transparent, so text and controls sit on the
renderer's glass. Card animations drive the XAML transform and the native rectangle from one clock
(`PanelTransition`), so content and glass move together.

## Input

Precision touchpads report two-finger scrolling to WinUI as mouse wheel events. `RawTouchpad` reads the
touchpad's contacts through Raw Input (without intercepting them), so two-finger movement orbits freely in
both axes with momentum. Pinch, Ctrl+wheel and mouse-wheel notches zoom. Without Raw Input data, scroll
deltas that are sideways or not multiples of 120 count as touchpad scrolling.

## UI strings

The app's text lives in `src/Nebula.Windows/Strings/en-US/Resources.resw`. XAML elements take theirs through
`x:Uid` (for example `SearchBox.PlaceholderText`, or
`AddFolderButton.[using:Microsoft.UI.Xaml.Automation]AutomationProperties.Name` for an attached property).
Code uses `Strings.Get("Key")` and `Strings.Format("Key", args)` (`Services/Strings.cs`), which format with
the current culture. To add a language, add `Strings/<language>/Resources.resw` with the same keys.

Some text is deliberately not in the resource file, because code also uses it as data:

- Facet labels that are also their grouping keys (department and colour names, Other, Undated, Square,
  Landscape, Portrait) and the arrangement names, which come from the `Arrangement` enum.
- Group names and the "Mixed" fallback, which come from the vocabulary and the visual-group cache.
- Progress text from `Nebula.Embed` (the app parses parts of it) and import messages from `LibraryService`,
  which the command-line tools share.
- The query "a photo" used to warm up the text model, and all log messages.

Translating those needs localized display names mapped from invariant keys.

## Local data

Nebula reads originals and never writes to them. Everything it generates goes under one folder in the
user's local app data, `Nebula`. Windows redirects a packaged app's new files to the package's own
folder, so on most PCs the app's data is at:

```
%LOCALAPPDATA%\Packages\<package family name>\LocalCache\Local\
```

If the folder `%LOCALAPPDATA%\Nebula\Data` already exists (for example because the command-line tools
created it), the app uses that folder directly.

| Path | Contents |
|---|---|
| `Nebula\Data\Libraries\<id>\library.json` | The imported collection. `<id>` is derived from the folder path. |
| `Nebula\Data\Libraries\<id>\thumbs\` | 256-pixel thumbnails |
| `Nebula\Data\Libraries\<id>\embeddings.bin`, `embeddings.json` | Image embeddings and their photo ids |
| `Nebula\Data\Libraries\<id>\similarity.bin`, `groups.json`, `*.npy`, `calib.json` | Similarity layout and visual-group caches |
| `Nebula\Data\preferences.json` | The last opened collection |
| `Nebula\Data\application.log` | Diagnostics (rotated at 8 MB). Contains file paths. |
| `Nebula\Cache\Render-v<N>\` | The renderer's thumbnail and mip cache |
| `Nebula\Models\siglip2-so400m-patch16-384-onnx\` | The AI model pack, written by `tools/prepare_models.py` |
| `Nebula\Data\ui-state.json` | Appearance, panel visibility and favourites |

Uninstalling the package removes the redirected folder. Deleting a library folder makes the app import that
collection again.
