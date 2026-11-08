// ---------------------------------------------------------------------------
// Properties of rmp::Dialogue (src/rmp/dialogue/): any script built of Yarn's
// statements compiles or says why; whatever the player picks it never
// crashes and ends or keeps asking; its variables survive a save; its bytes
// read back; a presenter shows whatever it is handed; and a Talker is in
// range exactly when a model of it says so.
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"

#include "../../src/rmp/dialogue/dialogue_internal.h"
#include "../../src/rmp/dialogue/yarn.h"
#include "../../src/rmp/internal.h"
#include "../../src/rmp/object_internal.h"
#include "../../src/rmp/text_internal.h"
#include "../../src/rmp/ui/internal.h"

#include <rmp/behavior.h>
#include <rmp/dialogue.h>
#include <rmp/save.h>
#include <rmp/tilemap.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

#include <doctest.h>
#include <rapidcheck.h>

#include <cmath>
#include <string>
#include <vector>

// covers: rmp::Dialogue::Dialogue()
// covers: rmp::Dialogue::Dialogue(Dialogue &&)
// covers: rmp::Dialogue::Dialogue(std::unique_ptr<detail::DialogueState>)
// covers: rmp::Dialogue::advance()
// covers: rmp::Dialogue::choose(int)
// covers: rmp::Dialogue::command() const
// covers: rmp::Dialogue::errors() const
// covers: rmp::Dialogue::has_command() const
// covers: rmp::Dialogue::has_line() const
// covers: rmp::Dialogue::has_node(std::string_view) const
// covers: rmp::Dialogue::has_options() const
// covers: rmp::Dialogue::line() const
// covers: rmp::Dialogue::load(const rmp::Value &)
// covers: rmp::Dialogue::loaded() const
// covers: rmp::Dialogue::node() const
// covers: rmp::Dialogue::operator=(Dialogue &&)
// covers: rmp::Dialogue::options() const
// covers: rmp::Dialogue::running() const
// covers: rmp::Dialogue::save(rmp::Value &) const
// covers: rmp::Dialogue::set_saliency(DialogueSaliency)
// covers: rmp::Dialogue::set_variable(std::string_view, const DialogueValue &)
// covers: rmp::Dialogue::start(std::string_view)
// covers: rmp::Dialogue::stop()
// covers: rmp::Dialogue::variable(std::string_view) const
// covers: rmp::Dialogue::visits(std::string_view) const
// covers: rmp::Dialogue::~Dialogue()
// covers: rmp::DialogueMarkup::property(std::string_view) const
// covers: rmp::DialogueValue::DialogueValue [template]
// covers: rmp::DialogueValue::DialogueValue(bool)
// covers: rmp::DialogueValue::DialogueValue(double)
// covers: rmp::DialogueValue::DialogueValue(float)
// covers: rmp::DialogueValue::DialogueValue(int)
// covers: rmp::DialogueValue::DialogueValue(std::string)
// covers: rmp::DialogueValue::as_bool() const
// covers: rmp::DialogueValue::as_number() const
// covers: rmp::DialogueValue::as_string() const
// covers: rmp::DialogueValue::kind() const
// covers: rmp::assets::load_dialogue(std::span<const std::string_view>)
// covers: rmp::assets::load_dialogue(std::string_view)
// covers: rmp::behavior::Talker::_update(Object &, float)
// covers: rmp::behavior::Talker::conversation()
// covers: rmp::behavior::Talker::in_range() const
// covers: rmp::behavior::Talker::talking() const
// covers: rmp::ui::DialogueBox::show(rmp::Dialogue &)
// covers: rmp::ui::DialogueBox::toggle_history()
// covers: rmp::ui::DialogueBubble::show(rmp::Dialogue &)
// covers: rmp::ui::DialogueStage::show(rmp::Dialogue &)
// covers: rmp::ui::rich_text(const rmp::Text &)
// covers: rmp::ui::rich_text(const rmp::Text &, const RichTextOptions &)
// covers: rmp::ui::rich_text_length(const rmp::Text &)

namespace {

// A statement of Yarn, from the pieces of the language.
rc::Gen<std::string> statement() {
    return rc::gen::element<std::string>(
        "Mika: Hello, [b]you[/b]!", "Line with {$n} and {$s}.", "<<set $n += 1>>",
        "<<set $s = \"x\" + $s>>", "<<set $b = !$b>>",
        "<<if $n > 2>>\nBig.\n<<else>>\nSmall.\n<<endif>>",
        "-> One <<if $b>>\n    Picked one.\n-> Two\n    <<jump Other>>\n-> Three "
        "<<once>>",
        "=> Group A\n=> Group B <<if $n > 1>>\n=> Group C",
        "<<once>>\nOnce only.\n<<endonce>>", "<<detour Other>>", "<<wave 1 2>>",
        "<<custom thing>>", "Visited: {visited(\"Other\")}",
        "Count: {visited_count(\"Start\")} {round(1.5)} {floor(2.7)} {dice(6) > 0}",
        R"([plural value={$n} one="% gem" other="% gems"/])", "<<return>>",
        "Mood. #mood:sad");
}

std::string script(const std::vector<std::string> &start,
                   const std::vector<std::string> &other) {
    std::string s = "title: Start\n---\n<<declare $n = 0>>\n<<declare $s = "
                    "\"\">>\n<<declare $b = false>>\n";
    for (const std::string &st : start) s += st + "\n";
    s += "===\ntitle: Other\n---\n";
    for (const std::string &st : other) s += st + "\n";
    s += "===\n";
    return s;
}

class World : public rmp::Scene {
public:
    ~World() override { rmp::objects::detail::release_scene(*this); }
};

void quiet_lang() {
    rmp::lang::detail::reset_for_tests();
    rmp::lang::detail::set_persist(false);
    rmp::lang::detail::set_system_locale_for_tests("en");
    rmp::lang::detail::load_tables({});
}

} // namespace

TEST_SUITE("property: dialogue") {
    TEST_CASE("any script of Yarn's statements runs to its end, whatever is chosen") {
        rmp::test::check("compile, run, choose anything, save, load, bytes", [] {
            quiet_lang();
            SetTraceLogLevel(LOG_NONE);
            const auto start = *rc::gen::resize(
                8, rc::gen::container<std::vector<std::string>>(statement()));
            const auto other = *rc::gen::resize(
                4, rc::gen::container<std::vector<std::string>>(statement()));
            const std::string source = script(start, other);
            rmp::Dialogue d = rmp::dialogue::detail::compile_text(source);
            RC_ASSERT(d.loaded() == d.errors().empty());
            if (!d.loaded()) return;
            RC_ASSERT(d.has_node("Start"));
            RC_ASSERT(d.has_node("Other"));
            int waves = 0;
            d.on_command("wave", [&](std::span<const std::string> args) {
                RC_ASSERT(args.size() == 2U);
                ++waves;
            });
            d.on_function("unused",
                          [](rmp::DialogueValue &out,
                             std::span<const rmp::DialogueValue>) { out = 1; });
            d.set_saliency(*rc::gen::element(rmp::DialogueSaliency::FIRST,
                                             rmp::DialogueSaliency::BEST,
                                             rmp::DialogueSaliency::LEAST_SEEN,
                                             rmp::DialogueSaliency::RANDOM_LEAST_SEEN));
            RC_ASSERT(d.start());
            const int pick = *rc::gen::inRange(0, 3);
            for (int step = 0; step < 300 && d.running(); ++step) {
                RC_ASSERT(d.has_line() || d.has_options() || d.has_command());
                RC_ASSERT(!d.node().empty());
                if (d.has_line()) {
                    (void)d.line().text;
                    for (const rmp::DialogueMarkup &m : d.line().markup)
                        (void)m.property("value");
                    d.advance();
                } else if (d.has_options()) {
                    const auto opts = d.options();
                    int chosen = -1;
                    for (std::size_t k = 0; k < opts.size(); ++k) {
                        if (opts[(k + static_cast<std::size_t>(pick)) % opts.size()]
                                .available) {
                            chosen = static_cast<int>(
                                (k + static_cast<std::size_t>(pick)) % opts.size());
                            break;
                        }
                    }
                    d.choose(chosen);
                } else {
                    RC_ASSERT(!d.command().empty());
                    d.advance();
                }
            }
            RC_ASSERT(d.visits("Start") >= 0);
            d.set_variable("$b", rmp::DialogueValue(true));
            RC_ASSERT(d.variable("$b").as_bool());
            rmp::Value saved;
            d.save(saved);
            rmp::Dialogue again = rmp::dialogue::detail::compile_text(source);
            again.load(saved);
            RC_ASSERT(again.variable("$n").as_number() == d.variable("$n").as_number());
            RC_ASSERT(again.variable("$s").as_string() == d.variable("$s").as_string());
            RC_ASSERT(again.variable("$b").as_bool() == d.variable("$b").as_bool());
            d.stop();
            RC_ASSERT(!d.running());
            rmp::Dialogue moved(std::move(again));
            rmp::Dialogue assigned;
            assigned = std::move(moved);
            RC_ASSERT(assigned.loaded());
            (void)waves;
        });
    }

    TEST_CASE("Yarn's values convert as the language says, whatever they hold") {
        rmp::test::check("number, bool, string", [] {
            const float f = *rmp::test::gen::hostile_float();
            const rmp::DialogueValue n(f);
            RC_ASSERT(n.kind() == rmp::DialogueValue::Kind::NUMBER);
            RC_ASSERT(n.as_bool() == (f != 0));
            RC_ASSERT(!n.as_string().empty());
            const int i = *rc::gen::inRange(-100000, 100000);
            RC_ASSERT(rmp::DialogueValue(i).as_number() == static_cast<float>(i));
            RC_ASSERT(rmp::DialogueValue(static_cast<double>(i)).as_number() ==
                      static_cast<float>(i));
            const bool b = *rc::gen::arbitrary<bool>();
            RC_ASSERT(rmp::DialogueValue(b).as_number() == (b ? 1.0f : 0.0f));
            RC_ASSERT(rmp::DialogueValue(b).as_string() == (b ? "True" : "False"));
            const std::string s = *rmp::test::gen::hostile_string();
            const rmp::DialogueValue t(s);
            RC_ASSERT(t.kind() == rmp::DialogueValue::Kind::STRING);
            RC_ASSERT(t.as_string() == s);
            (void)t.as_number();
            (void)t.as_bool();
            RC_ASSERT(rmp::DialogueValue("text").as_string() == "text");
        });
    }

    TEST_CASE("any bytes are a program or refused, never read past") {
        rmp::test::check("from_bytes of any bytes", [] {
            const auto bytes = *rc::gen::container<std::vector<std::uint8_t>>(
                rc::gen::arbitrary<std::uint8_t>());
            if (auto p = rmp::yarn::from_bytes(bytes)) {
                RC_ASSERT(rmp::yarn::to_bytes(*p) == bytes);
            }
            const rmp::Dialogue d = rmp::dialogue::detail::from_program(bytes);
            (void)d.loaded();
        });
    }

    TEST_CASE("the presenters and rich_text show whatever they are handed") {
        rmp::ui::detail::set_test_viewport(800, 450);
        rmp::ui::detail::set_test_frame_time(1.0f / 30);
        rmp::test::check("any script, any presenter, any frames", [] {
            quiet_lang();
            const auto start = *rc::gen::resize(
                6, rc::gen::container<std::vector<std::string>>(statement()));
            rmp::Dialogue d =
                rmp::dialogue::detail::compile_text(script(start, { "Bye." }));
            if (!d.loaded()) return;
            d.start();
            rmp::ui::DialogueBox box{ .flow = { .letters_per_second =
                                                    *rc::gen::element(0.0f, 30.0f, 1e9f),
                                                .auto_advance = 0.01f,
                                                .choice_seconds = 0.05f } };
            rmp::ui::DialogueBubble bubble{ .flow = { .auto_advance = 0.01f,
                                                      .choice_seconds = 0.05f } };
            rmp::ui::DialogueStage stage{ .flow = { .auto_advance = 0.01f,
                                                    .skip_seen = true,
                                                    .choice_seconds = 0.05f } };
            const int which = *rc::gen::inRange(0, 3);
            for (int f = 0; f < 120 && d.running(); ++f) {
                rmp::ui::begin();
                if (which == 0) box.show(d);
                if (which == 1) bubble.show(d);
                if (which == 2) stage.show(d);
                rmp::ui::rich_text(rmp::Text(*rmp::test::gen::hostile_string()),
                                   { .visible = f });
                rmp::ui::rich_text("[wave]a[/wave]");
                rmp::ui::end();
                if (f % 17 == 0) box.toggle_history();
            }
            RC_ASSERT(rmp::ui::rich_text_length("[b]ab[/b]c") == 3);
        });
        rmp::ui::detail::set_test_frame_time(0);
    }

    TEST_CASE("a Talker is in range exactly when the distance says so") {
        rmp::test::check("in_range, talking, conversation", [] {
            rmp::objects::detail::reset_for_tests();
            rmp::objects::detail::reset_behaviors_for_tests();
            World world;
            const float range =
                *rc::gen::element(0.0f, 10.0f, 50.0f, std::nanf(""), INFINITY);
            const float x = static_cast<float>(*rc::gen::inRange(-200, 200));
            const rmp::Object &hero = world.spawn({ .position = { x, 0 } });
            rmp::Object &npc = world.spawn({ .position = { 0, 0 } });
            npc.add<rmp::behavior::Talker>({ .range = range, .player = hero.handle<>() });
            rmp::objects::detail::update(world, 1.0f / 60);
            const auto t = npc.get<rmp::behavior::Talker>();
            RC_ASSERT(t->in_range() == (std::isfinite(range) && std::fabs(x) <= range));
            RC_ASSERT(!t->talking()); // no dialogue named: nothing to say
            RC_ASSERT(!t->conversation().loaded());
            // The hook itself, on a Talker of its own: the same answer.
            rmp::behavior::Talker own{ .range = range, .player = hero.handle<>() };
            own._update(npc, 1.0f / 60);
            RC_ASSERT(own.in_range() == t->in_range());
            rmp::objects::detail::reset_for_tests();
        });
    }

    TEST_CASE("load_dialogue of names that are no file is an error, never a crash") {
        rmp::test::check("load_dialogue of any name", [] {
            SetTraceLogLevel(LOG_NONE);
            const std::string name = *rmp::test::gen::hostile_string();
            const rmp::Dialogue one = rmp::assets::load_dialogue(name + ".none.yarn");
            RC_ASSERT(!one.loaded());
            const std::vector<std::string_view> names{ "a.none.yarn", "b.none.yarn" };
            RC_ASSERT(!rmp::assets::load_dialogue(names).loaded());
        });
    }
}
