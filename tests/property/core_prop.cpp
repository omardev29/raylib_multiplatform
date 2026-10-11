// ---------------------------------------------------------------------------
// The small ones, held to what they promise over any sequence of calls:
//
//   - report_once: every (site, key) is said once, and is one line, heard by
//     the sink in the order it was first said; [dev] strict stops once per
//     new line, after the sink heard it; a key given as bytes is every byte
//     of it, a C string is its bytes up to its end, and the plain form is the
//     empty key; a line longer than the log's buffer is cut, never overrun;
//   - rmp::global<T>(): one instance per type, the same one until the
//     shutdown, built by default the first time it is asked for, its state
//     kept between asks, and destroyed in the reverse of first use;
//   - rmp::ads: off Android, any sequence of calls loads nothing and earns
//     nothing (the stubs of thirdparty/raymob/admob.h).
//
// covers: rmp::global [template]
// covers: rmp::ads::request_interstitial()
// covers: rmp::ads::is_interstitial_loaded()
// covers: rmp::ads::show_interstitial()
// covers: rmp::ads::request_rewarded()
// covers: rmp::ads::is_rewarded_loaded()
// covers: rmp::ads::show_rewarded()
// covers: rmp::ads::take_reward_earned()
// covers: rmp::ads::reward_amount()
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"

#include "../../src/rmp/internal.h"

#include <rmp/ads.h>
#include <rmp/app.h>
#include <rmp/object.h> // rmp::Callback, what the report sink is

#include <doctest.h>
#include <rapidcheck.h>
#include <raylib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// --- report_once --------------------------------------------------------------

// Four sites, as four expansions of RMP_REPORT_ONCE would be.
constexpr std::array<char, 4> SITES{};

// The log's buffer: a line is cut to one byte less.
constexpr std::size_t LINE = 1024;

struct {
    std::vector<std::string> lines;
    std::vector<int> stops_before;
    int stops = 0;
} heard;

void count_stop() { ++heard.stops; }

// One report: from which site, keyed how, by what, saying what.
enum class Form { PLAIN, C_STRING, BYTES };
struct Said {
    int site = 0;
    Form form = Form::PLAIN;
    std::string key;
    std::string text;
};

// How RapidCheck prints one in a counterexample: the bytes escaped.
// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
void showValue(const Said &said, std::ostream &os) {
    static constexpr std::array<const char *, 3> FORMS = { "plain", "C string", "bytes" };
    os << "{ site " << said.site << ", " << FORMS.at(static_cast<std::size_t>(said.form))
       << ", key ";
    rmp::showValue(rmp::Value(said.key), os);
    os << ", " << said.text.size() << " bytes of text }";
}

// The key the table files a report under, worked out a second way.
std::string filed_as(const Said &said) {
    if (said.form == Form::PLAIN) return {};
    if (said.form == Form::C_STRING) return said.key.substr(0, said.key.find('\0'));
    return said.key;
}

rc::Gen<Said> any_report() {
    using namespace std::string_literals;
    return rc::gen::exec([] {
        Said said;
        said.site = *rc::gen::inRange(0, static_cast<int>(SITES.size()));
        said.form = *rc::gen::element(Form::PLAIN, Form::C_STRING, Form::BYTES);
        // Keys that collide in each of the ways the table must tell apart.
        said.key = *rc::gen::weightedOneOf<std::string>({
            { 1, rmp::test::gen::hostile_string() },
            { 2,
              rc::gen::element<std::string>(""s, "a"s, "a\0b"s, "a\0c"s, "\0"s,
                                            "jump"s) },
        });
        said.text = *rc::gen::weightedOneOf<std::string>({
            { 3, rmp::test::gen::hostile_string() },
            { 1,
              rc::gen::map(rc::gen::inRange<std::size_t>(0, 3 * LINE),
                           [](std::size_t n) { return std::string(n, 'x'); }) },
        });
        return said;
    });
}

void say(const Said &said) {
    const void *site = &SITES.at(static_cast<std::size_t>(said.site));
    switch (said.form) {
        case Form::PLAIN:
            rmp::detail::report_once(site, "%s", said.text.c_str());
            break;
        case Form::C_STRING:
            rmp::detail::report_once_keyed(site, said.key.c_str(), "%s",
                                           said.text.c_str());
            break;
        case Form::BYTES:
            rmp::detail::report_once_keyed(site, std::string_view(said.key), "%s",
                                           said.text.c_str());
            break;
    }
}

// What the log line of `text` is: up to its NUL, cut to the buffer.
std::string line_of(const std::string &text) {
    const std::string up_to_nul = text.substr(0, text.find('\0'));
    return up_to_nul.substr(0, LINE - 1);
}

// --- rmp::global ----------------------------------------------------------------

struct {
    std::string built;
    std::string gone;
} life;

// Three types that say when they are built and when they go.
template <char NAME> struct Kept {
    Kept() { life.built += NAME; }
    Kept(const Kept &) = delete;
    Kept &operator=(const Kept &) = delete;
    Kept(Kept &&) = delete;
    Kept &operator=(Kept &&) = delete;
    ~Kept() { life.gone += NAME; }
    int value = 0;
};
using A = Kept<'a'>;
using B = Kept<'b'>;
using C = Kept<'c'>;

// rmp::global<T>() of the type a number names.
int &value_of(int which) {
    if (which == 0) return rmp::global<A>().value;
    if (which == 1) return rmp::global<B>().value;
    return rmp::global<C>().value;
}
const void *address_of(int which) {
    if (which == 0) return &rmp::global<A>();
    if (which == 1) return &rmp::global<B>();
    return &rmp::global<C>();
}

} // namespace

TEST_SUITE("property: core") {
    TEST_CASE("report_once says every site and key once, to the log and the sink") {
        SetTraceLogLevel(LOG_NONE);
        rmp::test::check(
            "report_once files a report by its site and every byte of its key, once", [] {
                rmp::detail::reset_reports_for_tests();
                heard.lines.clear();
                heard.stops_before.clear();
                heard.stops = 0;
                const bool strict = *rc::gen::arbitrary<bool>();
                const rmp::detail::StrictHandler previous =
                    rmp::detail::set_strict_handler(count_stop);
                rmp::detail::set_strict(strict);
                rmp::detail::set_report_sink([](std::string_view line) {
                    heard.lines.emplace_back(line);
                    heard.stops_before.push_back(heard.stops);
                });

                std::set<std::pair<int, std::string>> filed;
                std::vector<std::string> expected;
                const int count = *rc::gen::inRange(1, 24);
                for (int i = 0; i < count; ++i) {
                    const Said said = *any_report();
                    say(said);
                    if (filed.emplace(said.site, filed_as(said)).second) {
                        expected.push_back(line_of(said.text));
                    }
                    RC_ASSERT(rmp::detail::report_count() ==
                              static_cast<int>(filed.size()));
                }

                rmp::detail::set_report_sink({});
                rmp::detail::set_strict(false);
                rmp::detail::set_strict_handler(previous);
                rmp::detail::reset_reports_for_tests();

                RC_ASSERT(heard.lines == expected);
                RC_ASSERT(heard.stops ==
                          (strict ? static_cast<int>(expected.size()) : 0));
                // Each line was heard before strict stopped for it.
                for (std::size_t i = 0; i < heard.stops_before.size(); ++i) {
                    RC_ASSERT(heard.stops_before[i] ==
                              (strict ? static_cast<int>(i) : 0));
                }
            });
        SetTraceLogLevel(LOG_INFO);
    }

    TEST_CASE("rmp::global keeps one instance a type until the shutdown") {
        rmp::test::check(
            "global<T>() is one instance a type, kept, destroyed in reverse of first use",
            [] {
                rmp::app::detail::shutdown_globals();
                life.built.clear();
                life.gone.clear();

                std::vector<int> order; // first use, since the last shutdown
                std::array<int, 3> model{};
                std::array<const void *, 3> where{};
                std::string built;
                std::string gone;
                const int steps = *rc::gen::inRange(1, 30);
                for (int i = 0; i < steps; ++i) {
                    const int which = *rc::gen::inRange(0, 4);
                    if (which == 3) {
                        rmp::app::detail::shutdown_globals();
                        for (auto it = order.rbegin(); it != order.rend(); ++it) {
                            gone += static_cast<char>('a' + *it);
                        }
                        order.clear();
                        model = {};
                        where = {};
                        RC_ASSERT(life.gone == gone);
                        continue;
                    }
                    const auto slot = static_cast<std::size_t>(which);
                    if (std::ranges::find(order, which) == order.end()) {
                        order.push_back(which);
                        built += static_cast<char>('a' + which);
                        // Built by default, the first time it is asked for.
                        RC_ASSERT(value_of(which) == 0);
                        where.at(slot) = address_of(which);
                    }
                    RC_ASSERT(life.built == built);
                    const bool same = address_of(which) == where.at(slot);
                    RC_ASSERT(same);
                    RC_ASSERT(value_of(which) == model.at(slot));
                    const int next = *rc::gen::arbitrary<int>();
                    value_of(which) = next;
                    model.at(slot) = next;
                }
                rmp::app::detail::shutdown_globals();
                for (auto it = order.rbegin(); it != order.rend(); ++it) {
                    gone += static_cast<char>('a' + *it);
                }
                RC_ASSERT(life.gone == gone);
                RC_ASSERT(life.built.size() == life.gone.size());
            });
    }

    TEST_CASE("rmp::ads off Android loads nothing and earns nothing") {
        rmp::test::check(
            "every rmp::ads call off Android is a no-op that loads and earns nothing",
            [] {
                const int steps = *rc::gen::inRange(1, 20);
                for (int i = 0; i < steps; ++i) {
                    switch (*rc::gen::inRange(0, 8)) {
                        case 0:
                            rmp::ads::request_interstitial();
                            break;
                        case 1:
                            rmp::ads::show_interstitial();
                            break;
                        case 2:
                            rmp::ads::request_rewarded();
                            break;
                        case 3:
                            rmp::ads::show_rewarded();
                            break;
                        case 4:
                            RC_ASSERT(!rmp::ads::take_reward_earned());
                            break;
                        case 5:
                            RC_ASSERT(rmp::ads::reward_amount() == 0);
                            break;
                        case 6:
                            RC_ASSERT(!rmp::ads::is_rewarded_loaded());
                            break;
                        default:
                            RC_ASSERT(!rmp::ads::is_interstitial_loaded());
                            break;
                    }
                }
                RC_ASSERT(!rmp::ads::is_interstitial_loaded());
                RC_ASSERT(!rmp::ads::is_rewarded_loaded());
                RC_ASSERT(!rmp::ads::take_reward_earned());
                RC_ASSERT(rmp::ads::reward_amount() == 0);
            });
    }
}
