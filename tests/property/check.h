#pragma once
// ---------------------------------------------------------------------------
// A RapidCheck property as one doctest check:
//
//     rmp::test::check("a copy keeps its depth", [](const rmp::Value &v) {
//         RC_ASSERT(rmp::test::gen::depth(rmp::Value(v)) == rmp::test::gen::depth(v));
//     });
//
// Silent when it holds, and counted in doctest's summary. When it does not,
// the shrunk counterexample is the failure's message, and RapidCheck prints
// the RC_PARAMS="reproduce=..." that plays exactly that case again when the
// program ends. The description is the key that reproduce string finds the
// property by, so it is a string literal and two properties never share one
// (tests/campaign_test.py's PropertyFilesTest reads every *_prop.cpp).
//
// RapidCheck ships a doctest integration (extras/doctest), and it does not
// compile: inside its own `namespace rc::doctest`, doctest's macros' plain
// `doctest::` names that namespace. These lines do what it meant to.
// ---------------------------------------------------------------------------

#include <doctest.h>
#include <rapidcheck.h>

#include <sstream>
#include <string>
#include <utility>

namespace rmp::test {

template <class Testable>
void check(const std::string &description, Testable &&property) {
    rc::detail::TestMetadata metadata;
    metadata.id = description;
    metadata.description = description;
    const rc::detail::TestResult result =
        rc::detail::checkTestable(std::forward<Testable>(property), metadata);
    const bool held = result.template is<rc::detail::SuccessResult>();
    std::string why;
    if (!held) {
        std::ostringstream message;
        rc::detail::printResultMessage(result, message);
        why = message.str();
    }
    CHECK_MESSAGE(held, description << "\n" << why);
}

} // namespace rmp::test
