# Third-party notices

Nebula's own source code is licensed under the Apache License 2.0 (see [LICENSE](LICENSE)). It uses
or redistributes the components below, each under its own license.

## Source code in this repository

| Component | Location | License |
|---|---|---|
| [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) by Richard Geldreich, Jr. (commit `b9438627eef73a1157e84201b6fa6eb2ffd6d9f0`, `bc7enc.cpp`/`.h` only, unmodified) | `src/Nebula.Render/vendor/bc7enc` | MIT (or public domain), [`LICENSE`](src/Nebula.Render/vendor/bc7enc/LICENSE). Nebula uses the MIT option. The app package includes this license as `LICENSE-bc7enc.txt`. |

## Data in this repository

| Component | Location | License |
|---|---|---|
| Recognize Anything Model tag list | `models/vocab/ram_tags.txt` | Apache License 2.0, [`LICENSE-ram-apache-2.0.txt`](models/vocab/LICENSE-ram-apache-2.0.txt) |
| Places365 scene categories | `models/vocab/places365.txt` | MIT, [`LICENSE-places365-mit.txt`](models/vocab/LICENSE-places365-mit.txt) |

See [`models/vocab/NOTICE.md`](models/vocab/NOTICE.md) for how each list was derived.

## NuGet packages built into the app and tools

| Package | License |
|---|---|
| [Windows App SDK](https://github.com/microsoft/WindowsAppSDK) (`Microsoft.WindowsAppSDK`, including Windows ML) | [Microsoft Software License Terms](https://www.nuget.org/packages/Microsoft.WindowsAppSDK/2.5.1/License) |
| [Windows SDK Build Tools](https://www.nuget.org/packages/Microsoft.Windows.SDK.BuildTools) (`Microsoft.Windows.SDK.BuildTools`, `.WinApp`, `.WinUIAnalyzer`) | Microsoft Software License Terms (see each package) |
| [ONNX Runtime](https://github.com/microsoft/onnxruntime) with DirectML (`Microsoft.ML.OnnxRuntime.DirectML`) | MIT, with the package's `ThirdPartyNotices.txt` |
| [CommunityToolkit.Mvvm](https://github.com/CommunityToolkit/dotnet) | MIT |
| [System.Drawing.Common](https://github.com/dotnet/winforms) | MIT |
| [Tokenizers.DotNet](https://github.com/sappho192/Tokenizers.DotNet) and its native runtime packages, which wrap Hugging Face [tokenizers](https://github.com/huggingface/tokenizers) | MIT; tokenizers is Apache License 2.0 |

In the packaged app, ONNX Runtime and DirectML come from Windows ML in the Windows App SDK. The
standalone `Nebula.Embed.Cli` tool carries its own ONNX Runtime copy.

## Model weights (downloaded, not included)

`tools/prepare_models.py` downloads [SigLIP 2 so400m-patch16-384](https://huggingface.co/google/siglip2-so400m-patch16-384)
by Google from Hugging Face, converts it to ONNX and writes the model pack to your PC. The weights and
tokenizer are licensed under the Apache License 2.0. Nebula does not redistribute them.

## Python tools (not part of the app)

`tools/prepare_models.py` installs pinned versions of PyTorch (BSD-3-Clause), Transformers (Apache
2.0), ONNX (Apache 2.0), ONNX Runtime (MIT), Hugging Face Hub (Apache 2.0), safetensors (Apache 2.0),
NumPy (BSD-3-Clause) and Pillow (MIT-CMU). `tools/make_icons.py` uses Pillow.
