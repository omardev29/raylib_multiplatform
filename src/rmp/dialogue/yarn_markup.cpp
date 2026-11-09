// ===========================================================================
// A line's markup -> plain text and attributes. Ported from
// YarnSpinner.Markup/LineParser.cs: [name prop=value]..[/name], [name/],
// [/] closing every open one, [nomarkup], \[ \] and \: escapes, the
// character ("Name: ") as an attribute, and the three replacement markers --
// select, plural and ordinal -- whose % is the value.
// ===========================================================================

#include "../text/text_internal.h"
#include "yarn.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>

namespace rmp::yarn {

namespace {

int code_points(std::string_view s) {
    int n = 0;
    for (const char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    }
    return n;
}

bool space(char c) { return c == ' ' || c == '\t'; }

struct Tag {
    std::string name;
    std::vector<std::pair<std::string, std::string>> properties;
    bool closing = false;
    bool self_closing = false;
    bool close_all = false;
};

// The tag that starts at `text[i]` ('['), and where it ends; nothing when it
// is not one.
std::optional<Tag> read_tag(std::string_view text, std::size_t &i) {
    std::size_t j = i + 1;
    Tag tag;
    const auto skip = [&] {
        while (j < text.size() && space(text[j])) ++j;
    };
    skip();
    if (j < text.size() && text[j] == '/') {
        tag.closing = true;
        ++j;
        skip();
        if (j < text.size() && text[j] == ']') {
            tag.close_all = true;
            i = j + 1;
            return tag;
        }
    }
    const auto name_char = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' ||
            static_cast<unsigned char>(c) >= 0x80;
    };
    const std::size_t name_start = j;
    while (j < text.size() && name_char(text[j])) ++j;
    tag.name = std::string(text.substr(name_start, j - name_start));
    if (tag.name.empty()) return std::nullopt;
    // [name=value]: the tag's own value, as a property of its name.
    const auto value = [&]() -> std::optional<std::string> {
        skip();
        std::string v;
        if (j < text.size() && text[j] == '"') {
            ++j;
            while (j < text.size() && text[j] != '"') {
                if (text[j] == '\\' && j + 1 < text.size()) ++j;
                v += text[j++];
            }
            if (j >= text.size()) return std::nullopt;
            ++j;
            return v;
        }
        while (j < text.size() && !space(text[j]) && text[j] != ']' &&
               (text[j] != '/' || j + 1 >= text.size() || text[j + 1] != ']')) {
            v += text[j++];
        }
        return v;
    };
    skip();
    if (j < text.size() && text[j] == '=') {
        ++j;
        auto v = value();
        if (!v) return std::nullopt;
        tag.properties.emplace_back(tag.name, std::move(*v));
    }
    while (true) {
        skip();
        if (j >= text.size()) return std::nullopt;
        if (text[j] == ']') {
            i = j + 1;
            return tag;
        }
        if (text[j] == '/' && j + 1 < text.size() && text[j + 1] == ']') {
            tag.self_closing = true;
            i = j + 2;
            return tag;
        }
        const std::size_t key_start = j;
        while (j < text.size() && name_char(text[j])) ++j;
        const std::string key(text.substr(key_start, j - key_start));
        if (key.empty()) return std::nullopt;
        skip();
        if (j < text.size() && text[j] == '=') {
            ++j;
            auto v = value();
            if (!v) return std::nullopt;
            tag.properties.emplace_back(key, std::move(*v));
        } else {
            tag.properties.emplace_back(key, "true");
        }
    }
}

std::string property(const Tag &tag, std::string_view key) {
    for (const auto &[k, v] : tag.properties) {
        if (k == key) return v;
    }
    return {};
}

bool has_property(const Tag &tag, std::string_view key) {
    return std::ranges::any_of(tag.properties,
                               [&](const auto &p) { return p.first == key; });
}

// What a replacement marker says: the property its value chooses, with % as
// the value.
std::optional<std::string> replace(const Tag &tag, std::string_view language) {
    const std::string value = property(tag, "value");
    std::string chosen;
    if (tag.name == "select") {
        chosen = has_property(tag, value) ? property(tag, value) : property(tag, "other");
    } else if (tag.name == "plural" || tag.name == "ordinal") {
        const double n = std::strtod(value.c_str(), nullptr);
        const bool integer = value.find('.') == std::string::npos;
        const std::string_view category = tag.name == "plural"
            ? rmp::lang::detail::plural_category(language, n, integer)
            : rmp::lang::detail::ordinal_category(language, n);
        chosen = has_property(tag, category) ? property(tag, category)
                                             : property(tag, "other");
    } else {
        return std::nullopt;
    }
    std::string out;
    for (const char c : chosen) {
        if (c == '%') {
            out += value;
        } else {
            out += c;
        }
    }
    return out;
}

} // namespace

Marked parse_markup(std::string_view text, std::string_view language) {
    Marked out;
    struct Open {
        std::string name;
        int position = 0;
        std::vector<std::pair<std::string, std::string>> properties;
    };
    std::vector<Open> open;
    std::size_t i = 0;
    bool nomarkup = false;
    // The character: "Name: " at the start, its colon not escaped.
    {
        std::size_t colon = std::string_view::npos;
        for (std::size_t k = 0; k < text.size(); ++k) {
            if (text[k] == '\\') {
                ++k;
                continue;
            }
            if (text[k] == '[') break;
            if (text[k] == ':') {
                colon = k;
                break;
            }
        }
        if (colon != std::string_view::npos && colon > 0) {
            std::size_t end = colon + 1;
            while (end < text.size() && space(text[end])) ++end;
            Attribute a;
            a.name = "character";
            a.position = 0;
            a.length = code_points(text.substr(0, end));
            a.properties.emplace_back("name", std::string(text.substr(0, colon)));
            out.attributes.push_back(std::move(a));
        }
    }
    while (i < text.size()) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size() &&
            (text[i + 1] == '[' || text[i + 1] == ']' || text[i + 1] == ':' ||
             text[i + 1] == '\\')) {
            out.text += text[i + 1];
            i += 2;
            continue;
        }
        if (c == '[') {
            std::size_t at = i;
            std::optional<Tag> tag = read_tag(text, at);
            if (nomarkup && !(tag && tag->closing && tag->name == "nomarkup"))
                tag.reset();
            if (tag) {
                i = at;
                const int position = code_points(out.text);
                if (tag->close_all) {
                    for (Open &o : open) {
                        out.attributes.push_back({ o.name, o.position,
                                                   position - o.position,
                                                   std::move(o.properties) });
                    }
                    open.clear();
                } else if (tag->closing) {
                    if (tag->name == "nomarkup") nomarkup = false;
                    for (std::size_t k = open.size(); k > 0; --k) {
                        if (open[k - 1].name == tag->name) {
                            Open o = std::move(open[k - 1]);
                            open.erase(open.begin() + static_cast<std::ptrdiff_t>(k - 1));
                            out.attributes.push_back({ o.name, o.position,
                                                       position - o.position,
                                                       std::move(o.properties) });
                            break;
                        }
                    }
                } else if (tag->self_closing) {
                    if (auto replaced = replace(*tag, language)) {
                        out.text += *replaced;
                    } else {
                        out.attributes.push_back(
                            { tag->name, position, 0, tag->properties });
                        // A marker on its own takes the space after it with it.
                        const bool trim = property(*tag, "trimwhitespace") != "false";
                        if (trim && i < text.size() && space(text[i]) && position > 0)
                            ++i;
                    }
                } else {
                    if (tag->name == "nomarkup") nomarkup = true;
                    open.push_back({ tag->name, position, tag->properties });
                }
                continue;
            }
        }
        out.text += c;
        ++i;
    }
    const int end = code_points(out.text);
    for (Open &o : open) {
        out.attributes.push_back(
            { o.name, o.position, end - o.position, std::move(o.properties) });
    }
    return out;
}

} // namespace rmp::yarn
