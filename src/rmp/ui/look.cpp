// ===========================================================================
// Looks: what a Style resolves to, pictures, the records the renderer draws
// effects from, and the events of every control.
//
// A widget asks one question -- "what do I look like?" -- and gets one answer,
// resolved here in one order: the Theme's sounds and motion, Theme::styles for
// its kind, the `children` of the containers it is in (the nearest last), and
// its own Style. Inside a LookScope, current_theme() is the Theme with that
// answer's colours and metrics in it, so the code of every widget that read
// the theme reads the style without knowing there is one. What a Theme has no
// field for -- a skin, a shadow, a font, an outline, motion -- travels to the
// renderer in a DrawRecord or a TextRecord.
//
// Nothing here runs for an unstyled frame beyond a lookup: with the
// framework's own themes every look is empty, decorate() returns nullptr, and
// what is drawn is what was always drawn.
// ===========================================================================

#include "internal.h"
#include "../internal.h" // RMP_REPORT_ONCE

#include <rmp/audio.h>

#include <cmath>
#include <deque>
#include <numbers>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rmp::ui {

// ---------------------------------------------------------------------------
// Pictures
// ---------------------------------------------------------------------------

Picture::Picture(const rmp::Texture &whole) : texture(whole) {}

Picture::Picture(const rmp::Texture &atlas, Rectangle part) : texture(atlas), source(part) {}

Picture::Picture(const rmp::Sprite &sprite) {
    if (sprite.sheet.valid()) {
        sheet = sprite.sheet;
        const bool own = sprite.source.width > 0 && sprite.source.height > 0;
        source = own ? sprite.source : sheet.raw().frame(sprite.frame_index()).source;
        return;
    }
    texture = sprite.texture;
    source = sprite.source;
}

Picture::Picture(const NineSlice &nine)
    : texture(nine.texture), sheet(nine.sheet), source(nine.source), left(nine.left),
      top(nine.top), right(nine.right), bottom(nine.bottom), slice_scale(nine.scale) {}

bool Picture::empty() const {
    if (texture.valid()) return texture.raw().id == 0;
    return !sheet.valid() || sheet.raw().texture.id == 0;
}

Vector2 Picture::size() const {
    if (source.width > 0 && source.height > 0) return Vector2{ source.width, source.height };
    const Texture2D &t = texture.valid() ? texture.raw() : sheet.raw().texture;
    return Vector2{ static_cast<float>(t.width), static_cast<float>(t.height) };
}

bool Picture::sliced() const { return left > 0 || top > 0 || right > 0 || bottom > 0; }

namespace {

// The slice called `name`, or nothing, said once per name.
const SheetSlice *find_slice(const rmp::SpriteSheet &sheet, std::string_view name,
                             const char *who) {
    const SheetSlice &found = sheet.raw().slice(name);
    if (found.bounds.width > 0) return &found;
    RMP_REPORT_ONCE_KEYED(name, "UI: rmp::ui::%s(sheet, \"%.*s\"): the sheet has no slice by that name",
                          who, static_cast<int>(name.size()), name.data());
    return nullptr;
}

} // namespace

Picture slice(const rmp::SpriteSheet &sheet, std::string_view name) {
    const SheetSlice *s = find_slice(sheet, name, "slice");
    if (s == nullptr) return Picture{};
    Picture p;
    p.sheet = sheet;
    p.source = s->bounds;
    if (s->nine) {
        p.left = s->center.x;
        p.top = s->center.y;
        p.right = s->bounds.width - s->center.x - s->center.width;
        p.bottom = s->bounds.height - s->center.y - s->center.height;
    }
    return p;
}

NineSlice nine_slice(const rmp::SpriteSheet &sheet, std::string_view name) {
    const SheetSlice *s = find_slice(sheet, name, "nine_slice");
    if (s == nullptr) return NineSlice{};
    NineSlice n;
    n.sheet = sheet;
    n.source = s->bounds;
    if (!s->nine) {
        RMP_REPORT_ONCE_KEYED(name,
                              "UI: rmp::ui::nine_slice(sheet, \"%.*s\"): the slice has no "
                              "9-slices; it stretches whole",
                              static_cast<int>(name.size()), name.data());
        return n;
    }
    n.left = s->center.x;
    n.top = s->center.y;
    n.right = s->bounds.width - s->center.x - s->center.width;
    n.bottom = s->bounds.height - s->center.y - s->center.height;
    return n;
}

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------

void set_font(std::string_view name) { detail::set_global_font(name); }

void set_font(const rmp::Font &font) { detail::set_global_font(font.name()); }

std::string_view font_name() { return detail::global_font_name(); }

namespace detail {

namespace {

// --- resolving a look -------------------------------------------------------

template <class T> void over(std::optional<T> &base, const std::optional<T> &top) {
    if (top) base = top;
}

// `top` over `base`, field by field: what `top` sets wins, what it leaves
// unset keeps `base`'s.
void merge(Style &base, const Style &top) {
    over(base.background, top.background);
    over(base.background_hover, top.background_hover);
    over(base.background_press, top.background_press);
    over(base.background_disabled, top.background_disabled);
    over(base.skin, top.skin);
    over(base.tint, top.tint);
    over(base.border, top.border);
    over(base.border_width, top.border_width);
    over(base.radius, top.radius);
    over(base.shadow, top.shadow);
    over(base.padding_x, top.padding_x);
    over(base.padding_y, top.padding_y);
    over(base.gap, top.gap);
    over(base.text, top.text);
    over(base.text_disabled, top.text_disabled);
    if (top.font.valid()) base.font = top.font;
    over(base.font_size, top.font_size);
    over(base.text_outline, top.text_outline);
    over(base.text_shadow, top.text_shadow);
    over(base.motion, top.motion);
    over(base.sounds.hover, top.sounds.hover);
    over(base.sounds.press, top.sounds.press);
    over(base.sounds.release, top.sounds.release);
    over(base.sounds.click, top.sounds.click);
    over(base.sounds.focus, top.sounds.focus);
    over(base.sounds.change, top.sounds.change);
    over(base.sounds.open, top.sounds.open);
    over(base.sounds.close, top.sounds.close);
    over(base.sounds.shake, top.sounds.shake);
    over(base.sounds.volume, top.sounds.volume);
}

const Style &kind_style(const Theme &t, WidgetKind kind) {
    switch (kind) {
        case WidgetKind::BUTTON:
            return t.styles.button;
        case WidgetKind::LABEL:
            return t.styles.label;
        case WidgetKind::IMAGE:
            return t.styles.image;
        case WidgetKind::PANEL:
            return t.styles.panel;
        case WidgetKind::CHECKBOX:
            return t.styles.checkbox;
        case WidgetKind::SLIDER:
            return t.styles.slider;
        case WidgetKind::DROPDOWN:
            return t.styles.dropdown;
        case WidgetKind::TEXT_INPUT:
            return t.styles.text_input;
        case WidgetKind::PROGRESS:
            break;
    }
    return t.styles.progress;
}

// Whether a style sets anything the Theme has a field for.
bool themes_anything(const Style &s) {
    return s.background || s.background_hover || s.background_press ||
        s.background_disabled || s.border || s.border_width || s.radius || s.padding_x ||
        s.padding_y || s.gap || s.text || s.text_disabled || s.font_size;
}

Color lighter(Color c) { return mix_color(c, Color{ 255, 255, 255, c.a }, 0.12f); }
Color darker(Color c) { return mix_color(c, Color{ 0, 0, 0, c.a }, 0.18f); }

// The colours and metrics of `base`, without its styles, sounds and motion --
// which a widget reads from look(), and which would only be copied for nothing.
void copy_metrics(Theme &to, const Theme &from) {
    to.background = from.background;
    to.panel = from.panel;
    to.surface = from.surface;
    to.surface_hover = from.surface_hover;
    to.surface_press = from.surface_press;
    to.border = from.border;
    to.text = from.text;
    to.text_muted = from.text_muted;
    to.text_on_accent = from.text_on_accent;
    to.primary = from.primary;
    to.primary_hover = from.primary_hover;
    to.primary_press = from.primary_press;
    to.danger = from.danger;
    to.danger_hover = from.danger_hover;
    to.danger_press = from.danger_press;
    to.disabled = from.disabled;
    to.disabled_text = from.disabled_text;
    to.focus = from.focus;
    to.font_size = from.font_size;
    to.font_size_small = from.font_size_small;
    to.font_size_large = from.font_size_large;
    to.padding_x = from.padding_x;
    to.padding_y = from.padding_y;
    to.gap = from.gap;
    to.panel_padding = from.panel_padding;
    to.corner_radius = from.corner_radius;
    to.border_width = from.border_width;
    to.control_size = from.control_size;
    to.track_thickness = from.track_thickness;
    to.focus_ring = from.focus_ring;
    to.transition = from.transition;
    to.min_touch_size = from.min_touch_size;
}

// The Theme a widget with this look is drawn with: `base`'s, with what the
// style sets over it. A background is the fill of every kind of surface --
// a button of any variant, a field, a panel, a track -- and its hover and
// press, unless given, are a little lighter and a little darker.
void apply(Theme &t, const Style &s) {
    if (s.background) {
        const Color bg = *s.background;
        const Color hover = s.background_hover.value_or(lighter(bg));
        const Color press = s.background_press.value_or(darker(bg));
        t.surface = t.primary = t.danger = t.panel = bg;
        t.surface_hover = t.primary_hover = t.danger_hover = hover;
        t.surface_press = t.primary_press = t.danger_press = press;
    } else {
        if (s.background_hover)
            t.surface_hover = t.primary_hover = t.danger_hover = *s.background_hover;
        if (s.background_press)
            t.surface_press = t.primary_press = t.danger_press = *s.background_press;
    }
    if (s.background_disabled) t.disabled = *s.background_disabled;
    if (s.text) t.text = t.text_on_accent = *s.text;
    if (s.text_disabled) t.disabled_text = *s.text_disabled;
    if (s.border) t.border = *s.border;
    if (s.border_width) t.border_width = std::max(0.0f, *s.border_width);
    if (s.radius) t.corner_radius = std::max(0.0f, *s.radius);
    if (s.padding_x) t.padding_x = t.panel_padding = std::max(0.0f, *s.padding_x);
    if (s.padding_y) t.padding_y = std::max(0.0f, *s.padding_y);
    if (s.gap) t.gap = std::max(0.0f, *s.gap);
    if (s.font_size && *s.font_size > 0) {
        const float ratio = *s.font_size / (t.font_size > 0 ? t.font_size : 1.0f);
        t.font_size = *s.font_size;
        t.font_size_small *= ratio;
        t.font_size_large *= ratio;
    }
}

struct LookFrame {
    WidgetKind kind = WidgetKind::BUTTON;
    Style style;
    Theme theme;
    bool themed = false;
};

// One frame's records: deques, so that a pointer handed to Clay stays where it
// is while more are added. Emptied at the frame boundary, after they were
// drawn -- which is when the textures they hold may go.
struct {
    std::vector<Style> inherited; // cumulative: each entry is the one below with a `children` over it
    std::deque<LookFrame> stack; // the widgets being declared, innermost last
    std::deque<DrawRecord> draws;
    std::deque<TextRecord> texts;
    rmp::Callback<const Event &> hook;
    unsigned frame = 0;
} looks;

// Per element, across frames: what it was doing (for the events), when it
// appeared (for `appear`) and how far into a shake it is.
struct Memory {
    unsigned seen = 0; // the frame it was last declared in
    bool over = false;
    bool down = false;
    bool focused = false;
    float age = 0; // seconds since it appeared
    unsigned aged = 0; // the frame `age` was last advanced in
    float shaking = -1; // seconds into its shake; < 0 = not shaking
};
std::unordered_map<uint32_t, Memory> &memory() {
    static std::unordered_map<uint32_t, Memory> table;
    return table;
}

const Style &empty_style() {
    static const Style EMPTY{};
    return EMPTY;
}

float ease(float t) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    return t * t * (3.0f - 2.0f * t);
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

// The seconds of this frame, for ages and shakes: 0 headless, where an
// animation is complete the moment it starts.
float frame_seconds() { return test_mode() ? 0.0f : frame_time(); }

Memory &remember(uint32_t element) {
    Memory &m = memory()[element];
    if (m.aged != looks.frame) {
        // Seen last frame: older by a frame. Not seen since: it appears again.
        const bool continuing = m.aged + 1 == looks.frame && m.aged != 0;
        m.age = continuing ? m.age + frame_seconds() : 0.0f;
        if (m.shaking >= 0) m.shaking += continuing ? frame_seconds() : 0.0f;
        m.aged = looks.frame;
    }
    m.seen = looks.frame;
    return m;
}

Motion motion_of(const Style &s) { return s.motion.value_or(Motion{}); }

bool moves(const Motion &m) {
    return m.hover_scale != 1 || m.press_scale != 1 || m.hover_lift != 0 || m.appear > 0;
}

// The picture a skin shows in this state: its own, or `normal` lit or shaded.
const Picture *skin_picture(const Skin &skin, const Interaction &now, float &brighten,
                            Color &tint) {
    brighten = 0;
    tint = WHITE;
    if (now.disabled) {
        if (!skin.disabled.empty()) return &skin.disabled;
        tint = Color{ 140, 140, 150, 220 };
        return &skin.normal;
    }
    if (now.down) {
        if (!skin.press.empty()) return &skin.press;
        tint = Color{ 205, 205, 205, 255 };
        return &skin.normal;
    }
    if (now.over) {
        if (!skin.hover.empty()) return &skin.hover;
        brighten = 0.18f;
        return &skin.normal;
    }
    if (now.focused && !skin.focus.empty()) return &skin.focus;
    return &skin.normal;
}

Color times(Color a, Color b) {
    auto m = [](unsigned char x, unsigned char y) {
        return static_cast<unsigned char>((static_cast<unsigned>(x) * y + 127U) / 255U);
    };
    return Color{ m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a) };
}

void fill_picture(DrawRecord &r, const Picture &p) {
    r.has_picture = true;
    r.texture = p.texture;
    r.sheet = p.sheet;
    r.raw = p.texture.valid() ? p.texture.raw() : p.sheet.raw().texture;
    const Vector2 size = p.size();
    r.source = Rectangle{ p.source.x, p.source.y, size.x, size.y };
    r.left = p.left;
    r.top = p.top;
    r.right = p.right;
    r.bottom = p.bottom;
    r.slice_px = px(p.slice_scale > 0 ? p.slice_scale : 1.0f);
}

std::optional<std::string> sound_for(const Sounds &s, EventKind kind) {
    switch (kind) {
        case EventKind::HOVER:
            return s.hover;
        case EventKind::PRESS:
            return s.press;
        case EventKind::RELEASE:
            return s.release;
        case EventKind::CLICK:
            return s.click;
        case EventKind::FOCUS:
            return s.focus;
        case EventKind::CHANGE:
            return s.change;
        case EventKind::OPEN:
            return s.open;
        case EventKind::CLOSE:
            return s.close;
        case EventKind::SHAKE:
            return s.shake;
        case EventKind::UNHOVER:
        case EventKind::BLUR:
            break;
    }
    return std::nullopt;
}

// The look of a widget that is not inside a LookScope -- an event emitted
// after its scope closed: what the theme alone gives its kind.
Style resolve(WidgetKind kind, const Style *own) {
    const Theme &t = base_theme();
    Style r;
    r.sounds = t.sounds;
    r.motion = t.motion;
    merge(r, kind_style(t, kind));
    if (!looks.inherited.empty()) merge(r, looks.inherited.back());
    if (own != nullptr) merge(r, *own);
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// The scope
// ---------------------------------------------------------------------------

LookScope::LookScope(WidgetKind kind, const Style *own) {
    LookFrame &f = looks.stack.emplace_back();
    f.kind = kind;
    f.style = resolve(kind, own);
    f.themed = themes_anything(f.style);
    if (f.themed) {
        copy_metrics(f.theme, base_theme());
        apply(f.theme, f.style);
    }
}

LookScope::~LookScope() {
    if (!looks.stack.empty()) looks.stack.pop_back();
}

const Style &look() { return looks.stack.empty() ? empty_style() : looks.stack.back().style; }

const Theme *look_theme() {
    if (looks.stack.empty() || !looks.stack.back().themed) return nullptr;
    return &looks.stack.back().theme;
}

WidgetKind look_kind() {
    return looks.stack.empty() ? WidgetKind::BUTTON : looks.stack.back().kind;
}

void push_children(const Style &style) {
    Style next = looks.inherited.empty() ? Style{} : looks.inherited.back();
    merge(next, style);
    looks.inherited.push_back(std::move(next));
}

void pop_children() {
    if (!looks.inherited.empty()) looks.inherited.pop_back();
}

void begin_pass_looks() {
    looks.inherited.clear();
    looks.stack.clear();
}

void begin_frame_looks() {
    ++looks.frame;
    looks.draws.clear();
    looks.texts.clear();
    // Forget what has not been declared for a while: a screen that is gone.
    auto &table = memory();
    for (auto it = table.begin(); it != table.end();) {
        if (it->second.seen + 2 < looks.frame)
            it = table.erase(it);
        else
            ++it;
    }
}

void forget_looks() {
    looks.inherited.clear();
    looks.stack.clear();
    looks.draws.clear();
    looks.texts.clear();
    looks.hook.clear();
    memory().clear();
}

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------

DrawRecord *decorate(Clay_ElementDeclaration &d, Clay_ElementId id, const Interaction &now,
                     DrawRecord *into) {
    const Style &s = look();
    const Motion m = motion_of(s);
    Memory &mem = remember(id.id);
    const bool shaking = mem.shaking >= 0 && mem.shaking < m.shake;
    if (mem.shaking >= m.shake) mem.shaking = -1;
    const bool skinned = into == nullptr && s.skin && !s.skin->normal.empty();
    if (!skinned && !s.shadow && !moves(m) && !shaking) {
        if (into != nullptr) d.userData = into;
        return into;
    }

    DrawRecord &r = into != nullptr ? *into : looks.draws.emplace_back();
    r.element = id.id;
    r.radius = d.cornerRadius.topLeft;

    if (skinned) {
        Color state_tint = WHITE;
        const Picture *p = skin_picture(*s.skin, now, r.brighten, state_tint);
        fill_picture(r, *p);
        r.tint = times(state_tint, s.tint.value_or(WHITE));
        // The picture IS the box: no flat fill over it or under it. Clay emits
        // an IMAGE command only for an element with imageData, so it points at
        // the record's own copy of the texture.
        d.backgroundColor = Clay_Color{ 0, 0, 0, 0 };
        d.image.imageData = &r.raw;
    }

    float lift = 0;
    if (moves(m)) {
        const float speed = m.speed > 0 ? m.speed : 0.0f;
        const float hover = anim_value(id, 2, now.over && !now.disabled, speed);
        const float press = anim_value(id, 3, now.down && !now.disabled, speed);
        r.scale = lerp(1.0f, m.hover_scale, hover) * lerp(1.0f, m.press_scale, press);
        lift = px(m.hover_lift) * hover * (1.0f - press);
        r.offset.y -= lift;
        if (m.appear > 0 && mem.age < m.appear) {
            const float k = ease(mem.age / m.appear);
            r.alpha = k;
            r.offset.y += (1.0f - k) * px(m.appear_offset);
            r.scale *= lerp(m.appear_scale, 1.0f, k);
        }
    }
    if (shaking) {
        const float t = mem.shaking / (m.shake > 0 ? m.shake : 1.0f);
        r.offset.x += std::sin(mem.shaking * 2.0f * std::numbers::pi_v<float> * 9.0f) *
            px(m.shake_strength) * (1.0f - t);
    }

    if (s.shadow) {
        r.has_shadow = true;
        r.shadow_offset = Vector2{ px(s.shadow->offset.x), px(s.shadow->offset.y) + lift };
        r.shadow_blur = px(std::max(0.0f, s.shadow->blur)) + lift;
        r.shadow_spread = px(s.shadow->spread);
        r.shadow_color = s.shadow->color;
    }

    d.userData = &r;
    return &r;
}

DrawRecord *picture_record(const Picture &picture, Color tint) {
    if (picture.empty()) return nullptr;
    DrawRecord &r = looks.draws.emplace_back();
    fill_picture(r, picture);
    r.tint = tint;
    return &r;
}

DrawRecord *raw_record(const Texture2D &texture, Color tint) {
    DrawRecord &r = looks.draws.emplace_back();
    r.has_picture = true;
    r.raw = texture;
    r.source = Rectangle{ 0, 0, static_cast<float>(texture.width), static_cast<float>(texture.height) };
    r.tint = tint;
    return &r;
}

void open_text(Clay_String text, Clay_TextElementConfig config) {
    const Style &s = look();
    if (s.font.valid()) config.fontId = font_id_of(s.font);
    if (s.text_outline || s.text_shadow) {
        TextRecord &t = looks.texts.emplace_back();
        if (s.text_outline && s.text_outline->width > 0) {
            t.outline = true;
            t.outline_px = std::max(1.0f, px(s.text_outline->width));
            t.outline_color = s.text_outline->color;
        }
        if (s.text_shadow) {
            t.shadow = true;
            t.shadow_px = Vector2{ px(s.text_shadow->offset.x), px(s.text_shadow->offset.y) };
            t.shadow_color = s.text_shadow->color;
        }
        config.userData = &t;
    }
    Clay__OpenTextElement(text, config);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void emit(EventKind kind, WidgetKind widget, std::string_view name) {
    const bool scoped = !looks.stack.empty();
    const Style resolved = scoped ? Style{} : resolve(widget, nullptr);
    const Style &s = scoped ? look() : resolved;
    if (const std::optional<std::string> sound = sound_for(s.sounds, kind);
        sound && !sound->empty()) {
        rmp::audio::play(*sound, rmp::audio::PlayOptions{ .volume = s.sounds.volume.value_or(1.0f) });
    }
    if (looks.hook) {
        const Event e{ kind, widget, name };
        looks.hook(e);
    }
}

Edges track(Clay_ElementId id, WidgetKind kind, std::string_view name, const Interaction &now) {
    Memory &m = remember(id.id);
    Edges e;
    e.hover = now.over && !m.over;
    e.unhover = !now.over && m.over;
    e.press = now.down && !m.down;
    e.release = !now.down && m.down;
    e.focus = now.focused && !m.focused;
    e.blur = !now.focused && m.focused;
    e.click = now.clicked;
    e.change = now.changed;
    m.over = now.over;
    m.down = now.down;
    m.focused = now.focused;
    if (e.hover) emit(EventKind::HOVER, kind, name);
    if (e.unhover) emit(EventKind::UNHOVER, kind, name);
    if (e.press) emit(EventKind::PRESS, kind, name);
    if (e.release) emit(EventKind::RELEASE, kind, name);
    if (e.click) emit(EventKind::CLICK, kind, name);
    if (e.focus) emit(EventKind::FOCUS, kind, name);
    if (e.blur) emit(EventKind::BLUR, kind, name);
    if (e.change) emit(EventKind::CHANGE, kind, name);
    return e;
}

void start_shake(uint32_t element, WidgetKind widget, std::string_view name) {
    remember(element).shaking = 0;
    emit(EventKind::SHAKE, widget, name);
}

} // namespace detail

void on_event(rmp::Callback<const Event &> hook) { detail::looks.hook = std::move(hook); }

} // namespace rmp::ui
