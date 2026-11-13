#!/usr/bin/env python3
"""
Export the SigLIP 2 checkpoint (google/siglip2-so400m-patch16-384) to ONNX and build the model pack.

Outputs go to %LOCALAPPDATA%\\Nebula\\Models\\siglip2-so400m-patch16-384-onnx:
  - vision.onnx: pixel_values float32 NCHW, RGB, 384x384, values x/127.5 - 1
  - text.onnx: input_ids int64, shape [N, 64]
  - tokenizer files from Hugging Face
  - the naming vocabulary from models/vocab and its text embeddings (wide_siglip2.npy)
  - manifest.json with pinned tool versions, checkpoint revision and SHA256 hashes

The script can bootstrap its pinned Python dependencies (Python 3.12):
  python tools\\prepare_models.py --install-deps --deps-only
Then run:
  python tools\\prepare_models.py [--library-dir <library>]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
import uuid
from pathlib import Path

MODEL_ID = "google/siglip2-so400m-patch16-384"
# The checkpoint revision Nebula is validated against. Pass --revision to export another one.
MODEL_REVISION = "dd658faac399427308559e2c3ac1e99cbe43845d"
OUT_DIR = Path(os.environ.get("LOCALAPPDATA", str(Path.home() / "AppData" / "Local"))) / "Nebula" / "Models" / "siglip2-so400m-patch16-384-onnx"
PINS = {
    "torch": "2.8.0",
    "transformers": "4.56.2",
    "onnx": "1.18.0",
    "onnxruntime": "1.22.1",
    "huggingface-hub": "0.34.4",
    "pillow": "11.3.0",
    "numpy": "2.3.2",
    "safetensors": "0.6.2",
}


def install_deps() -> None:
    packages = [f"{name}=={version}" for name, version in PINS.items()]
    subprocess.check_call([sys.executable, "-m", "pip", "install", "--upgrade", *packages])


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def file_record(path: Path, root: Path) -> dict:
    return {
        "path": str(path.relative_to(root)).replace("\\", "/"),
        "bytes": path.stat().st_size,
        "sha256": sha256(path),
    }

def reference_file_records(out: Path) -> list[dict]:
    return [file_record(path, out) for path in sorted(out.rglob("*"))
            if path.is_file() and path.name not in {"manifest.json", "manifest.json.part"}
            and not any(part.startswith("text-lowmem-") for part in path.relative_to(out).parts)]


VOCAB_DIR = Path(__file__).resolve().parents[1] / "models" / "vocab"
VOCAB_SOURCES = ("ram_tags.txt", "places365.txt", "photo_types.txt")
# Must match the prompt templates in Nebula.Embed (EmbeddingIndex and VisualGroups).
TEMPLATES = ("{}", "a photo of {}.", "a picture of {}.", "a snapshot of {}.", "a photo showing {}.")


def read_words(path: Path) -> list[str]:
    return [line.strip() for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.lstrip().startswith("#")]


def encode_words(words: list[str], revision: str, batch: int = 256):
    """One unit row per word: every template through the text tower, each normalized, averaged, normalized."""
    import numpy as np
    import torch
    from transformers import AutoModel, AutoTokenizer

    model = AutoModel.from_pretrained(MODEL_ID, revision=revision, attn_implementation="eager", torch_dtype=torch.float32).eval()
    tokenizer = AutoTokenizer.from_pretrained(MODEL_ID, revision=revision)
    prompts = [t.replace("{}", w) for w in words for t in TEMPLATES]
    rows = []
    with torch.no_grad():
        for k in range(0, len(prompts), batch):
            ids = tokenizer(prompts[k:k + batch], padding="max_length", max_length=64, truncation=True, return_tensors="pt")["input_ids"]
            rows.append(model.get_text_features(input_ids=ids).numpy().astype(np.float32))
            print(f"  vocabulary prompts {min(k + batch, len(prompts))}/{len(prompts)}", flush=True)

    def l2(x):
        return x / np.clip(np.linalg.norm(x, axis=-1, keepdims=True), 1e-9, None)

    q = l2(l2(np.concatenate(rows)).reshape(len(words), len(TEMPLATES), -1).mean(1))
    return q.astype(np.float16).astype(np.float32)


def ship_vocab_pack(out: Path, revision: str) -> dict:
    """Write the naming vocabulary (models/vocab) and its text embeddings into the model pack."""
    import numpy as np

    bank = sorted(set(read_words(VOCAB_DIR / "bank.txt")))
    core = sorted(set(bank) | set(read_words(VOCAB_DIR / "core_extra.txt")))
    never_named = sorted(set(read_words(VOCAB_DIR / "never_named.txt")))
    excluded = set(read_words(VOCAB_DIR / "excluded.txt"))
    extra: set[str] = set()
    for source in VOCAB_SOURCES:
        extra.update(read_words(VOCAB_DIR / source))
    wide = sorted((set(core) | extra) - excluded)
    fingerprint = hashlib.sha256(json.dumps([wide, list(TEMPLATES), revision]).encode("utf-8")).hexdigest()[:16]
    meta = {"fingerprint": fingerprint, "model": MODEL_ID, "revision": revision, "templates": list(TEMPLATES), "words": len(wide)}

    npy, meta_path = out / "wide_siglip2.npy", out / "wide_siglip2.json"
    current = json.loads(meta_path.read_text(encoding="utf-8")) if meta_path.exists() and npy.exists() else None
    if current != meta:
        print(f"encoding {len(wide)} vocabulary words x {len(TEMPLATES)} templates", flush=True)
        np.save(npy, encode_words(wide, revision))
        meta_path.write_text(json.dumps(meta, indent=2, sort_keys=True), encoding="utf-8")
    (out / "bank_vocab.txt").write_text("\n".join(bank), encoding="utf-8")
    (out / "core_vocab.txt").write_text("\n".join(core), encoding="utf-8")
    (out / "wide_vocab.txt").write_text("\n".join(wide), encoding="utf-8")
    (out / "never_named.txt").write_text("\n".join(never_named), encoding="utf-8")
    return {"coreWords": len(core), "bankWords": len(bank), "wideWords": len(wide), "neverNamed": len(never_named), "wideMeta": meta}


def cos(a, b) -> float:
    import numpy as np

    a = np.asarray(a, dtype=np.float64).reshape(-1)
    b = np.asarray(b, dtype=np.float64).reshape(-1)
    return float(a @ b / max((np.linalg.norm(a) * np.linalg.norm(b)), 1e-12))


def preprocess(path: Path):
    import numpy as np
    from PIL import Image, ImageOps

    img = ImageOps.exif_transpose(Image.open(path)).convert("RGB").resize((384, 384), Image.Resampling.BILINEAR)
    arr = np.asarray(img, dtype=np.float32) / 127.5 - 1.0
    return np.transpose(arr, (2, 0, 1))[None, ...]


def export_models(out: Path, revision: str | None, opset: int) -> dict:
    import numpy as np
    import onnx
    import onnxruntime as ort
    import torch
    from huggingface_hub import HfApi
    from transformers import AutoModel, AutoTokenizer

    out.mkdir(parents=True, exist_ok=True)
    resolved = HfApi().model_info(MODEL_ID, revision=revision).sha
    print(f"loading {MODEL_ID}@{resolved}", flush=True)
    model = AutoModel.from_pretrained(MODEL_ID, revision=resolved, attn_implementation="eager", torch_dtype=torch.float32).eval()
    tokenizer = AutoTokenizer.from_pretrained(MODEL_ID, revision=resolved)
    tokenizer.save_pretrained(out)

    class Vision(torch.nn.Module):
        def __init__(self, m):
            super().__init__()
            self.m = m

        def forward(self, pixel_values):
            return self.m.get_image_features(pixel_values=pixel_values)

    class Text(torch.nn.Module):
        def __init__(self, m):
            super().__init__()
            self.m = m

        def forward(self, input_ids):
            return self.m.get_text_features(input_ids=input_ids)

    vision_path = out / "vision.onnx"
    text_path = out / "text.onnx"
    with torch.no_grad():
        torch.onnx.export(
            Vision(model),
            torch.zeros(1, 3, 384, 384, dtype=torch.float32),
            str(vision_path),
            input_names=["pixel_values"],
            output_names=["embedding"],
            dynamic_axes={"pixel_values": {0: "batch"}, "embedding": {0: "batch"}},
            opset_version=opset,
            external_data=True,
        )
        torch.onnx.export(
            Text(model),
            torch.zeros(1, 64, dtype=torch.long),
            str(text_path),
            input_names=["input_ids"],
            output_names=["embedding"],
            dynamic_axes={"input_ids": {0: "batch"}, "embedding": {0: "batch"}},
            opset_version=opset,
            external_data=True,
        )
    onnx.checker.check_model(str(vision_path))
    onnx.checker.check_model(str(text_path))

    so = ort.SessionOptions()
    vision = ort.InferenceSession(str(vision_path), so, providers=["CPUExecutionProvider"])
    text = ort.InferenceSession(str(text_path), so, providers=["CPUExecutionProvider"])
    rng = np.random.default_rng(1)
    image_input = rng.random((2, 3, 384, 384), dtype=np.float32) * 2 - 1
    ids = tokenizer(["a photo of a dog", "egyptian hieroglyphs"], padding="max_length", max_length=64, truncation=True, return_tensors="np")["input_ids"].astype(np.int64)
    with torch.no_grad():
        torch_img = model.get_image_features(pixel_values=torch.from_numpy(image_input)).numpy()
        torch_txt = model.get_text_features(input_ids=torch.from_numpy(ids)).numpy()
    onnx_img = vision.run(None, {"pixel_values": image_input})[0]
    onnx_txt = text.run(None, {"input_ids": ids})[0]
    return {
        "revision": resolved,
        "dimension": int(torch_img.shape[-1]),
        "padTokenId": int(tokenizer.pad_token_id or 0),
        "exportParity": {
            "imageCosineMin": min(cos(torch_img[i], onnx_img[i]) for i in range(torch_img.shape[0])),
            "textCosineMin": min(cos(torch_txt[i], onnx_txt[i]) for i in range(torch_txt.shape[0])),
        },
    }


def library_parity(out: Path, library_dir: Path, max_images: int) -> dict:
    import numpy as np
    import onnxruntime as ort
    import torch
    from transformers import AutoModel, AutoTokenizer

    library = json.loads((library_dir / "library.json").read_text(encoding="utf-8"))
    photos = library["photos" if "photos" in library else "Photos"][:max_images]
    model = AutoModel.from_pretrained(MODEL_ID, revision=json.loads((out / "manifest.json").read_text())["revision"], attn_implementation="eager", torch_dtype=torch.float32).eval()
    tokenizer = AutoTokenizer.from_pretrained(out)
    vision = ort.InferenceSession(str(out / "vision.onnx"), providers=["CPUExecutionProvider"])
    text = ort.InferenceSession(str(out / "text.onnx"), providers=["CPUExecutionProvider"])
    image_cos = []
    for p in photos:
        path = Path(p.get("ThumbnailPath") or p.get("thumbnailPath") or p.get("OriginalPath") or p.get("originalPath"))
        x = preprocess(path)
        with torch.no_grad():
            ref = model.get_image_features(pixel_values=torch.from_numpy(x)).numpy()[0]
        got = vision.run(None, {"pixel_values": x})[0][0]
        image_cos.append(cos(ref, got))
    queries = [
        "a blue and white porcelain plate", "armor", "a portrait of a woman", "egyptian hieroglyphs", "a horse",
        "a photo of a dog", "cachorro na praia ao pôr do sol", "receipt from a restaurant", "gold jewelry", "a landscape",
    ]
    ids = tokenizer(queries, padding="max_length", max_length=64, truncation=True, return_tensors="np")["input_ids"].astype(np.int64)
    with torch.no_grad():
        ref_text = model.get_text_features(input_ids=torch.from_numpy(ids)).numpy()
    got_text = text.run(None, {"input_ids": ids})[0]
    text_cos = [cos(ref_text[i], got_text[i]) for i in range(len(queries))]
    return {
        "reference": "PyTorch transformers fp32 from original checkpoint",
        "images": len(image_cos),
        "imageCosineMin": float(np.min(image_cos)) if image_cos else None,
        "imageCosineMean": float(np.mean(image_cos)) if image_cos else None,
        "textQueries": len(text_cos),
        "textCosineMin": float(np.min(text_cos)),
        "textCosineMean": float(np.mean(text_cos)),
    }

def register_compact_text(out: Path, destination: Path, receipt: dict, manifest: dict) -> dict:
    receipt["files"] = [entry for entry in receipt["files"] if not entry["path"].endswith("/initializers.json")]
    receipt.pop("initializerFile", None)
    receipt.pop("weightsFile", None)
    receipt["id"] = hashlib.sha256("".join(f"{x['path']}:{x['sha256']}" for x in receipt["files"]).encode()).hexdigest()
    previous = manifest.get("compactText", {})
    if previous.get("id") == receipt["id"]:
        receipt["enabled"] = previous.get("enabled", False)
        if "parity" in previous:
            receipt["parity"] = previous["parity"]
        receipt["idleReleaseSeconds"] = previous.get("idleReleaseSeconds", 0)
        receipt["verifiedReloadMs"] = previous.get("verifiedReloadMs", 0)
    else:
        receipt["enabled"] = False
        receipt.pop("parity", None)
    (destination / "variant.json").write_text(json.dumps(receipt, indent=2, sort_keys=True), encoding="utf-8")
    manifest["compactText"] = receipt
    prefix = destination.name + "/"
    manifest["files"] = [x for x in manifest["files"] if not x["path"].startswith(prefix)] + receipt["files"]
    temporary = out / "manifest.json.part"
    temporary.write_text(json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8")
    temporary.replace(out / "manifest.json")
    return receipt


def export_compact_text(out: Path) -> dict:
    """Keep the fp32 reference intact; remove its vocabulary Gather before fp16 conversion."""
    import numpy as np
    import onnx
    from onnxruntime.transformers.float16 import convert_float_to_float16

    manifest_path = out / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    source = out / manifest["textFile"]
    source_hash = sha256(source)
    directory_name = "text-lowmem-v1-" + manifest["modelHash"][:12]
    destination = out / directory_name
    if destination.exists():
        receipt = json.loads((destination / "variant.json").read_text(encoding="utf-8"))
        for item in receipt["files"]:
            if sha256(out / item["path"]) != item["sha256"]:
                raise RuntimeError(f"Existing compact model is corrupt: {item['path']}")
        return register_compact_text(out, destination, receipt, manifest)
    staging = out / (directory_name + ".part-" + uuid.uuid4().hex)
    staging.mkdir(parents=True)
    model = onnx.load(str(source), load_external_data=False)
    tables = [t for t in model.graph.initializer if t.name.endswith("token_embedding.weight")]
    if len(tables) != 1 or len(tables[0].dims) != 2:
        raise RuntimeError("Expected exactly one rank-2 SigLIP token embedding table.")
    table = tables[0]
    rows, columns = map(int, table.dims)
    users = [node for node in model.graph.node if table.name in node.input]
    if len(users) != 1 or users[0].op_type != "Gather":
        raise RuntimeError("Token table is not exclusively consumed by the expected Gather.")
    gather = users[0]
    metadata = {entry.key: entry.value for entry in table.external_data}
    if table.data_type != onnx.TensorProto.FLOAT:
        raise RuntimeError("The reference token table must be float32.")
    if metadata:
        table_data = np.memmap(source.parent / metadata["location"], mode="r", dtype="<f4",
                               offset=int(metadata.get("offset", 0)), shape=(rows, columns))
    else:
        table_data = onnx.numpy_helper.to_array(table)
    with (staging / "tokens.f16").open("wb") as file:
        for start in range(0, rows, 4096):
            values = np.asarray(table_data[start:start + 4096])
            if not np.isfinite(values).all() or np.abs(values).max() > 65504:
                raise RuntimeError("Token table cannot be represented as finite float16.")
            file.write(values.astype("<f2").tobytes())
    del table_data
    old_output = gather.output[0]
    for node in model.graph.node:
        for i, name in enumerate(node.input):
            if name == old_output:
                node.input[i] = "token_embeddings"
    model.graph.node.remove(gather)
    model.graph.initializer.remove(table)
    for info in list(model.graph.value_info):
        if info.name == old_output:
            model.graph.value_info.remove(info)
    model.graph.input.append(onnx.helper.make_tensor_value_info(
        "token_embeddings", onnx.TensorProto.FLOAT, ["batch", 64, columns]))
    for tensor in model.graph.initializer:
        if tensor.data_location == onnx.TensorProto.EXTERNAL:
            onnx.external_data_helper.load_external_data_for_tensor(tensor, str(source.parent))
            tensor.ClearField("external_data")
            tensor.data_location = onnx.TensorProto.DEFAULT
    # Float32 public IO, fp16 internal weights/activations. Existing shape metadata suffices;
    # protobuf shape inference on a multi-GB external-data reference would exceed its size limit.
    compact = convert_float_to_float16(model, keep_io_types=True, disable_shape_infer=True)
    del model
    available = {value.name for value in compact.graph.input} | {value.name for value in compact.graph.initializer}
    pending, ordered = list(compact.graph.node), []
    while pending:
        remaining = []
        for node in pending:
            if all(not name or name in available for name in node.input):
                ordered.append(node)
                available.update(node.output)
            else:
                remaining.append(node)
        if len(remaining) == len(pending):
            raise RuntimeError("Compact graph has unresolved or cyclic inputs.")
        pending = remaining
    del compact.graph.node[:]
    compact.graph.node.extend(ordered)
    onnx.save_model(compact, str(staging / "text.onnx"), save_as_external_data=True,
                    all_tensors_to_one_file=True, location="weights.bin", size_threshold=1024)
    del compact
    onnx.checker.check_model(str(staging / "text.onnx"))
    files = []
    for path in sorted(staging.iterdir()):
        item = file_record(path, staging)
        item["path"] = directory_name + "/" + item["path"]
        files.append(item)
    fingerprint = hashlib.sha256("".join(f"{x['path']}:{x['sha256']}" for x in files).encode()).hexdigest()
    receipt = {
        "id": fingerprint, "sourceModelHash": manifest["modelHash"], "sourceGraphSha256": source_hash,
        "file": directory_name + "/text.onnx", "tokenFile": directory_name + "/tokens.f16",
        "tokenRows": rows, "tokenColumns": columns, "embeddingInput": "token_embeddings",
        "tokenType": "float16", "inputType": "float32", "outputType": "float32", "enabled": False,
        "files": files,
    }
    (staging / "variant.json").write_text(json.dumps(receipt, indent=2, sort_keys=True), encoding="utf-8")
    staging.rename(destination)
    manifest["compactText"] = receipt
    # Image embeddings still describe the same fp32 reference checkpoint. Adding a text
    # execution variant must not invalidate 50K cached image embeddings.
    manifest["files"] = [x for x in manifest["files"] if not x["path"].startswith(directory_name + "/")] + files
    temporary = manifest_path.with_suffix(".json.part")
    temporary.write_text(json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8")
    temporary.replace(manifest_path)
    return register_compact_text(out, destination, receipt, manifest)


def approve_compact_text(out: Path, benchmark: Path) -> dict:
    manifest_path = out / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    variant = manifest["compactText"]
    report_path = benchmark / "report.json"
    report = json.loads(report_path.read_text(encoding="utf-8"))
    parity = report.get("parity") or {}
    if (report.get("textVariantId") != variant["id"] or report.get("ModelHash") != manifest["modelHash"]
            or report.get("Count", 0) < 50000 or parity.get("queries", 0) < 200
            or parity.get("rankingQueries", 0) < 50 or parity.get("minimumCosine", 0) < .999
            or parity.get("minimumTop20Overlap", 0) < .95):
        raise RuntimeError("Compact text activation requires matching 50K/200-text/50-ranking parity evidence.")
    for item in variant["files"]:
        if sha256(out / item["path"]) != item["sha256"]:
            raise RuntimeError(f"Compact model hash mismatch: {item['path']}")
    variant["enabled"] = True
    reloads = report.get("reloadSamples", [])
    variant["verifiedReloadMs"] = max(reloads) if reloads else 0
    variant["idleReleaseSeconds"] = 120 if len(reloads) >= 3 and max(reloads) < 1500 else 0
    variant["parity"] = {
        "receiptSha256": sha256(report_path), "queries": parity["queries"],
        "rankingQueries": parity["rankingQueries"], "minimumCosine": parity["minimumCosine"],
        "minimumTop20Overlap": parity["minimumTop20Overlap"],
    }
    temporary = manifest_path.with_suffix(".json.part")
    temporary.write_text(json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8")
    temporary.replace(manifest_path)
    return variant


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=OUT_DIR)
    parser.add_argument("--revision", default=MODEL_REVISION, help="Hugging Face revision of the checkpoint (default: the validated revision).")
    parser.add_argument("--opset", type=int, default=17)
    parser.add_argument("--install-deps", action="store_true")
    parser.add_argument("--deps-only", action="store_true")
    parser.add_argument("--vocab-only", action="store_true")
    parser.add_argument("--library-dir", type=Path)
    parser.add_argument("--parity-images", type=int, default=50)
    parser.add_argument("--compact-text", action="store_true", help="Build an opt-in fp16 text trunk plus mmap token table from the existing fp32 pack.")
    parser.add_argument("--approve-compact-text", type=Path, help="Enable the compact variant after a passing --memory-bench parity report.")
    args = parser.parse_args()
    if args.install_deps:
        install_deps()
        if args.deps_only:
            return 0
    if args.compact_text:
        print(json.dumps(export_compact_text(args.out), indent=2))
        return 0
    if args.approve_compact_text:
        print(json.dumps(approve_compact_text(args.out, args.approve_compact_text), indent=2))
        return 0
    if args.vocab_only:
        args.out.mkdir(parents=True, exist_ok=True)
        manifest_path = args.out / "manifest.json"
        revision = json.loads(manifest_path.read_text(encoding="utf-8"))["revision"] if manifest_path.exists() else None
        if revision is None:
            from huggingface_hub import HfApi
            revision = HfApi().model_info(MODEL_ID, revision=args.revision).sha
        vocab_meta = ship_vocab_pack(args.out, revision)
        if manifest_path.exists():
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["vocabulary"] = vocab_meta
            reference = reference_file_records(args.out)
            manifest["files"] = reference + manifest.get("compactText", {}).get("files", [])
            manifest["modelHash"] = hashlib.sha256("".join(f"{f['path']}:{f['sha256']}" for f in reference).encode("utf-8")).hexdigest()
            manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8")
        print(json.dumps(vocab_meta, indent=2))
        return 0
    t0 = time.time()
    meta = export_models(args.out, args.revision, args.opset)
    vocab_meta = ship_vocab_pack(args.out, meta["revision"])
    files = reference_file_records(args.out)
    manifest = {
        "modelId": MODEL_ID,
        "revision": meta["revision"],
        "createdUtc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "toolPins": PINS,
        "opset": args.opset,
        "visionFile": "vision.onnx",
        "textFile": "text.onnx",
        "dimensions": meta["dimension"],
        "padTokenId": meta["padTokenId"],
        "imageSize": 384,
        "preprocessing": "RGB 384x384, Pillow/Image.Resampling.BILINEAR, float32 x/127.5 - 1, NCHW",
        "text": "SigLIP tokenizer, max_length=64, truncation, padding=max_length with <pad>, 5 prompt templates applied by the app",
        "exportParity": meta["exportParity"],
        "vocabulary": vocab_meta,
        "files": files,
        "modelHash": hashlib.sha256("".join(f"{f['path']}:{f['sha256']}" for f in files).encode("utf-8")).hexdigest(),
        "elapsedSeconds": round(time.time() - t0, 3),
    }
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8")
    if args.library_dir:
        parity = library_parity(args.out, args.library_dir, args.parity_images)
        manifest = json.loads((args.out / "manifest.json").read_text(encoding="utf-8"))
        manifest["parity"] = parity
        (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8")
    print(json.dumps(json.loads((args.out / "manifest.json").read_text(encoding="utf-8")), indent=2)[:4000])
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
