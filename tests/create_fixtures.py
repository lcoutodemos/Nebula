"""Create small synthetic import fixtures, never counted as museum photographs."""
import argparse
import json
from pathlib import Path

from PIL import Image, ImageDraw


def create(output):
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError("Use a new, empty fixture directory; existing files are never overwritten")
    expected = []
    for year, colour in ((2019, "#b45540"), (2021, "#406fa8"), (2024, "#6c8750")):
        image = Image.new("RGB", (720, 480), colour)
        draw = ImageDraw.Draw(image)
        draw.rectangle((20, 20, 200, 120), fill="white")
        draw.text((35, 45), f"FIXTURE {year} / TOP LEFT", fill="black")
        exif = Image.Exif()
        exif[274] = 1
        exif[306] = f"{year}:07:15 12:30:00"
        exif[34665] = {36867: f"{year}:07:15 12:30:00"}
        name = f"Capture {year}.jpg"
        image.save(output / name, quality=92, exif=exif)
        expected.append({"file": name, "width": 720, "height": 480, "capture_year": year})
    oriented = Image.new("RGB", (720, 360), "#93765a")
    draw = ImageDraw.Draw(oriented)
    draw.rectangle((0, 0, 180, 100), fill="white")
    draw.text((20, 35), "TOP LEFT BEFORE ROTATION", fill="black")
    for orientation in (6, 8):
        exif = Image.Exif()
        exif[274] = orientation
        name = f"Orientation {orientation}.jpg"
        oriented.save(output / name, quality=92, exif=exif)
        expected.append({"file": name, "width": 360, "height": 720})
    for name, size, colour, mode in (
        ("Wide panorama.png", (3000, 120), "#887750", "RGB"),
        ("Tall portrait.png", (120, 3000), "#786694", "RGB"),
        ("Açores 日本語.png", (240, 160), "#6a8b96", "RGB"),
        ("Transparent.png", (300, 200), (0, 0, 0, 0), "RGBA"),
    ):
        image = Image.new(mode, size, colour)
        if mode == "RGBA":
            ImageDraw.Draw(image).ellipse((50, 25, 250, 175), fill=(200, 110, 60, 255))
        image.save(output / name)
        expected.append({"file": name, "width": size[0], "height": size[1]})
    (output / "Duplicate capture.jpg").write_bytes((output / "Capture 2019.jpg").read_bytes())
    (output / "Corrupt.jpg").write_bytes(b"This is deliberately not a JPEG.")
    report = {
        "synthetic": True, "museum_image_count": 0, "valid_distinct": len(expected),
        "duplicate_files": 1, "corrupt_files": 1, "expected": expected,
        "required": ["errors visible", "duplicate excluded", "orientation respected", "capture years retained",
                     "aspect ratios retained", "Unicode filename searchable", "original bytes unchanged"],
    }
    (output / "expected.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    create(parser.parse_args().output)
