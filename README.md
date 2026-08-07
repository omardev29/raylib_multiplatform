# raylib CMake Template

A batteries-included C++20 project template that handles all the boilerplate of linking [raylib](https://www.raylib.es/) statically via CMake. Clone or use the template button, and start writing code immediately.

It's fine to say that this template modifies raylib and raymob slightly to adapt it to the template, but it's mainly CMakeList stuff

Based on [meemknight/raylibCmakeSetup](https://github.com/meemknight/raylibCmakeSetup).

---

## Table of Contents

- [Project Structure](#project-structure)
- [How the Linking Works](#how-the-linking-works)
- [Dependencies](#dependencies)
- [Building](#building)
- [RESOURCES_PATH](#resources_path)
- [Editor Setup](#editor-setup)
  - [Common](#common)
  - [Visual Studio 2022](#visual-studio-2022)
  - [VSCode / VSCodium](#vscode--vscodium)
  - [CLion](#clion)
  - [Neovim (clangd)](#neovim-clangd)
- [Adding New Source Files](#adding-new-source-files)
- [Adding New Libraries](#adding-new-libraries)
- [Export to Web](#export-to-web)
- [Android](#Android)
- [CI/CD](#ci/cd)
- [FAQ](#faq)

---

## Project Structure

```
.
├── CMakeLists.txt              # Main build configuration
├── CMakePresets.json           # Cmake profile
├── .clangd                     # clangd LSP configuration (Neovim/Helix/etc.)
├── include/                    # Your project headers
├── raymob/                     # This template is integrated with this for exporting to Android
├── src/                        # Your project source files (.cpp)
│   └── main.cpp
├── resources/                  # Game assets (textures, sounds, fonts, etc.)
└── thirdparty/
    ├── raylib-5.5/
    ├── raymob/                  # raymob functions for android
    # you can add other libraries here
```

---

### Compile-time definitions

The CMakeLists sets several `#define`s that are available in your code:

| Macro | Value | Description |
|---|---|---|
| `RESOURCES_PATH` | Absolute path (dev) or `"./resources/"` (production) | Path to your assets folder |
| `PRODUCTION_BUILD` | `0` or `1` | Whether this is a production build |

Use them in code like:

```cpp
Texture2D tex = LoadTexture(RESOURCES_PATH "player.png");
```

### LTO (Link Time Optimization)

```cmake
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
```

This enables LTO globally, which allows the linker to optimize across translation units. It produces smaller, faster binaries at the cost of longer link times. It's active in all build types. If compile times become a problem during development you can temporarily set it to `FALSE`.

## Dependencies

You need the following installed before building:

| Tool | Required | Notes |
|---|---|---|
| CMake | ≥ 3.30 | learn cmake would be good but you dont need it for this template |
| A C++20 compiler | Yes | MSVC, GCC, or Clang |
| Ninja | Strongly Recommended | Faster builds, required for `compile_commands.json`|

- If you dont't have a compiler installed yet, i recommend installing gcc with a package manager:
### Linux (apt)
```bash
sudo apt install build-essential
```
### Linux (pacman)
```bash
sudo pacman -S --needed base-devel
```
### Windows
- You can use msvc, but if you don't want to i recommend installing MinGW with a package manager like choco or scoop, install it and then:
```bash
scoop install mingw
```
---

## Building

In CMake, change the project name to whatever you want, and that will be your exe.
<br>
<img width="1202" height="343" alt="Image" src="https://github.com/user-attachments/assets/86bb1a61-12a6-4845-87a5-d6fce7397aa0" />

The template also has an option to view the assembler; once you have a build of your project, run `cmake --build build --target assembler`.

also, in linux make sure you have the x11 libraries and if you use wayland you may need xwayland
```bash
# pacman
sudo pacman -Syu libx11 libxrandr libxi libxcursor libxinerama mesa

# apt
sudo apt update;sudo apt install libx11-dev libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev libgl1-mesa-dev
```
---

## Production Build (IMPORTANT)

To make a distributable build make sure you use the the release preset

What changes in a production build:

- `RESOURCES_PATH` becomes `"./resources/"` (relative to the executable) instead of the absolute source path. You must ship the `resources/` folder next to the executable.
- `PRODUCTION_BUILD` is defined as `1` in your code, so you can `#if PRODUCTION_BUILD` to strip debug features.
- On MSVC, the console window is hidden.

---

## RESOURCES_PATH

During development, `RESOURCES_PATH` expands to the absolute path of your `resources/` folder in the source tree. This means you can run the executable from any working directory and asset loading will still work.

In production, it becomes `"./resources/"`, so the folder must be next to the `.exe`.

Prefer the asset layer below (`Assets::LoadImage` / `Assets::LoadTexture`) over raw
`RESOURCES_PATH` calls — it transparently switches between loose files and a packed,
optionally-encrypted `resources.rres`.

---

## Resources packing (rres) + encryption

Assets can be packed into a single `resources.rres` container ([rres](https://github.com/raysan5/rres)),
optionally **AES-256 encrypted**. The game auto-detects which mode to use at startup
(`Assets::Init()`): if `resources.rres` exists next to the resources it loads from it,
otherwise it loads loose files. **No code changes needed** to switch.

- Pack (AES): `cmake --build build --target pack_resources`
- Unpack (back to loose): `cmake --build build --target unpack_resources`
- The packer is the open `tools/rres_pack` (no paid/closed tooling required). The official
  `rrespacker` output is also compatible if you own it.
- Set the password with `-DRRES_PACK_PASSWORD="..."` (default `raylib-template`). This is
  **obfuscation, not real security** — the password is embedded in the binary.

> Note: encryption only adds load-time cost (Argon2i key derivation per resource), never
> per-frame cost.

## Game lifecycle (Godot style)

Your game logic lives in three functions in `src/main.cpp` — `_ready()`, `_process()`,
`_exit()` — and a small platform runner drives them: a classic `while` loop on
desktop/BSD/Android/Web, and `ios_ready`/`ios_update`/`ios_destroy` callbacks on iOS.
This is what makes the same code run everywhere, including iOS.

---

## Editor Setup

### Visual Studio 2022
Open the folder directly in VS 2022/2026 (File → Open → Folder). VS will detect `CMakeSettings.json` and configure automatically using the `Debug` profile
You can change presets here.
<br>
<img width="261" height="117" alt="Image" src="https://github.com/user-attachments/assets/d41d4a69-9380-45f6-a453-15cc787143b9" />
<br>
And make sure to select ray_test or whatever name you gave your project as an exe
<br>
<img width="512" height="223" alt="Image" src="https://github.com/user-attachments/assets/d901630c-ee47-4bca-9eba-561f77b53bbf" />

if you have a problem with cmake, delete the build folder and recompile

### COMMON
- This has to be done in all the editors

- You need to run the generate compile_commands.json.sh in Linux or Mac
and .ps1 in Windows and the the update_clangd script

- This script will create a compile_commands file with Android support as well, so make sure you have the necessary Android dependencies or modify the script yourself to exclude Android.

- after that use cmake --preset debug to finish


### VSCode / VSCodium

Install the **CMake Tools** extension. Open the folder, select a kit (MSVC or GCC) when prompted, and click Build. 

For IntelliSense, also install the **clangd** extension, `compile_commands.json` is generated automatically

### CLion

CLion detects `CMakePresets.txt` automatically. the cmake its configurate to compile the `compile_commands.json` automatically

### Neovim (clangd)


# Linux / macOS

it's just

**Configure, (make the build folder)**
```bash
# just one time
cmake --preset debug
cmake --preset release # just to deploy the game
```
**Compile (make the exe)**
```bash
cmake --build build
```
**See all the presets availables**

```bash
cmake --list-presets
```
To rebuild after CMake changes:
```bash
cmake --build build
```

---

## Adding New Source Files

Drop any `.cpp` file into `src/` (or a subfolder of `src/`). The `GLOB_RECURSE` in `CMakeLists.txt` picks it up on the next build automatically thanks to `CONFIGURE_DEPENDS`.

Headers can go in `include/` — that directory is already on the include path.

---

## Adding New Libraries

To add another library from source (same pattern as the existing ones):

1. Add it to `thirdparty/`
2. Add `add_subdirectory(thirdparty/yourlibrary)` in `CMakeLists.txt`
3. Add its target name to `target_link_libraries`:

```cmake
target_link_libraries("${CMAKE_PROJECT_NAME}" PRIVATE
    raylib_static awesome_library xd yourlibrary_target_name
)
```

The target name depends on what the library's own `CMakeLists.txt` defines — check its source or documentation.

---

## Export to Web

This template supports exporting to Web using [Emscripten](https://emscripten.org/).

### Prerequisites


1. **Install Emscripten SDK:**
   ```powershell
   # Clone emsdk
   git clone https://github.com/emscripten-core/emsdk.git C:\Users\<your_user>\Documents\Emscripten\emsdk
   
   # Navigate to the folder
   cd C:\Users\<your_user>\Documents\Emscripten\emsdk
   
   # Install and activate the latest SDK
   emsdk install latest
   emsdk activate latest
   ```
  - You can install Emscripten using a package manager. If you do this, go to its installation folder, find Emscripten.cmake, and place it in the Cmake presets directory.     It's usually located in a path similar to this: ../Cmake/Modules/Platform/Emscripten.cmake
   

3. **Activate emsdk** (once per terminal session):
   ```powershell
   C:\Users\<your_user>\Documents\Emscripten\emsdk\emsdk_env.ps1
   ```


   Windows example


### Building for Web (CLI)

```powershell
# 1. Configure CMake with the web preset
cmake --preset web

# 2. Build the project
cmake --build --preset web
```

The output files will be in `build/web/`:
- `ray_test.html` - Main HTML file
- `ray_test.js` - Emscripten runtime
- `ray_test.wasm` - WebAssembly binary
- `ray_test.data` - Packaged resources (textures, audio, etc.)

### Building for Web (Visual Studio 2022)

1. Open the project folder in VS 2022
2. Select the **web** configuration from the dropdown menu
3. Build normally (Ctrl + Shift + B)

### Building for Web (VSCode)

you can figure it out 

### Testing the Web Build

Web builds require a local HTTP server (opening the HTML file directly won't work):

```powershell
# Python
python -m http.server 8000 --directory build/web

# Emrun
emrun build/web/ray_test.html

# Node.js
npx serve build/web
```

Then open `http://localhost:8000/ray_test.html` in your browser.

### Memory Configuration

By default, the Web build allocates **67 MB** of memory (`TOTAL_MEMORY=67108864`). If your game needs more memory (for large textures, audio, or data), modify this value in `CMakeLists.txt`:

```cmake
if(${PLATFORM} STREQUAL "Web")
    set_target_properties("${CMAKE_PROJECT_NAME}" PROPERTIES
        SUFFIX ".html"
        LINK_FLAGS "-s USE_GLFW=3 -s ASYNCIFY -s TOTAL_MEMORY=134217728 --preload-file ${CMAKE_SOURCE_DIR}/resources@/resources/"
    )
endif()
```

Common values:
| Value | Bytes | Description |
|-------|-------|-------------|
| `67108864` | 64 MB | Default, good for small games |
| `134217728` | 128 MB | Medium games with more assets |
| `268435456` | 256 MB | Large games with many textures |

Increase only as needed — larger values mean longer load times.



# ANDROID

- open the raymob folder with android studio first it will install any necessary dependencies if they are missing

- Install cmake 3.30.3 inside android studio (It doesn't matter if you already have it installed on your PC, Android Studio needs its own things.)

- make sure you have java 11+ installed (if its posible latest version to avoid problems)

- after that, you can just ./gradlew build for APK or ./gradlew bundleRelease for aab


## raymob.h
  - this template provides acces to raymob.h, It's a header that allows you to use native Android functions; use it in your code like this.
  ```cpp

  
#include <raylib.h>
#include <raymob.h> // It will expand to practically nothing if you don't export to mobile

int main() {

#ifdef __ANDROID__
  Vibrate(2);
#endif
// rest of the code

}
```

## CI/CD
- The template ships a multiplatform GitHub Actions workflow (`.github/workflows/build.yaml`).
  Change `PROJECT_NAME` (and `BUILD_IMAGE`, see below) at the top to your project's values.
- It builds: **Linux x64 / ARM64 / RISC-V, Windows x64 / ARM64, macOS, iOS, Web, Android,
  FreeBSD, OpenBSD, NetBSD**. On a tag push it also creates a Release with the artifacts.
- **Frozen build image:** Linux jobs run inside a Docker image with every toolchain pinned.
  The image lives in its **own repo** (`raylib-build-image`, next to this one: `Dockerfile` +
  `docker-image.yaml` → GHCR). This is what makes builds reproducible (nothing is downloaded
  at job time). Set `BUILD_IMAGE` at the top of `build.yaml` to your published image.
- **Reproducibility:** all third-party actions are pinned by full commit SHA, and raylib /
  raylib-iOS versions are frozen (see `thirdparty/FROZEN_VERSIONS.md`).
- The experimental targets (RISC-V, Windows ARM64, iOS, BSDs) are `continue-on-error`, so they
  don't block a release while they're being tuned; their artifacts are uploaded when they succeed.
- For local debugging keep using `cmake --build build` — it only recompiles your C++ and is much
  faster than waiting on CI.

## FAQ


**Q: I changed `PRODUCTION_BUILD` and the build is wrong.**  
A: Delete the build folder entirely and reconfigure. CMake caches this value and VS in particular doesn't always detect the change.

**Q: Can I use this on Linux/macOS?**  
A: Yes. The CMake setup is cross-platform. On Linux you need a few system GL/X11 libraries (listed in the [Linux](#linux) section). The `CMakeSettings.json` is VS-specific and ignored on other platforms.

**Q: Do I need to ship any DLLs with the game?**  
A: No. Everything is linked statically. On Windows with MSVC, even the CRT is static, so no Visual C++ Redistributable is needed
