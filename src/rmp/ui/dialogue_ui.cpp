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

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>

namespace rmp::ui {

namespace {

constexpr float BOX_PORTRAIT = 96; // design units
constexpr float STAGE_ON_STAGE = 3; // speakers on stage at once
constexpr float FULL_ALPHA = 255;

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
                                     const std::string &speaker, const std::string &mood) {
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
bool type(DialogueMemory &m, const DialogueFlow &f, const rmp::DialogueLine &line) {
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
            detail::play_sound(f.blip, 1.0f);
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
bool choices(DialogueMemory &m, const DialogueFlow &f, rmp::Dialogue &d, const Style &style) {
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
            const bool ok = chosen >= 0 && static_cast<std::size_t>(chosen) < options.size() &&
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
    m.history.emplace_back(std::string(), std::string(options[static_cast<std::size_t>(chosen)].text));
    m.line_id.clear();
    d.choose(chosen);
    return true;
}

// The lines read so far, newest last, in a scroll.
void history(const DialogueMemory &m) {
    scroll({ .height = 240 }, [&] {
        for (const auto &[speaker, text] : m.history) {
            ui::text(rmp::Text(speaker.empty() ? "> " + text : speaker + ": " + text));
        }
    });
}

// Commands nobody handled are passed; true while there is something to show.
bool settle(rmp::Dialogue &d) {
    int guard = 0;
    while (d.has_command() && guard++ < 64) d.advance();
    return d.running();
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

} // namespace

bool DialogueBox::show(rmp::Dialogue &dialogue) {
    if (!settle(dialogue)) return false;
    Panel box{ .row = true, .box = { .gap = 16, .grow_x = true, .height = height }, .style = style };
    box.show([&] {
        if (ours.showing_history) {
            column({ .grow_x = true }, [&] { history(ours); });
            if (next_button("dialogue_history_close")) ours.showing_history = false;
            return;
        }
        if (dialogue.has_line()) {
            const rmp::DialogueLine &line = dialogue.line();
            if (const auto p = portrait_for(portraits, line.speaker, tag_value(line.tags, "mood"))) {
                image(*p, { .width = BOX_PORTRAIT, .height = BOX_PORTRAIT });
            }
            const bool full = type(ours, flow, line);
            bool pressed = false;
            column({ .gap = 6, .items = Align::TOP_LEFT, .grow_x = true }, [&] {
                name_plate(line.speaker, name_style);
                detail::rich_with(marked_of(line), RichTextOptions{ .visible = static_cast<int>(ours.shown) },
                                  "dialogue_line");
            });
            pressed = next_button("dialogue_next");
            answer_line(ours, flow, dialogue, full, pressed);
        } else if (dialogue.has_options()) {
            column({ .gap = 8, .grow_x = true }, [&] { choices(ours, flow, dialogue, choice_style); });
        }
    });
    return dialogue.running();
}

void DialogueBox::toggle_history() { ours.showing_history = !ours.showing_history; }

bool DialogueBubble::show(rmp::Dialogue &dialogue) {
    if (!settle(dialogue)) return false;
    const auto bubble = [&](const rmp::DialogueLine &line) {
        Panel p{ .box = { .gap = 6, .items = Align::TOP_LEFT, .width = width }, .style = style };
        p.show([&] {
            const bool full = type(ours, flow, line);
            detail::rich_with(marked_of(line),
                              RichTextOptions{ .width = width - 24, .visible = static_cast<int>(ours.shown) },
                              "dialogue_bubble_line");
            const bool pressed = next_button("dialogue_bubble_next");
            answer_line(ours, flow, dialogue, full, pressed);
        });
    };
    if (dialogue.has_line()) {
        const rmp::DialogueLine line = dialogue.line();
        for (const BubbleSpeaker &s : speakers) {
            if (s.name != line.speaker) continue;
            if (const auto o = s.object.get()) {
                const Vector2 world{ o->position.x + s.offset.x, o->position.y + s.offset.y };
                at(world, AtOptions{ .anchor = Align::BOTTOM_CENTER, .id = "dialogue_bubble" },
                   [&] { bubble(line); });
                return dialogue.running();
            }
        }
        bubble(line); // nobody to point at: where the game put it
    } else if (dialogue.has_options()) {
        Panel p{ .box = { .gap = 8, .width = width }, .style = style };
        p.show([&] { choices(ours, flow, dialogue, choice_style); });
    }
    return dialogue.running();
}

bool DialogueStage::show(rmp::Dialogue &dialogue) {
    if (!settle(dialogue)) return false;
    std::string speaker;
    std::string mood;
    if (dialogue.has_line()) {
        speaker = dialogue.line().speaker;
        mood = tag_value(dialogue.line().tags, "mood");
        if (!speaker.empty() && std::ranges::find(ours.stage, speaker) == ours.stage.end()) {
            ours.stage.push_back(speaker);
            if (static_cast<float>(ours.stage.size()) > STAGE_ON_STAGE) ours.stage.erase(ours.stage.begin());
        }
    }
    column({ .gap = 0, .grow_x = true }, [&] {
        // The cast, left to right; whoever speaks in full colour.
        row({ .gap = 24, .items = Align::BOTTOM_CENTER, .grow_x = true }, [&] {
            for (const std::string &who : ours.stage) {
                const bool speaking = who == speaker;
                if (const auto p = portrait_for(portraits, who, speaking ? mood : std::string())) {
                    image(*p, { .height = portrait_height, .tint = speaking ? WHITE : dimmed });
                }
            }
        });
        Panel box{ .box = { .gap = 6, .items = Align::TOP_LEFT, .grow_x = true }, .style = style };
        box.show([&] {
            if (dialogue.has_line()) {
                const rmp::DialogueLine &line = dialogue.line();
                const bool full = type(ours, flow, line);
                name_plate(line.speaker, name_style);
                detail::rich_with(marked_of(line), RichTextOptions{ .visible = static_cast<int>(ours.shown) },
                                  "dialogue_stage_line");
                const bool pressed = next_button("dialogue_stage_next");
                answer_line(ours, flow, dialogue, full, pressed);
            } else if (dialogue.has_options()) {
                choices(ours, flow, dialogue, choice_style);
            }
        });
    });
    (void)FULL_ALPHA;
    return dialogue.running();
}

} // namespace rmp::ui
