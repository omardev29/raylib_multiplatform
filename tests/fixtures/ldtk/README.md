# LDtk fixtures

`tests/ldtk_test.cpp` reads these. Two kinds, for two different questions.

**Written by LDtk itself** -- "does the reader understand what the editor
actually saves?". These are four of the sample projects LDtk 1.5.3 ships in its
installer (`extraFiles/samples/`), copied byte for byte, the JSON only: none of
the tileset art they name is here, and none is needed, because parsing is
arithmetic. The installer was `LDtk 1.5.3 installer.AppImage`, sha256
`8b5cd2b5a4ba38f93be118ae8fffad343533e0a435c73ce3a779289293a737d3`.

| File | What it is there for |
|---|---|
| `Test_file_for_API_showing_all_features.ldtk` | every field type, an 8 px IntGrid under 16 px layers, flipped and stacked tiles, LDtk's built-in icon atlas, a linear layout |
| `Typical_2D_platformer_example.ldtk` | a Free world: levels at world positions, one of them at a negative y, and neighbours |
| `Typical_TopDown_example.ldtk` | a GridVania world, a layer with an offset, tiles with alpha |
| `SeparateLevelFiles.ldtk` + `World_Level_*.ldtkl` | levels saved to their own files. LDtk puts them in a `SeparateLevelFiles/` folder; they are flat here, because `resources/` is flat and the reader looks them up by file name |

LDtk is by Sébastien Benard (Deepnight Games), MIT licensed; the notice is in
`LICENSE-ldtk.txt`. These files are test data, never part of a build.

**Written by hand** -- "does it do the right thing with each value?":
`minimal.ldtk` is small enough to know every number in it, and
`tests/ldtk_test.cpp` builds the hostile ones (wrong types, nulls, values out of
range) inline.
