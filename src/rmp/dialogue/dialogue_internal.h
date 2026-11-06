#pragma once

// The seams of rmp::Dialogue: the table of scripts rmp_yarnc built into the
// game ([dialogue] build = "embedded"), and a script compiled from a string,
// which the tests use and the public API does not offer (dialogue is written
// in .yarn files).

#include <rmp/dialogue.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace rmp::dialogue::detail {

struct Embedded {
    std::string_view name; // as load_dialogue() is asked for it: "wizard.yarn"
    std::span<const std::uint8_t> bytes; // yarn::to_bytes() of its program
};

// What the generated dialogue_bytecode.cpp hands the game at start.
void set_embedded(std::span<const Embedded> table);

Dialogue compile_text(std::string_view source, std::string_view file = "test.yarn");
Dialogue from_program(std::span<const std::uint8_t> bytes);

} // namespace rmp::dialogue::detail
