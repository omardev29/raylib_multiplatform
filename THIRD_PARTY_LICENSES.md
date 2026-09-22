# Third-party licences

This framework vendors the libraries below. Each keeps its own licence; the
authoritative texts are the `LICENSE` files and in-file notices the table points
at. Where we have altered a component, the `modified` column says so and a
`PATCHES.md` next to it lists every change -- the zlib licence requires altered
source versions to be plainly marked, and it is the right thing to do whatever
the licence.

**The block below is machine-readable, and it is the single source of two
things**: `tools/license_check.sh` compares it against what is actually on disk
(both directions, so a dependency added without a row fails and a row without a
dependency fails), and `tools/configure.py` generates the `LICENSES.txt` that
ships next to every binary from it. A component that is not here does not ship
a notice; a component that is here ships the notice the row points at.

Columns: `name`; `path` from the repository root; `licences` the upstream offers
(`|` between alternatives); `elect` which alternative **we take** when there is
a choice (`-` when there is none); `modified` -- `no`, `yes` (see the
`PATCHES.md` in that directory) or `subset` (a partial copy, also documented in
a `PATCHES.md`); `linked` -- which release families the code reaches
(`all`, `desktop` = Linux/Windows/macOS/BSD, `web`, `android`, `ios`, or `none`
for something that is never in a shipped artefact); `evidence` -- where the
licence text is: a `LICENSE` file in the directory (`file`), a named file
(`file:<name>`), a block in the source itself (`header`), the parent component's
own licence (`part-of:<name>`), or, only for public-domain dedications where no
notice is owed, an upstream URL (`upstream:<url>`).

```components
# name                      path                                                    licences                 elect          modified  linked    evidence
raylib                      thirdparty/raylib                                       zlib                     -              yes       all       file
clay                        thirdparty/clay                                         zlib                     -              yes       all       file
cute_tiled                  thirdparty/cute_tiled                                   zlib|Unlicense           Unlicense      yes       all       file
cute_aseprite               thirdparty/cute_aseprite                                zlib|Unlicense           Unlicense      no        all       file
raylib-cpp                  thirdparty/raylib-cpp                                   zlib                     -              subset    all       file
rres                        thirdparty/rres                                         MIT                      -              no        all       file
tiny-AES-c                  thirdparty/rres/external/aes.c                          Unlicense                -              no        all       file:LICENSE-tiny-AES-c.txt
monocypher                  thirdparty/rres/external/monocypher.c                   BSD-2|CC0                CC0            no        all       header
lz4                         thirdparty/rres/external/lz4.c                          BSD-2                    -              no        none      header
qoi                         thirdparty/rres/external/qoi.h                          MIT                      -              no        all       header
raymob                      thirdparty/raymob                                       MIT                      -              yes       android   file
doctest                     thirdparty/doctest                                      MIT                      -              no        none      file
raylib-ios                  thirdparty/raylib-ios                                   zlib                     -              no        ios       file
angle                       ios/ANGLE-LICENSE.txt                                   BSD-3                    -              no        ios       file
# What raylib bundles under src/external/. Statically linked into every binary
# that uses the module they back; none of them is modified by us (raylib's
# PATCHES.md says so), so they are recorded as raylib ships them.
glfw                        thirdparty/raylib/src/external/glfw                     zlib                     -              no        desktop   file
RGFW                        thirdparty/raylib/src/external/RGFW                     zlib                     -              no        desktop   header
RGFW.h                      thirdparty/raylib/src/external/RGFW.h                   zlib                     -              no        desktop   header
glad                        thirdparty/raylib/src/external/glad.h                   WTFPL|CC0|public-domain  CC0            no        desktop   upstream:https://github.com/Dav1dde/glad
glad-gles2                  thirdparty/raylib/src/external/glad_gles2.h             WTFPL|CC0|Apache-2.0     CC0            no        all       header
cgltf                       thirdparty/raylib/src/external/cgltf.h                  MIT                      -              no        all       header
cgltf-write                 thirdparty/raylib/src/external/cgltf_write.h            MIT                      -              no        all       header
dirent                      thirdparty/raylib/src/external/dirent.h                 permissive               -              no        desktop   header
dr_flac                     thirdparty/raylib/src/external/dr_flac.h                Unlicense|MIT-0          Unlicense      no        all       header
dr_mp3                      thirdparty/raylib/src/external/dr_mp3.h                 Unlicense|MIT-0          Unlicense      no        all       header
dr_wav                      thirdparty/raylib/src/external/dr_wav.h                 Unlicense|MIT-0          Unlicense      no        all       header
fix_win32_compatibility     thirdparty/raylib/src/external/fix_win32_compatibility.h MIT                     -              no        desktop   header
jar_mod                     thirdparty/raylib/src/external/jar_mod.h                WTFPL|public-domain      public-domain  no        all       header
jar_xm                      thirdparty/raylib/src/external/jar_xm.h                 WTFPL                    -              no        all       header
m3d                         thirdparty/raylib/src/external/m3d.h                    MIT                      -              no        all       header
miniaudio                   thirdparty/raylib/src/external/miniaudio.h              Unlicense|MIT-0          Unlicense      no        all       header
msf_gif                     thirdparty/raylib/src/external/msf_gif.h                MIT|Unlicense            Unlicense      no        all       header
par_shapes                  thirdparty/raylib/src/external/par_shapes.h             MIT                      -              no        all       header
qoa                         thirdparty/raylib/src/external/qoa.h                    MIT                      -              no        all       header
qoaplay                     thirdparty/raylib/src/external/qoaplay.c                MIT                      -              no        all       header
qoi-raylib                  thirdparty/raylib/src/external/qoi.h                    MIT                      -              no        all       header
rl_gputex                   thirdparty/raylib/src/external/rl_gputex.h              zlib                     -              no        all       header
rlsw                        thirdparty/raylib/src/external/rlsw.h                   MIT                      -              no        all       header
rltexgpu                    thirdparty/raylib/src/external/rltexgpu.h               zlib                     -              no        all       header
rprand                      thirdparty/raylib/src/external/rprand.h                 zlib                     -              no        all       header
sdefl                       thirdparty/raylib/src/external/sdefl.h                  MIT|Unlicense            Unlicense      no        all       header
sinfl                       thirdparty/raylib/src/external/sinfl.h                  MIT|Unlicense            Unlicense      no        all       header
stb_image                   thirdparty/raylib/src/external/stb_image.h              MIT|Unlicense            Unlicense      no        all       header
stb_image_resize2           thirdparty/raylib/src/external/stb_image_resize2.h      MIT|Unlicense            Unlicense      no        all       header
stb_image_write             thirdparty/raylib/src/external/stb_image_write.h        MIT|Unlicense            Unlicense      no        all       header
stb_perlin                  thirdparty/raylib/src/external/stb_perlin.h             MIT|Unlicense            Unlicense      no        all       header
stb_rect_pack               thirdparty/raylib/src/external/stb_rect_pack.h          MIT|Unlicense            Unlicense      no        all       header
stb_truetype                thirdparty/raylib/src/external/stb_truetype.h           MIT|Unlicense            Unlicense      no        all       header
stb_vorbis                  thirdparty/raylib/src/external/stb_vorbis.c             MIT|Unlicense            Unlicense      no        all       header
tinyobj_loader_c            thirdparty/raylib/src/external/tinyobj_loader_c.h       MIT                      -              no        all       header
vox_loader                  thirdparty/raylib/src/external/vox_loader.h             MIT                      -              no        all       header
win32_clipboard             thirdparty/raylib/src/external/win32_clipboard.h        zlib                     -              no        desktop   part-of:raylib
```

## The elections, and why

Where upstream offers a choice, we take the alternative that owes the least,
and we say so here so that it is a decision on record and not something
inferred from a build:

- **cute_tiled, cute_aseprite** (Randy Gaul): zlib OR public domain -- we take
  the **Unlicense**. cute_tiled is modified anyway (see its `PATCHES.md`),
  because a reader of the file cannot tell which alternative was chosen.
- **monocypher**: BSD-2-Clause OR CC0-1.0 -- we take **CC0**. Its notice still
  ships, because it costs nothing and it is what the author would want.
- **stb\_\***, **sdefl/sinfl**, **msf_gif**: MIT OR public domain -- **Unlicense**.
- **dr_flac/dr_mp3/dr_wav**, **miniaudio** (David Reid): public domain OR
  MIT-0 -- **Unlicense**.
- **glad** loaders: the generated code is WTFPL OR CC0 -- **CC0**. `glad_gles2.h`
  is also `AND Apache-2.0` for the Khronos API data it was generated from; that
  part is not electable, it is unmodified, and its notice (the SPDX line and
  the generator header) ships with the block.
- **jar_mod**: "public domain / C0" in the file's own words, WTFPL for the
  lines it took from jar_xm -- **public domain**.

## What is modified, and where the mark is

The zlib licence's clause 2 requires altered source versions to be plainly
marked as such. Every altered component has a `PATCHES.md` in its directory
that starts with the word MODIFIED and lists file, line, change and reason, and
every change is commented at its site:

| Component | Mark |
|---|---|
| raylib 6.0.0 | [`thirdparty/raylib/PATCHES.md`](thirdparty/raylib/PATCHES.md) -- six build-system and platform-selection patches, no public function changed, nothing under `src/external/` touched |
| Clay 0.14 | [`thirdparty/clay/PATCHES.md`](thirdparty/clay/PATCHES.md) -- one line, the C++ version guard |
| cute_tiled | [`thirdparty/cute_tiled/PATCHES.md`](thirdparty/cute_tiled/PATCHES.md) -- unknown JSON keys are skipped instead of failing the load |
| raylib-cpp 6.0.3 | [`thirdparty/raylib-cpp/PATCHES.md`](thirdparty/raylib-cpp/PATCHES.md) -- a partial copy, the math headers only |
| raymob | [`thirdparty/raymob/PATCHES.md`](thirdparty/raymob/PATCHES.md) -- JNI hardening; MIT has no mark clause, recorded anyway |

`thirdparty/FROZEN_VERSIONS.md` carries the same patches with the full
reasoning, and the sha256 of every unmodified single-header component
(`sha256_<name>` in its versions block), which `tools/license_check.sh`
recomputes: a file that changes without its row changing fails the build.

## Notes on particular components

- **raylib** is by Ramon Santamaria and is zlib/libpng licensed. This project
  is built on it, is not affiliated with or endorsed by the raylib project,
  and ships a modified copy -- the modifications are listed above and in the
  shipped `LICENSES.txt`.
- **raylib-iOS** is a fork of raylib by ghera, distributed under raylib's
  zlib/libpng licence, which the fork inherits. Used only for the iOS target,
  as the git submodule `thirdparty/raylib-ios` pinned to commit `29ce933d` on
  our own fork `omardev29/raylib-iOS`. Credit: <https://github.com/ghera/raylib-iOS>
  (based on raylib by Ramon Santamaria, iOS rcore from PR raysan5/raylib#3880
  by blueloveTH). The submodule is empty on a machine that does not build for
  iOS, and `configure.py` refuses an iOS configure without it rather than
  shipping an iOS notice with the fork missing from it.
- **ANGLE** (OpenGL ES over Metal) is copyright The ANGLE Project Authors,
  BSD-3-Clause. The raylib-iOS fork ships the prebuilt `libEGL.xcframework` and
  `libGLESv2.xcframework` under `thirdparty/raylib-ios/deps/ANGLE/` without
  the ANGLE LICENSE file, so the full BSD text is reproduced at
  [`ios/ANGLE-LICENSE.txt`](ios/ANGLE-LICENSE.txt) and ships in the iOS
  bundle's `LICENSES.txt` (upstream: <https://github.com/google/angle/blob/main/LICENSE>).
- **raymob** (Le Juez Victor, MIT) is the Android glue: `thirdparty/raymob/`
  is the native half and `raymob/` the Gradle project with the Java half;
  the two `LICENSE` files are byte-identical. It is linked only into the
  Android APK/AAB, and that is the only `LICENSES.txt` it appears in.
- **rres** (Ramon Santamaria, MIT) and its `external/`: **tiny-AES-c** (kokke,
  Unlicense -- the vendored files carry no notice of their own, so the
  Unlicense text and the provenance are in
  [`thirdparty/rres/external/LICENSE-tiny-AES-c.txt`](thirdparty/rres/external/LICENSE-tiny-AES-c.txt)),
  **Monocypher 4.0.2** (Loup Vaillant), **QOI** (Dominic Szablewski, MIT), and
  **LZ4** (Yann Collet, BSD-2-Clause), which is in the tree but **not compiled**:
  `src/rmp/rres_impl.cpp` enables AES and XChaCha20 and never
  `RRES_SUPPORT_COMPRESSION_LZ4`. The day `[resources]` gains compression its
  row changes from `none` to `all` and its notice starts shipping.
- **Clay** (Nic Barker, zlib) is the layout engine behind `rmp::ui`.
- **raylib-cpp** (Rob Loach, zlib): the math subset only, header-only, behind
  `rmp/math.h`.
- **doctest** (Viktor Kirilov, MIT) is compiled into `unit_test`, which is never
  shipped; it is in no release artefact and in no `LICENSES.txt`.
- **raylib's bundled dependencies** (`thirdparty/raylib/src/external/`): the
  audio decoders (miniaudio, dr_flac, dr_mp3, dr_wav, stb_vorbis, qoa, jar_mod,
  jar_xm), the image and font code (stb_image, stb_image_write,
  stb_image_resize2, stb_truetype, stb_rect_pack, stb_perlin, qoi, msf_gif,
  sdefl, sinfl, rl_gputex, rltexgpu), the model loaders (cgltf, m3d,
  tinyobj_loader_c, vox_loader, par_shapes), the windowing backends (GLFW,
  RGFW, glad), the software rasteriser (rlsw), the random generator (rprand)
  and three Windows shims (dirent, win32_clipboard, fix_win32_compatibility).
  All permissive; each row above says which family and where its notice is.
- **The framework's own code** (`src/`, `include/`, `tools/`, `cmake/`, `ios/`,
  the build files) is under the root `LICENSE` (MIT). `tools/md5.c` is a
  transcription of RFC 1321 written for this repository's `rres_pack` tool and
  is ours; its header says so.
