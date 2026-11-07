// ===========================================================================
// The dialogue presenters -- DialogueBox, DialogueBubble, DialogueStage (see
// rmp/ui.h): an rmp::Dialogue shown with rmp::ui's own widgets, so a theme and
// a style dress them. What they share is here once: the typewriter and its
// [speed] and [pause], the blips and the recorded voice, going on (a click,
// Enter, auto-advance, a line read before), timed choices and the history.
// ===========================================================================

#include "../dialogue/yarn.h"
#include "internal.h"

#include <rmp/dialogue.h>
#include <rmp/ui.h>

#include <rmp/random.h>

#include <clay.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>

namespace rmp::ui {

namespace {

constexpr float STAGE_ON_STAGE = 3; // speakers on stage at once
constexpr float SHAKE_HZ = 9;
constexpr float CHOICE_PANEL_WIDTH = 360; // design units, of the choices' own panel

int letters(std::string_view text) {
    int n = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    }
    return n;
}

yarn::Marked marked_of(const rmp::DialogueLine &line) {
    yarn::Marked m;
    m.text = line.text;
    for (const rmp::DialogueMarkup &a : line.markup) {
        m.attributes.push_back({ a.name, a.position, a.length, a.properties });
    }
    return m;
}

std::string tag_value(const std::vector<std::string> &tags, std::string_view key) {
    for (const std::string &t : tags) {
        if (t.size() > key.size() && t.starts_with(key) && t[key.size()] == ':') {
            return t.substr(key.size() + 1);
        }
    }
    return {};
}

rmp::Ref<const Picture> portrait_for(const std::vector<Portrait> &portraits,
                                     const std::string &speaker,
                                     const std::string &mood) {
    rmp::Ref<const Picture> any;
    for (const Portrait &p : portraits) {
        if (p.speaker != speaker) continue;
        if (!mood.empty() && p.mood == mood) return rmp::Ref<const Picture>(p.picture);
        if (p.mood.empty() && !any) any = rmp::Ref<const Picture>(p.picture);
    }
    return any;
}

// How fast the letter at `at` types, of [speed=2]..[/speed].
float speed_at(const rmp::DialogueLine &line, int at) {
    for (const rmp::DialogueMarkup &m : line.markup) {
        if (m.name == "speed" && at >= m.position && at < m.position + m.length) {
            const float s = std::strtof(m.property("speed").c_str(), nullptr);
            if (s > 0) return s;
        }
    }
    return 1.0f;
}

// A [pause=0.4/] waiting at `at`, in seconds.
float pause_at(const rmp::DialogueLine &line, int at) {
    for (const rmp::DialogueMarkup &m : line.markup) {
        if (m.name == "pause" && m.position == at) {
            return std::max(0.0f, std::strtof(m.property("pause").c_str(), nullptr));
        }
    }
    return 0;
}

bool seen(const DialogueMemory &m, const std::string &id) {
    return std::ranges::find(m.seen, id) != m.seen.end();
}

// The typewriter, a frame of it. True when the line is all shown.
bool type(DialogueMemory &m, const DialogueFlow &f, const rmp::DialogueLine &line,
          float pitch) {
    const std::string key = line.id + "\x1f" + line.text;
    const float dt = detail::frame_time();
    if (m.line_id != key) {
        m.line_id = key;
        m.shown = 0;
        m.waited = 0;
        m.blips = 0;
        m.length = letters(line.text);
        if (!f.voices.empty() && !line.id.empty()) {
            const std::string_view id = std::string_view(line.id).starts_with("line:")
                ? std::string_view(line.id).substr(5)
                : std::string_view(line.id);
            detail::play_sound(SoundName(f.voices + std::string(id)), 1.0f);
        }
        if (f.letters_per_second <= 0) m.shown = static_cast<float>(m.length);
    }
    if (m.shown < static_cast<float>(m.length)) {
        const int at = static_cast<int>(m.shown);
        const float wait = pause_at(line, at);
        if (wait > 0 && m.waited < wait) {
            m.waited += dt;
            return false;
        }
        m.waited = 0;
        m.shown = std::min(static_cast<float>(m.length),
                           m.shown + dt * f.letters_per_second * speed_at(line, at));
        const int typed = static_cast<int>(m.shown);
        if (!f.blip.empty() && f.blip_every > 0 && typed / f.blip_every > m.blips) {
            m.blips = typed / f.blip_every;
            const float jitter = std::isfinite(f.blip_jitter) ? std::clamp(f.blip_jitter, 0.0f, 0.9f)
                                                             : 0.0f;
            const float vary = jitter > 0 ? rmp::random::range(-jitter, jitter) : 0.0f;
            detail::play_sound(f.blip, 1.0f, pitch * (1.0f + vary));
        }
        if (m.shown < static_cast<float>(m.length)) return false;
        m.waited = 0;
        return true;
    }
    m.waited += dt;
    return true;
}

void go_on(DialogueMemory &m, rmp::Dialogue &d) {
    const rmp::DialogueLine &line = d.line();
    m.history.emplace_back(line.speaker, line.text);
    if (!seen(m, line.id)) m.seen.push_back(line.id);
    m.line_id.clear();
    d.advance();
}

// The player's answer to a line: true when the line went (and the frame's
// presenter should stop drawing it).
bool answer_line(DialogueMemory &m, const DialogueFlow &f, rmp::Dialogue &d, bool full,
                 bool pressed) {
    if (f.skip_seen && seen(m, d.line().id)) {
        go_on(m, d);
        return true;
    }
    if (pressed) {
        if (!full) {
            m.shown = static_cast<float>(m.length);
            return false;
        }
        go_on(m, d);
        return true;
    }
    if (full && f.auto_advance > 0 && m.waited >= f.auto_advance) {
        go_on(m, d);
        return true;
    }
    return false;
}

// The choices, as buttons; a timed choice's bar. True when one was taken.
bool show_choices(DialogueMemory &m, const DialogueFlow &f, rmp::Dialogue &d,
                  const Style &style) {
    const auto options = d.options();
    const bool any = std::ranges::any_of(options, &rmp::DialogueOption::available);
    if (!any) {
        d.choose(-1);
        return true;
    }
    std::string key = "options";
    for (const rmp::DialogueOption &o : options) key += "\x1f" + o.id;
    if (m.line_id != key) {
        m.line_id = key;
        m.choice_left = f.choice_seconds;
    }
    int chosen = -1;
    for (std::size_t i = 0; i < options.size(); ++i) {
        Button b{ .text = rmp::Text(std::string(options[i].text)),
                  .enabled = options[i].available,
                  .id = "dialogue_choice_" + std::to_string(i),
                  .style = style };
        if (b.show()) chosen = static_cast<int>(i);
    }
    if (f.choice_seconds > 0 && chosen < 0) {
        m.choice_left -= detail::frame_time();
        progress(std::clamp(m.choice_left / f.choice_seconds, 0.0f, 1.0f));
        if (m.choice_left <= 0) {
            chosen = f.default_choice;
            const bool ok = chosen >= 0 &&
                static_cast<std::size_t>(chosen) < options.size() &&
                options[static_cast<std::size_t>(chosen)].available;
            if (!ok) {
                for (std::size_t i = 0; i < options.size(); ++i) {
                    if (options[i].available) {
                        chosen = static_cast<int>(i);
                        break;
                    }
                }
            }
        }
    }
    if (chosen < 0) return false;
    m.history.emplace_back(std::string(),
                           std::string(options[static_cast<std::size_t>(chosen)].text));
    m.line_id.clear();
    d.choose(chosen);
    return true;
}

// The lines read so far, newest last, in a scroll.
void history(const DialogueMemory &m) {
    scroll({ .height = 240 }, [&] {
        for (const auto &[speaker, text] : m.history) {
            std::string said = speaker.empty() ? std::string("> ") : speaker + ": ";
            said += text;
            ui::text(rmp::Text(said));
        }
    });
}

// Where the dialogue is: gone, held by a command's wait, or with a line or
// choices to show. Commands nobody handled are passed.
enum class Now : std::uint8_t { GONE, WAITING, SHOWING };

Now settle(rmp::Dialogue &d) {
    for (int guard = 0; guard < 64; ++guard) {
        if (d.waiting()) return Now::WAITING;
        if (!d.has_command()) break;
        d.advance();
    }
    return d.running() ? Now::SHOWING : Now::GONE;
}

void name_plate(const std::string &speaker, const Style &style) {
    if (speaker.empty()) return;
    Label name{ .text = rmp::Text(speaker), .wrap = false, .style = style };
    name.show();
}

bool next_button(std::string_view id) {
    Button next{ .text = ">", .id = std::string(id) };
    return next.show();
}

rmp::Ref<const Speaker> speaker_of(const std::vector<Speaker> &speakers,
                                   const std::string &name) {
    for (const Speaker &s : speakers) {
        if (s.name == name) return rmp::Ref<const Speaker>(s);
    }
    return {};
}

// The flow with a speaker's blip, when they have one.
DialogueFlow voiced(const DialogueFlow &flow, const SoundName &blip) {
    DialogueFlow f = flow;
    if (!blip.empty()) f.blip = blip;
    return f;
}

// The line, typed out, and the player's answer to it: what every presenter
// and dialogue_line() draw.
void line_body(rmp::Dialogue &d, DialogueMemory &m, const DialogueFlow &f, float pitch,
               RichTextOptions o, std::string_view id, std::string_view next) {
    const rmp::DialogueLine &line = d.line();
    const bool full = type(m, f, line, pitch);
    o.visible = static_cast<int>(m.shown);
    if (o.id.empty()) o.id = id;
    detail::rich_with(marked_of(line), o, o.id);
    const bool pressed = next_button(next);
    answer_line(m, f, d, full, pressed);
}

// A box floating over its parent, `offset` design units from its top-left
// corner: a name plate on a frame's edge.
template <class Body> void over_parent(Vector2 offset, Body &&body) {
    Clay_ElementDeclaration d{};
    d.floating.attachTo = CLAY_ATTACH_TO_PARENT;
    d.floating.attachPoints = { CLAY_ATTACH_POINT_LEFT_TOP, CLAY_ATTACH_POINT_LEFT_TOP };
    d.floating.offset = { detail::px(offset.x), detail::px(offset.y) };
    d.floating.pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH;
    Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
    body();
    Clay__CloseElement();
}

// A box in the middle of the screen, over everything: the choices, a flash.
template <class Body> void over_screen(Clay_Sizing sizing, Body &&body) {
    Clay_ElementDeclaration d{};
    d.layout.sizing = sizing;
    d.floating.attachTo = CLAY_ATTACH_TO_ROOT;
    d.floating.attachPoints = { CLAY_ATTACH_POINT_CENTER_CENTER,
                                CLAY_ATTACH_POINT_CENTER_CENTER };
    d.floating.zIndex = 50;
    Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
    body();
    Clay__CloseElement();
}

float lerpf(float a, float b, float t) { return a + (b - a) * t; }

Color lerp_color(Color a, Color b, float t) {
    const auto mix = [t](unsigned char x, unsigned char y) {
        return static_cast<unsigned char>(std::lround(lerpf(x, y, t)));
    };
    return Color{ mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a) };
}

// 0..1, eased out: how far an entrance or a focus has gone.
float progress_of(float seconds, float over) {
    if (!(over > 0)) return 1;
    const float t = std::clamp(seconds / over, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t);
}

} // namespace

bool dialogue_line(rmp::Dialogue &dialogue, DialogueMemory &memory, const DialogueFlow &flow,
                   const RichTextOptions &text) {
    if (settle(dialogue) != Now::SHOWING || !dialogue.has_line()) return false;
    line_body(dialogue, memory, flow, 1.0f, text, "dialogue_line", "dialogue_next");
    return true;
}

bool dialogue_choices(rmp::Dialogue &dialogue, DialogueMemory &memory,
                      const DialogueFlow &flow, const Style &choice_style) {
    if (settle(dialogue) != Now::SHOWING || !dialogue.has_options()) return false;
    column({ .gap = 8, .grow_x = true },
           [&] { show_choices(memory, flow, dialogue, choice_style); });
    return true;
}

bool DialogueBox::show(rmp::Dialogue &dialogue) {
    const Now now = settle(dialogue);
    if (now == Now::GONE) return false;
    if (now == Now::WAITING) return true;
    const bool line = dialogue.has_line();
    const rmp::Ref<const Speaker> who =
        line ? speaker_of(speakers, dialogue.line().speaker) : rmp::Ref<const Speaker>{};
    const Style box_style = who ? detail::overlay(style, who->style) : style;
    const Style plate = who ? detail::overlay(name_style, who->name_style) : name_style;
    const DialogueFlow f = who ? voiced(flow, who->blip) : flow;
    const float pitch = who ? who->pitch : 1.0f;
    const bool apart = !line && dialogue.has_options() && choices != DialogueChoices::IN_BOX;

    const auto the_choices = [&] {
        Panel p{ .box = { .gap = 8, .width = CHOICE_PANEL_WIDTH }, .style = style };
        p.show([&] { show_choices(ours, flow, dialogue, choice_style); });
    };
    if (apart && choices == DialogueChoices::CENTER) {
        over_screen(Clay_Sizing{}, the_choices);
        return dialogue.running();
    }
    column({ .gap = 8, .items = Align::TOP_RIGHT, .grow_x = width <= 0, .width = width }, [&] {
        if (apart) the_choices(); // ABOVE: their own panel, over the box's right
        Panel box{ .row = true,
                   .box = { .gap = 16, .grow_x = true, .height = height },
                   .style = box_style };
        box.show([&] {
            if (ours.showing_history) {
                column({ .grow_x = true }, [&] { history(ours); });
                if (next_button("dialogue_history_close")) ours.showing_history = false;
                return;
            }
            if (line) {
                const rmp::DialogueLine &l = dialogue.line();
                const auto portrait =
                    portrait_for(portraits, l.speaker, tag_value(l.tags, "mood"));
                const auto picture = [&] {
                    if (!portrait) return;
                    column({ .id = "dialogue_portrait" }, [&] {
                        image(*portrait, { .width = portrait_size, .height = portrait_size });
                    });
                };
                const bool floating_name = name_offset.x != 0 || name_offset.y != 0;
                if (floating_name && !l.speaker.empty())
                    over_parent(name_offset, [&] { name_plate(l.speaker, plate); });
                if (portrait_side == DialogueSide::LEFT) picture();
                column({ .gap = 6, .items = Align::TOP_LEFT, .grow_x = true }, [&] {
                    if (!floating_name) name_plate(l.speaker, plate);
                    line_body(dialogue, ours, f, pitch, {}, "dialogue_line", "dialogue_next");
                });
                if (portrait_side == DialogueSide::RIGHT) picture();
            } else if (dialogue.has_options() && !apart) {
                column({ .gap = 8, .grow_x = true },
                       [&] { show_choices(ours, flow, dialogue, choice_style); });
            }
        });
    });
    return dialogue.running();
}

void DialogueBox::toggle_history() { ours.showing_history = !ours.showing_history; }

bool DialogueBubble::show(rmp::Dialogue &dialogue) {
    const Now now = settle(dialogue);
    if (now == Now::GONE) return false;
    if (now == Now::WAITING) return true;
    const auto bubble = [&](rmp::Ref<const BubbleSpeaker> who, bool pointing) {
        const Style look = who ? detail::overlay(style, who->style) : style;
        const DialogueFlow f = who ? voiced(flow, who->blip) : flow;
        column({ .gap = 0, .items = Align::TOP_CENTER }, [&] {
            Panel p{ .box = { .gap = 6, .items = Align::TOP_LEFT, .width = width },
                     .style = look };
            p.show([&] {
                line_body(dialogue, ours, f, who ? who->pitch : 1.0f,
                          RichTextOptions{ .width = width - 24 }, "dialogue_bubble_line",
                          "dialogue_bubble_next");
            });
            if (!pointing || !(tail > 0)) return;
            if (!tail_picture.empty()) {
                image(tail_picture, { .width = tail * 2, .height = tail });
                return;
            }
            Color c = tail_color;
            if (c.a == 0) {
                c = detail::resolved(WidgetKind::PANEL, look)
                        .background.value_or(current_theme().panel);
            }
            detail::open_tail(tail * 2, tail, c);
        });
    };
    if (dialogue.has_line()) {
        const rmp::DialogueLine &line = dialogue.line();
        for (const BubbleSpeaker &s : speakers) {
            if (s.name != line.speaker) continue;
            if (const auto o = s.object.get()) {
                const Vector2 world{ o->position.x + s.offset.x,
                                     o->position.y + s.offset.y };
                at(world,
                   AtOptions{ .anchor = Align::BOTTOM_CENTER, .id = "dialogue_bubble" },
                   [&] { bubble(rmp::Ref<const BubbleSpeaker>(s), true); });
                return dialogue.running();
            }
            bubble(rmp::Ref<const BubbleSpeaker>(s), false);
            return dialogue.running();
        }
        bubble({}, false); // nobody to point at: where the game put it
    } else if (dialogue.has_options()) {
        Panel p{ .box = { .gap = 8, .width = width }, .style = style };
        p.show([&] { show_choices(ours, flow, dialogue, choice_style); });
    }
    return dialogue.running();
}

void DialogueStage::shake(float strength, float seconds) {
    ours.shaking = 0;
    ours.shake_strength = std::isfinite(strength) ? std::max(0.0f, strength) : 0.0f;
    ours.shake_seconds = std::isfinite(seconds) ? std::max(0.0f, seconds) : 0.0f;
}

void DialogueStage::flash(Color color, float seconds) {
    ours.flashing = 0;
    ours.flash_color = color;
    ours.flash_seconds = std::isfinite(seconds) ? std::max(0.0f, seconds) : 0.0f;
}

bool DialogueStage::show(rmp::Dialogue &dialogue) {
    const Now now = settle(dialogue);
    if (now == Now::GONE) return false;
    if (now == Now::WAITING) return true;
    const float dt = detail::frame_time();
    std::string speaker;
    std::string mood;
    if (dialogue.has_line()) {
        speaker = dialogue.line().speaker;
        mood = tag_value(dialogue.line().tags, "mood");
        if (!speaker.empty() &&
            std::ranges::find(ours.stage, speaker) == ours.stage.end()) {
            ours.stage.push_back(speaker);
            ours.entered.push_back(0);
            ours.focus.push_back(0);
            if (static_cast<float>(ours.stage.size()) > STAGE_ON_STAGE) {
                ours.stage.erase(ours.stage.begin());
                ours.entered.erase(ours.entered.begin());
                ours.focus.erase(ours.focus.begin());
            }
        }
    }
    ours.entered.resize(ours.stage.size(), enter_seconds);
    ours.focus.resize(ours.stage.size(), 0);
    const float step = focus_seconds > 0 ? dt / focus_seconds : 1.0f;
    for (std::size_t i = 0; i < ours.stage.size(); ++i) {
        ours.entered[i] += dt;
        const float target = ours.stage[i] == speaker ? 1.0f : 0.0f;
        ours.focus[i] = target > ours.focus[i] ? std::min(target, ours.focus[i] + step)
                                               : std::max(target, ours.focus[i] - step);
    }
    Vector2 shaken{ 0, 0 };
    if (ours.shaking >= 0) {
        ours.shaking += dt;
        if (ours.shaking >= ours.shake_seconds) {
            ours.shaking = -1;
        } else {
            const float t = ours.shaking / std::max(ours.shake_seconds, 1e-6f);
            shaken.x = std::sin(ours.shaking * 2.0f * std::numbers::pi_v<float> * SHAKE_HZ) *
                ours.shake_strength * (1.0f - t);
        }
    }
    const rmp::Ref<const Speaker> who = speaker_of(speakers, speaker);
    const Style box_style = who ? detail::overlay(style, who->style) : style;
    const Style plate = who ? detail::overlay(name_style, who->name_style) : name_style;
    const DialogueFlow f = who ? voiced(flow, who->blip) : flow;

    detail::open_moved(shaken, 1.0f, 1.0f);
    column({ .gap = 0, .grow_x = true }, [&] {
        // The cast, left to right: the first half come in from the left, the
        // rest from the right; whoever speaks comes forward.
        row({ .gap = 24, .items = Align::BOTTOM_CENTER, .grow_x = true }, [&] {
            const std::size_t n = ours.stage.size();
            for (std::size_t i = 0; i < n; ++i) {
                const std::string &cast = ours.stage[i];
                const bool speaking = cast == speaker;
                const auto p = portrait_for(portraits, cast, speaking ? mood : std::string());
                if (!p) continue;
                const float in = progress_of(ours.entered[i], enter_seconds);
                const float from_left = 2 * i + 1 < n || n == 1 ? -1.0f : 1.0f;
                const float k = ours.focus[i];
                const Vector2 moved{ from_left * enter_offset * (1.0f - in), -focus_lift * k };
                detail::open_moved(moved, in, lerpf(1.0f, focus_scale, k));
                image(*p, { .height = portrait_height, .tint = lerp_color(dimmed, WHITE, k) });
                detail::close_moved();
            }
        });
        Panel box{ .box = { .gap = 6, .items = Align::TOP_LEFT, .grow_x = true },
                   .style = box_style };
        box.show([&] {
            if (dialogue.has_line()) {
                name_plate(dialogue.line().speaker, plate);
                line_body(dialogue, ours, f, who ? who->pitch : 1.0f, {},
                          "dialogue_stage_line", "dialogue_stage_next");
            } else if (dialogue.has_options()) {
                show_choices(ours, flow, dialogue, choice_style);
            }
        });
    });
    detail::close_moved();
    if (ours.flashing >= 0) {
        ours.flashing += dt;
        const float t = ours.flashing / std::max(ours.flash_seconds, 1e-6f);
        if (t >= 1) {
            ours.flashing = -1;
        } else {
            Color c = ours.flash_color;
            c.a = static_cast<unsigned char>(std::lround(c.a * (1.0f - t)));
            over_screen(Clay_Sizing{ CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) }, [&] {
                Panel veil{ .box = { .grow_x = true, .grow_y = true },
                            .style = { .background = c, .radius = 0.0f } };
                veil.show([] {});
            });
        }
    }
    return dialogue.running();
}

} // namespace rmp::ui
