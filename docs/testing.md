# Testing

| Suite | Command | Needs |
|---|---|---|
| Core checks | `dotnet run -c Release --project tests\Nebula.Core.Tests` | Nothing |
| Renderer smoke tests | `.\src\Nebula.Render\build.ps1 -SmokeTest` | A hardware Direct3D 11 GPU |
| Import validation | `dotnet run --project tools\Nebula.Import -- <folder> <minimum>` | A folder of images |
| Engine verification | `dotnet run -c Release --project tools\Nebula.Embed.Cli -- --bench --stage verify --sizes 2` | Nothing |
| UI acceptance | `.\tests\Test-UI.ps1 -LibraryJson <library.json>` | The deployed app with an indexed collection, WinApp CLI |

`.\build.ps1 -Test` runs the first two. Run them before every pull request.

## Core checks

`tests/Nebula.Core.Tests` is a console program of deterministic checks for `Nebula.Core`: the five
arrangements and their spacing at 50,000 photos, search ranking, the search volume and regathering, PCA,
PaCMAP, HDBSCAN and nearest-neighbour search. Each check prints `PASS` or `FAIL`; the program exits non-zero
if any check fails.

## Renderer smoke tests

`build.ps1 -SmokeTest` builds `NebulaRenderSmoke.exe` next to the DLL and runs it against a real hardware
Direct3D 11 device and a test implementation of the swap-chain panel interface. It checks:

- the C API: struct sizes on both architectures, argument validation, threading, scene replacement and
  cancellation, suspend and resume, and zero frames while idle;
- pixels read back from the GPU: photos, selection and hover rings, mosaics and Pixels modes, glass blur,
  rim and shadow, constellation lines, distance contrast, and the projection of a known photo to within one
  pixel;
- level of detail: continuous growth from dots to tiles, no size reversals while the wheel zooms, crossfades
  and request hysteresis;
- the BC7 cache: full mip chains, quality (PSNR), corruption detection and recovery;
- originals: all eight EXIF orientations, full-resolution and region loading, cancellation;
- scale: 5,000 and 100,000 photos within fixed CPU and GPU memory budgets, pick time and frame time
  (Release fails if fit CPU time p95 exceeds 8 ms at 100,000 photos).

GitHub-hosted runners have no GPU, so CI compiles the smoke test (`-BuildTestsOnly`) but doesn't run it.

Add `-LibraryPath <library.json>` to include a real collection that the app imported. That pass loads every
thumbnail and several originals, and checks that every photo becomes resident without errors. It never
modifies the collection or its originals.

## Import validation

`tools/Nebula.Import` imports a folder with the app's own `LibraryService` (the same source file, linked
into the tool), then checks that at least `<minimum>` images imported, that no file failed, and that every
original's SHA-256 is unchanged. The report goes to `import-validation.json` in the app's data folder.

```powershell
python -m pip install pillow
python tests\create_fixtures.py $env:TEMP\nebula-fixtures
dotnet run --project tools\Nebula.Import -- $env:TEMP\nebula-fixtures 9 --allow-file-errors
```

`create_fixtures.py` writes synthetic images with EXIF capture dates and orientations, extreme aspect
ratios, a Unicode file name, a transparent PNG, a duplicate and a corrupt file, plus `expected.json`.
`--allow-file-errors` is for fixtures like this one that contain a deliberately corrupt file.

## UI acceptance

`tests/Test-UI.ps1` drives the packaged app through UI Automation with `winapp ui`. It launches the deployed
app (or attaches with `-AppPid`), waits for the collection the app restores on start, then searches,
selects and focuses a photo, switches arrangements and Pixels modes, and checks the AI features. It records
screenshots and a video into `%LOCALAPPDATA%\Nebula\TestResults`.

Before you run it:

1. Build and deploy the app (`.\build.ps1 -Run`).
2. In the app, add the collection you want to test and wait until its photos are understood (the AI model
   pack must be installed). The app reopens this collection on the next launch.
3. Pass that collection's `library.json` and a search term with a known minimum number of matches:

```powershell
.\tests\Test-UI.ps1 -LibraryJson <library dir>\library.json -SearchTerm "portrait" -ExpectedMatches 12
```

Use a collection you can share, such as images from [The Met Open Access](https://www.metmuseum.org/about-the-met/policies-and-documents/open-access)
program (CC0). Never attach screenshots of your own photos to issues or pull requests.

## Benchmarks

`tools/Nebula.Embed.Cli --bench` measures the engine stages (nearest neighbours, PCA, PaCMAP, HDBSCAN, layout,
embedding, search, visual groups) on synthetic vectors or, with `--library`, on a collection's real
embeddings. Results go to `%LOCALAPPDATA%\Nebula\Bench`. Report timings from Release builds. See
[local-ai.md](local-ai.md#command-line-tools).
