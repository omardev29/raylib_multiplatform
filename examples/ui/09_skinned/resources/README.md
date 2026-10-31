# resources/ of the skinned UI example

Every file here is loaded by name from `src/scenes/shop.cpp`.

| File | What | Licence |
|---|---|---|
| `skin.aseprite` | one 72x48 frame: the nine-slices `button`, `button_down`, `panel` and `bar`, and the 12x12 icons `coin`, `heart`, `gear`, `star`, `play`, `lock`, each a named slice | CC0, `License-art.txt` |
| `spinner.aseprite` | four 16x16 frames of a turning star, tagged `spin` | CC0, `License-art.txt` |
| `click.wav`, `hover.wav`, `nope.wav` | the interface's sounds | CC0, `License-art.txt` |
| `title.ttf` | DejaVu Sans, subset to Latin-1: the titles' font | Bitstream Vera + public domain, `License-title.ttf.txt` |

`python3 tools/make_skinned_art.py` writes everything but the font again.
