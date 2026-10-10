---
name: rmp-api-design
description: How to design anything that goes into the public rmp:: surface — the simplicity rule, what earns a place, options-struct field order, and the traps that have already broken a build. Load before adding or changing a function, enum or options struct in include/rmp/.
---

# Designing public API

## The rule that outranks the rest

**As simple as possible for the user.** Advanced cases may be complex; the
simple case may not pay for them. The benchmark is a main menu in three
functions, and it is measured, not asserted:

```cpp
rmp::ui::begin();
if (rmp::ui::button("Play")) play();
if (rmp::ui::button("Quit")) quit();
rmp::ui::end();
```

## Does it get in?

Three questions, in order. Stop at the first no.

1. **Is there a real game that needs it?** "Someone might want it" is a no.
2. **Can it be composed from what exists?** Then document the pattern in
   `examples/` instead of adding API. A row that wraps is
   `grid({ .columns = 0 })`; that is why `row()` has no `wrap` flag.
3. **Does it complicate the three-function menu?** Then redesign until it does not.

Things deliberately left out, and why, are in `../next_architecture/ui/06-roadmap.md`.
Read it before proposing something — the answer may already be there.

## The shapes that are already established

Follow them; a second way to spell the same thing ages badly.

- **A plain call, then an overload taking options.** `button(label)` and
  `button(label, options)`. Never a pile of positional parameters.
- **The style guide reaches the API.** A deliberate implicit conversion carries
  `NOLINT(google-explicit-constructor)` with its reason. A public class with a
  destructor states its copy and move. Public arrays are `std::array`. Header
  constants are `inline constexpr`. A new public macro goes into
  `.clang-tidy`'s `macro-usage` AllowedRegexp with its category.
- **Options are an aggregate struct with defaults**, written with designated
  initialisers. `-1` means "whatever the theme says", `0` often means
  "automatic", `{0,0,0,0}` means "the theme's colour".
- **Containers take their body as a lambda.** No matching close call exists, so
  it cannot be forgotten. An RAII guard closes it even on an early return (the
  framework does not throw, so that is the case that matters).
- **Controls take the caller's variable by reference** and write to it, and
  return `true` on the frame it changed. There is no state of ours to
  synchronise — that is the entire state model.
- **Say what a thing MEANS, never what colour it is.** `Variant::DANGER`, not a
  `Color` parameter. That single choice is why restyling the game is one call
  and not a tour of every call site.
- **One field, two spellings, when they are genuinely one concept.** `.size`
  takes `Size::LARGE` or `34` through a converting struct in `detail`. Two
  fields could contradict each other; an overload would double every option.
- **Every read of outside data has a default and never throws.** `as_int(1)`,
  `as_string("Player")`: a missing key is the default, a key of the wrong type
  too. A save from yesterday's version is missing keys, not corrupt.
- **A non-const `[]` that must not create on read returns a path, not a
  reference.** `rmp::Value::Ref` reads like the const `[]` and creates only when
  assigned through; with a `T&`, reading grows the container, and reading an
  index past the end has to invent elements or complain.
- **A lazy service tolerates its own absence.** `rmp::audio` opens the device on
  the first sound, tries once, and on a machine without one every call is a
  silent no-op; `available()` says which case you are in, and asking opens
  nothing. A settings screen must be able to move a volume slider in a game
  that has made no sound.

## Field order is load-bearing

**C++20 requires designated initialisers in declaration order.** `{ .grow_y =
true, .width = 8 }` is a compile error. This has broken the build twice, both
times in an example.

So option structs are ordered **the way they will actually be written**:

```cpp
struct BoxOptions {
    float gap = -1;
    float padding = -1;
    Align items = Align::CENTER;
    bool grow_x = false; // sizing is interleaved BY AXIS — x, then y —
    float width = 0; // so that { .width = 240, .grow_y = true } is legal.
    bool grow_y = false; // Grouped by kind (grow_x, grow_y, width, height)
    float height = 0; // that ordinary sidebar would not compile.
    std::string_view id{}; // identity last: it is the rarest field
};
```

(The shipped one is `include/rmp/ui.h`'s `BoxOptions`; the naming table in
`rmp-code-style` applies to examples in a skill exactly as it does to code.)
A new field goes at the END of an existing options struct unless it belongs in
the sizing sequence, because every designated initialiser already written
depends on the order.

Sizing → appearance → identity. When you add a field, ask where a user would
type it, not where it looks tidy. Then run `rmp test examples`, which is the
gate that catches this.

## Boundaries

- **No Clay type may appear in `include/`.** The engine has to be replaceable
  without any user code changing. Container templates in the header call
  `detail::open_*()` declared there and defined in the `.cpp`.
- **The escape hatch is a feature.** Users can call Clay's macros, raylib, or
  OpenGL directly in the same frame. `examples/ui/03_clay_direct/` proves it
  and CI compiles it. Never design something that would break that.
- **A missing resource degrades, it does not switch the subsystem off.** A font
  that will not load logs once and falls back to the built-in one.
- **New `.toml` keys are validated in `tools/configure.py`.** A typo must be a
  configure error, not a silent fallback at runtime.
