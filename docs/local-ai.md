# Local AI

Nebula's AI features run entirely on your PC. They use one model: Google's
[SigLIP 2](https://huggingface.co/google/siglip2-so400m-patch16-384) (so400m, patch 16, 384 px), an image
and text encoder that places photos and phrases in the same 1,152-dimensional space. ONNX Runtime runs it
on the GPU through DirectML, and on the CPU when no suitable GPU is available.

The model is optional. Without it, Nebula imports, arranges and searches by metadata, and the Similarity
arrangement is unavailable.

## Prepare the model pack

Nebula does not ship the model weights. `tools/prepare_models.py` downloads the checkpoint from Hugging Face,
exports the image and text towers to ONNX, checks the export against PyTorch, and writes the pack:

```powershell
py -3.12 -m venv .venv
.venv\Scripts\python tools\prepare_models.py --install-deps --deps-only   # pinned PyTorch, Transformers, ONNX...
.venv\Scripts\python tools\prepare_models.py
```

The pack goes to `%LOCALAPPDATA%\Nebula\Models\siglip2-so400m-patch16-384-onnx` and takes about 6 GB. It
contains:

| File | Contents |
|---|---|
| `vision.onnx` + external weights | Image tower. Input: float32 NCHW RGB, 384×384, values `x / 127.5 − 1`. |
| `text.onnx` + external weights | Text tower. Input: int64 token ids, 64 per prompt. |
| `tokenizer.json` and related files | The SigLIP 2 tokenizer |
| `bank_vocab.txt`, `core_vocab.txt`, `wide_vocab.txt`, `never_named.txt` | Words for naming visual groups, built from [`models/vocab`](../models/vocab/NOTICE.md) |
| `wide_siglip2.npy`, `wide_siglip2.json` | The text embedding of every naming word, averaged over five prompt templates |
| `manifest.json` | Checkpoint id and revision, tool versions, export parity, and the SHA-256 of every file |

The default checkpoint revision is pinned in the script (`--revision` selects another). After editing the
word lists, run `prepare_models.py --vocab-only` to rebuild only the vocabulary files.

The `modelHash` in the manifest identifies the pack. Embeddings record the hash they were made with, and a
different hash makes Nebula embed the collection again.

## Understanding a collection

When a collection opens, `EmbeddingIndex` reconciles its embeddings with the photos by id, size and
modification time. Unchanged photos are reused; new or changed photos are embedded in the background while
you use the app, and the status line shows progress. Embeddings are stored next to `library.json` as a
float32 matrix (`embeddings.bin`, one L2-normalized row per photo) with a JSON sidecar.

The C# preprocessor reproduces the export's preprocessing: a Pillow-compatible bilinear resize to 384×384
and the same normalization. The vision tower is released as soon as embedding finishes; search only needs
the text tower.

## Search

A query becomes several prompts ("{query}", "a photo of {query}.", and so on), each encoded by the text
tower and averaged. Photos are ranked by their similarity to the query after centering on the collection's
mean, with SigLIP's sigmoid as a relevance gate. Title, artist and department matches the model missed are
appended, so an exact label is never lost.

In the Cloud arrangement the matches form a search volume: their first two principal components set X and
Y, so photos that look alike sit together, and match strength brings the best results nearest the camera.
Other arrangements keep their own shape and show only the matches.

Repeated queries are served from a 512-prompt cache. The text session is released after two minutes idle
when the pack has been validated for fast reloads (see Memory below).

## Similarity

The Similarity arrangement places every photo by what it looks like:

1. Center the embeddings on the collection mean.
2. Reduce to 100 dimensions with PCA (full covariance, double-precision eigensolver).
3. Project to 3D with [PaCMAP](https://jmlr.org/papers/v22/20-1061.html) (10 neighbours, MN ratio 0.3,
   FP ratio 4.0, seed 42).
4. Normalize to the 99th percentile, scale so the median neighbour distance is 1.5 tiles, and relax overlaps.

The result is cached as `similarity.bin` and reused until the photos or the model change.

## Visual groups

In Similarity, the Explore panel lists groups of photos that look alike, gathered into categories.
`VisualGroups.LoadOrBuildAsync`:

1. Reduces the centered embeddings to 10 dimensions (PCA, then PaCMAP).
2. Finds clusters with HDBSCAN (minimum cluster size 10, minimum samples 2), using an exact kd-tree Borůvka
   minimum spanning tree.
3. Joins ungrouped photos to a nearby group when they are close enough, and finds links and bridge photos
   between groups.
4. Cuts the groups into categories with Ward clustering.
5. Names each group. Every photo gets a short list of candidate words from the wide vocabulary that it
   matches unusually well. A group is named by the word most of its photos share, preferring words that
   are rare in the rest of the collection. Failing that, it is named by a family of related words (food,
   drinks, water, people, documents, screenshots, building work). Groups with no shared word stay
   unnamed and show their strongest words instead. Words in `never_named.txt` are never names. The core
   vocabulary supplies each group's top tags.

The result is cached as `groups.json`.

## Runtime

In the packaged app, ONNX Runtime and DirectML come from Windows ML in the Windows App SDK, so the package
contains a single `onnxruntime.dll`. `Nebula.Embed.Cli` carries its own ONNX Runtime for headless runs.
Nebula opts out of ONNX Runtime's telemetry events before it creates a session.

Environment variables for experiments:

| Variable | Effect |
|---|---|
| `NEB_EMBED_BATCH` | Embedding batch size (default 1, the fastest measured on DirectML) |
| `NEB_TEXT_VARIANT` | `fp32` forces the reference text tower; `compact` tests the compact variant; `auto` (default) uses the compact variant only if the pack approved it |

## Memory

The image matrix is memory-mapped read-only rather than copied, and only its mean and row norms stay
resident. An optional compact text tower keeps the 256,000 × 1,152 token table as an fp16 memory map on the
CPU and runs the rest in fp16 on DirectML. It is built and approved in steps, each checked against the fp32
reference:

```powershell
$py = ".venv\Scripts\python"
$bench = "$env:LOCALAPPDATA\Nebula\Bench\memory"
& $py tools\prepare_models.py --compact-text
dotnet run -c Release --project tools\Nebula.Embed.Cli -- --memory-bench --library <library dir> --text-mode fp32 --out "$bench\reference"
dotnet run -c Release --project tools\Nebula.Embed.Cli -- --memory-bench --library <library dir> --text-mode compact --out "$bench\validation" --reference "$bench\reference" --reload true --reload-count 3
& $py tools\prepare_models.py --approve-compact-text "$bench\validation"
```

Approval requires a minimum cosine similarity to the fp32 tower and top-20 result overlap. A separate gate
enables the two-minute idle release of the text session: three cold reloads, all under 1.5 seconds,
recorded in the validation run (`--reload true --reload-count 3`).

Measured on a 50,000-photo collection (Release, Arm64): the compact text tower kept a minimum cosine of
0.9999 to fp32 over 200 queries and a mean top-20 overlap of 99.6%. After the idle release, the engine held
0.6 GB of private memory and no dedicated GPU memory.

## Command-line tools

```powershell
# Embed a collection the app has imported (and optionally build its visual groups)
dotnet run -c Release --project tools\Nebula.Embed.Cli -- <library dir> [--groups] [--query "a red boat"]

# Engine benchmarks on synthetic vectors, or on a collection's real embeddings with --library
dotnet run -c Release --project tools\Nebula.Embed.Cli -- --bench --stage algorithms --sizes 5000,50000
```

`<library dir>` is a folder under `Nebula\Data\Libraries` (see
[architecture.md](architecture.md#local-data)). Benchmarks write to `%LOCALAPPDATA%\Nebula\Bench` and never
modify the collection. Stages: `algorithms`, `knn`, `layout`, `pca`, `p10`, `hdb`, `groups`, `embed`,
`search`, `verify`, `resume` and `layouts`.

## Privacy

Photos, embeddings, queries and names never leave the PC. Nebula's code makes no network requests.
`prepare_models.py` contacts Hugging Face and the Python package index to download the model and its tools.
