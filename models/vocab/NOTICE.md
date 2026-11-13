# Naming vocabulary

Nebula names groups of similar photos with words from these lists. They are words only: no images and
no photo data.

| File | Source | License |
|---|---|---|
| `bank.txt` | Nebula: everyday subjects, scenes, events and kinds of picture | Apache License 2.0 (this repository) |
| `core_extra.txt` | Nebula: kinds of picture the core list lacks | Apache License 2.0 (this repository) |
| `photo_types.txt` | Nebula: kinds of picture common in personal libraries that neither source list has | Apache License 2.0 (this repository) |
| `excluded.txt` | Nebula: words never used as a name (what a photo cannot show, places, words that label people) | Apache License 2.0 (this repository) |
| `never_named.txt` | Nebula: words kept as evidence of what a group contains but never used as its name | Apache License 2.0 (this repository) |
| `ram_tags.txt` | Tag list of the Recognize Anything Model (RAM), https://github.com/xinyu1205/recognize-anything, `ram/data/ram_tag_list.txt`, lower-cased | Apache License 2.0, `LICENSE-ram-apache-2.0.txt` |
| `places365.txt` | Scene categories of Places365, https://github.com/CSAILVision/places365, `categories_places365.txt`, with the `/a/` prefixes, indoor/outdoor suffixes and underscores removed | MIT License, `LICENSE-places365-mit.txt`, Copyright (c) 2017 Bolei Zhou and MIT CSAIL Computer Vision |

`tools/prepare_models.py` builds the vocabulary pack from these files. The core list is `bank.txt` plus
`core_extra.txt`. The wide list is the core list plus `ram_tags.txt`, `places365.txt` and
`photo_types.txt`, minus `excluded.txt`. The script encodes every wide-list word with the SigLIP 2 text
model through five prompt templates and writes the result to `wide_siglip2.npy` in the model pack.
