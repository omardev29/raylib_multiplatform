# raylib_multiplatform

A batteries-included **C++20** template for making games with [raylib](https://www.raylib.com/).
Static linking, no DLLs to ship, cross-platform, and a CI pipeline that builds **14 targets**
out of the box. Clone it (or *Use this template*), rename one line, and start writing your game.

Based on [meemknight/raylibCmakeSetup](https://github.com/meemknight/raylibCmakeSetup).

## Supported targets

| Platform | Notes |
|---|---|
| **Windows** (x64 / ARM64) | MSVC or MinGW, fully static |
| **Linux** (x64 / ARM64 / RISC-V) | X11 + Mesa |
| **macOS** | Apple Clang |
| **Web** (HTML5) | Emscripten / WebAssembly |
| **Android** | via raymob + Gradle, with AdMob |
| **iOS** | via the `raylib-iOS` fork (see `ios/`) |
| **FreeBSD / OpenBSD / NetBSD** | x64 / ARM64 (see `TECHNICAL.md`) |

**Extras baked in**

- **Godot-style lifecycle** — write your game in `_ready()` / `_process()` / `_exit()`; the same
  code runs everywhere, including iOS.
- **Resource packing & encryption** — pack assets into a single AES-encrypted `resources.rres`
  ([rres](https://github.com/raysan5/rres)) with one command.
- **AdMob** — interstitial + rewarded ads on Android via a cross-platform API (no-ops elsewhere).
- **CI/CD** — GitHub Actions builds all targets, **boots them and checks they actually render**,
  then cuts a Release and publishes to itch.io on tag push. Pushes and PRs get a ~10 minute fast
  lane instead of the full hour.

---

## Quick start

1. **Create your repo** — click **Use this template** (or clone).
2. **Rename your project** — in `CMakeLists.txt` change `project(ray_test ...)` to your game name.
   That becomes your executable name.
   <img width="1202" height="343" alt="Rename project in CMakeLists" src="https://github.com/user-attachments/assets/86bb1a61-12a6-4845-87a5-d6fce7397aa0" />
3. **Configure & build**
   ```bash
   cmake --preset debug       # configure (first time)
   cmake --build build        # build
   ```
4. **Run** the binary in `build/`. On Linux make sure the X11 dev libraries are installed
   (`sudo apt install libx11-dev libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev libgl1-mesa-dev`).

That's it. Edit `src/main.cpp` and go.

---

## Make it yours

| Task | How |
|---|---|
| **Add code** | Drop `.cpp` files into `src/` (any subfolder). They're picked up automatically. Headers go in `include/`. |
| **Add assets** | Put files in `resources/`, load them with the asset layer (below). |
| **Rename the game** | `project(<name> ...)` in `CMakeLists.txt`. |
| **Replace the sample** | The rabbit in `src/main.cpp` / `include/test.h` is just a demo — swap it for your game. |
| **Add a library** | Put it in `thirdparty/`, `add_subdirectory(...)` it, add it to `target_link_libraries`. See `TECHNICAL.md`. |

### Loading assets

Prefer the asset layer over raw paths — it transparently loads from a packed `resources.rres`
(if present) or loose files otherwise:

```cpp
#include <assets.h>

Image     img = Assets::LoadImage("player.png");
Texture2D tex = Assets::LoadTexture("player.png");
```

You can pack your assets (optionally AES-encrypted) with:

```bash
cmake --build build --target pack_resources    # -> resources/resources.rres
cmake --build build --target unpack_resources  # back to loose files
```

---

## Build presets

| Preset | Purpose |
|---|---|
| `debug` | Development. Absolute asset path, run from anywhere. |
| `release` | Distributable. `PRODUCTION_BUILD=1`, LTO, `RESOURCES_PATH="./resources/"`. |
| `web` | Emscripten / WebAssembly. |

```bash
cmake --list-presets          # see all presets
cmake --preset release        # production build
cmake --build build           # build the current config
```

> **Production:** ship the `resources/` folder (or `resources.rres`) next to the executable.

---

## Editor setup (one-time)

clangd works out of the box and **does not need the Android NDK**. Just configure
once so `build/compile_commands.json` exists:

```bash
cmake --preset debug       # generates build/compile_commands.json
```

The committed `.clangd` points clangd at that host build, so the whole project —
including `<admob.h>` and the asset layer — resolves without any extra toolchain.

> **Optional — Android intellisense.** If you also want clangd to index the code
> *as an Android build* (defines `__ANDROID__`, processes `<raymob.h>` and the NDK
> headers), that is opt-in and requires the NDK:
>
> ```bash
> ./generate_android_commands.sh   # Linux / macOS   (.\generate_android_commands.ps1 on Windows)
> ./update_clangd.sh               # rewrites .clangd to use the Android compile database
> ```
>
> To go back to the default host config: `git checkout -- .clangd`.

Then:

- **VS Code / VSCodium** — install **CMake Tools** (+ **clangd** for IntelliSense). Open the folder, pick a kit, build.
- **Visual Studio 2022/2026** — *File → Open → Folder*; it reads the CMake presets. Select your project as the startup target.
  <img width="261" height="117" alt="VS preset selection" src="https://github.com/user-attachments/assets/d41d4a69-9380-45f6-a453-15cc787143b9" />
  <img width="512" height="223" alt="Select startup target" src="https://github.com/user-attachments/assets/d901630c-ee47-4bca-9eba-561f77b53bbf" />
- **CLion** — detects the CMake presets automatically.
- **Neovim / Helix (clangd)** — the `.clangd` + `compile_commands.json` above are all you need.

If CMake gets confused, delete the `build/` folder and reconfigure.

---

## Where to read next

Everything about **how the template works** lives in **[TECHNICAL.md](TECHNICAL.md)**:

- Directory layout & build system in depth (CMake presets, static linking, LTO).
- `RESOURCES_PATH`, rres packing & encryption, the Godot-style lifecycle.
- AdMob setup, Web export, Android (raymob), iOS, and the BSD/RISC-V targets.
- The CI/CD pipeline and how to point it at your own build image.
- FAQ & troubleshooting.

Hands-on examples of the template's own features (lifecycle, AdMob, the raymob
mobile API, asset loading) live in **[examples/](examples/)**.

## License

This template is MIT. Third-party components keep their own licenses — see
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md). The bundled rabbit sprite
(`resources/rabbit.png`) is a sample asset; replace it with your own.
