---
name: rmp-ui-internals
description: The verified facts about Clay 0.14 and the map of src/rmp/ui/. Every entry here was confirmed by reproducing it, several of them after a bug. Load before touching anything under src/rmp/ui/ or debugging layout, hit-testing, ids or rendering.
---

# `rmp::ui` internals

## The file map

```
clay_impl.cpp   defines CLAY_IMPLEMENTATION and nothing else, like rres_impl.cpp
internal.h      the private header — THE ONLY PLACE Clay may appear
context.cpp     lazy start, scale, breakpoints, font, frame arena, ids, pointer, test seams
style.cpp       size steps, state colours, the transition table
widgets.cpp     begin / end / button / text
containers.cpp  row column panel center stack layer spacer image progress grid cell scroll
controls.cpp    checkbox slider dropdown text_input
focus.cpp       focus, keyboard and gamepad navigation, per-widget scratch state
render.cpp      Clay's render commands -> raylib draw calls
theme.cpp       theme_dark(), theme_light(), the active theme
```

Two structural rules: **no widget implements navigation** (it registers as
focusable and asks whether it is the one; everything else lives in `focus.cpp`),
and **no widget invents a colour or a size** (both come from `style.cpp`, which
is why they all feel the same and why a new widget cannot forget the transition).

## Clay 0.14 — verified, not assumed

Vendored at `thirdparty/clay/clay.h`, zlib, 5077 lines, frozen, and MODIFIED by
one line (see `thirdparty/clay/PATCHES.md`). Every line below was confirmed by
reproducing it.

- `Clay__OpenElement`, `Clay__OpenElementWithId`, `Clay__ConfigureOpenElement`,
  `Clay__CloseElement` and `Clay__OpenTextElement` are **public**
  (`CLAY_DLL_EXPORT`). The double underscore looks private; it is not.
- `Clay_ElementDeclaration` has **no `id` field** in 0.14. The id goes in when
  the element is opened, via `Clay__OpenElementWithId`.
- `Clay_EndLayout` takes a `float deltaTime`.
- `Clay_Color` is **floats 0-255**, not bytes and not 0-1.
- **`Clay_GetElementId` and `Clay_GetElementIdWithIndex(…, 0)` produce different
  hashes** — the index is mixed in before the final avalanche. Use `WithIndex`
  everywhere. Mixing them is why a panel that was visibly on screen could not be
  found by the headless test.
- **Clay does not copy strings.** "The underlying char data will not be copied
  internally and should live until at least the next frame." Everything is
  interned into a frame arena reset in `begin()`.
- `Clay_StringSlice.chars` is **not NUL-terminated**. `DrawTextEx` and
  `MeasureTextEx` both need one, so slices are copied into a scratch buffer.
- `Clay_SetMaxElementCount()` must be called **before** `Clay_MinMemorySize()`,
  or the arena is sized for the old count. Default is 8192, which reserves
  megabytes for a three-button menu; we use 512.
- **`Clay_PointerOver` answers for the previous frame's layout.** Inherent to
  immediate mode: a button cannot be clicked on the first frame it exists.
- **Clay wraps text, not elements.** A grid's rows have to be built for real,
  which is why `grid` requires a `cell()` per child — counting children is what
  tells it when to start a row.
- **Clay emits `RECTANGLE` in addition to `IMAGE`** when `backgroundColor.a > 0`,
  and the rectangle comes *after* the image in the command list. An image tint
  put in `backgroundColor` paints a flat square over the picture.
- Scrolling: `Clay_ClipElementConfig{horizontal, vertical, childOffset}`, with
  `Clay_UpdateScrollContainers(drag, delta, dt)` called **before**
  `Clay_BeginLayout` — after it the offset arrives a frame late.
- Floating elements (what `layer()` builds) are drawn **by z-index**, not by
  declaration order, so `next_layer_z()` hands out an increasing z per frame.
- SIMD includes: `<emmintrin.h>` on x86_64, `<arm_neon.h>` on aarch64, nothing on
  riscv64 or wasm.
- **One line of clay.h is patched**: the C++ version guard accepts `201709L`,
  because NetBSD 10.1's GCC 10.5 reports that under `-std=c++20`. Documented at
  the patch site and in `thirdparty/FROZEN_VERSIONS.md`.

## Our own decisions worth not reopening

- **Element identity** is `hash(label)` plus the number of times that label has
  appeared this frame, so two "Back" buttons on two screens are two elements. An
  explicit `.id` is the escape hatch for conditional UI. Parts of a widget get
  `sub_id(base, n)` — hashing a fixed suffix would give every slider in the frame
  the same track.
- **Hit-testing uses last frame's geometry**, deliberately. Clay's two-pass mode
  would run the user's code twice per frame; that is a worse trade.
- **Hover is suppressed when no pointer is present.** On a touch screen the last
  place tapped would otherwise stay lit forever.
- **Breakpoints classify by aspect ratio, not pixels.** `scale()` has already
  normalised size; a 1080-pixel-wide phone is not a desktop.
- **Transitions are colour only.** Nothing in the layout moves, so an animation
  can never make you miss what you clicked. `theme.transition = 0` removes the
  state entirely, which is what makes the headless test deterministic.
- **The transition table is direct-mapped with no probing.** A collision costs
  one control its fade, never correctness.

## Testing layout with no window

`tests/ui_layout_test.cpp` replaces two function pointers — the text measurer and
the pointer reader — and injects a viewport. `Clay_EndLayout` is pure
computation, so the whole thing runs with no GPU, no window and no display.

```
cmake --preset debug -DBUILD_UI_TESTS=ON
cmake --build --preset debug --target ui_layout_test && ./build/debug/ui_layout_test
```

Two traps that produced false failures, both in the test rather than the code:
`element_id()` called **after** the frame returns occurrence 1, because the
counters only reset in `begin()`; and text elements have no id, so a `text()` can
only be measured through a named box around it.
