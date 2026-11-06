// ===========================================================================
// A Program as bytes and back -- what rmp_yarnc writes into a game at build
// time ([dialogue] build = "embedded") and the game reads instead of a .yarn.
// The official compiler writes a protobuf Yarn.Program; this is the same
// content in a format of our own, little-endian, every count checked on the
// way back so a damaged blob is refused rather than read past.
// ===========================================================================

#include "yarn.h"

#include <cstring>
#include <limits>

namespace rmp::yarn {

namespace {

constexpr std::uint32_t MAGIC = 0x4E524159; // "YARN"
constexpr std::uint32_t VERSION = 1;

class Writer {
public:
    std::vector<std::uint8_t> bytes;

    void u32(std::uint32_t v) {
        for (int k = 0; k < 4; ++k)
            bytes.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
    }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void f32(float v) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        u32(bits);
    }
    void str(std::string_view s) {
        u32(static_cast<std::uint32_t>(s.size()));
        bytes.insert(bytes.end(), s.begin(), s.end());
    }
    void value(const Value &v) {
        u32(static_cast<std::uint32_t>(v.kind));
        f32(v.number);
        u32(v.boolean ? 1 : 0);
        str(v.text);
    }
    void code(const std::vector<Instruction> &c) {
        u32(static_cast<std::uint32_t>(c.size()));
        for (const Instruction &i : c) {
            u32(static_cast<std::uint32_t>(i.op));
            i32(i.a);
            i32(i.b);
            i32(i.c);
            f32(i.f);
            str(i.s);
        }
    }
    void strings(const std::vector<std::string> &list) {
        u32(static_cast<std::uint32_t>(list.size()));
        for (const std::string &s : list) str(s);
    }
    void pairs(const std::vector<std::pair<std::string, std::string>> &list) {
        u32(static_cast<std::uint32_t>(list.size()));
        for (const auto &[k, v] : list) {
            str(k);
            str(v);
        }
    }
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> b) : _b(b) {}
    bool ok = true;

    std::uint32_t u32() {
        if (_at + 4 > _b.size()) {
            ok = false;
            return 0;
        }
        std::uint32_t v = 0;
        for (int k = 0; k < 4; ++k) v |= static_cast<std::uint32_t>(_b[_at++]) << (8 * k);
        return v;
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    float f32() {
        const std::uint32_t bits = u32();
        float v = 0;
        std::memcpy(&v, &bits, sizeof v);
        return v;
    }
    std::string str() {
        const std::uint32_t n = u32();
        if (!ok || n > _b.size() - _at) {
            ok = false;
            return {};
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): bytes as chars
        std::string s(reinterpret_cast<const char *>(_b.data() + _at), n);
        _at += n;
        return s;
    }
    // A count, refused when the bytes left could not hold that many items.
    std::size_t count(std::size_t smallest) {
        const std::uint32_t n = u32();
        if (!ok || static_cast<std::size_t>(n) * smallest > _b.size() - _at) {
            ok = false;
            return 0;
        }
        return n;
    }
    Value value() {
        Value v;
        const std::uint32_t k = u32();
        if (k > static_cast<std::uint32_t>(Kind::BOOL)) {
            ok = false;
        } else {
            v.kind = static_cast<Kind>(k);
        }
        v.number = f32();
        v.boolean = u32() != 0;
        v.text = str();
        return v;
    }
    std::vector<Instruction> code() {
        std::vector<Instruction> c(count(24));
        for (Instruction &i : c) {
            const std::uint32_t op = u32();
            if (op > static_cast<std::uint32_t>(Op::REPLACE_NODE)) {
                ok = false;
            } else {
                i.op = static_cast<Op>(op);
            }
            i.a = i32();
            i.b = i32();
            i.c = i32();
            i.f = f32();
            i.s = str();
        }
        return c;
    }
    std::vector<std::string> strings() {
        std::vector<std::string> list(count(4));
        for (std::string &s : list) s = str();
        return list;
    }
    std::vector<std::pair<std::string, std::string>> pairs() {
        std::vector<std::pair<std::string, std::string>> list(count(8));
        for (auto &[k, v] : list) {
            k = str();
            v = str();
        }
        return list;
    }
    [[nodiscard]] bool at_end() const { return _at == _b.size(); }

private:
    std::span<const std::uint8_t> _b;
    std::size_t _at = 0;
};

} // namespace

std::vector<std::uint8_t> to_bytes(const Program &p) {
    Writer w;
    w.u32(MAGIC);
    w.u32(VERSION);
    w.u32(static_cast<std::uint32_t>(p.nodes.size()));
    for (const ProgramNode &n : p.nodes) {
        w.str(n.name);
        w.str(n.group);
        w.code(n.code);
        w.pairs(n.headers);
        w.strings(n.tags);
        w.u32(n.tracked ? 1 : 0);
        w.u32(n.hub ? 1 : 0);
        w.code(n.condition);
        w.i32(n.complexity);
        w.i32(n.source_line);
    }
    w.u32(static_cast<std::uint32_t>(p.lines.size()));
    for (const LineInfo &l : p.lines) {
        w.str(l.id);
        w.str(l.text);
        w.str(l.node);
        w.strings(l.tags);
        w.str(l.shadow);
        w.i32(l.source_line);
    }
    w.u32(static_cast<std::uint32_t>(p.initial.size()));
    for (const auto &[name, v] : p.initial) {
        w.str(name);
        w.value(v);
    }
    w.u32(static_cast<std::uint32_t>(p.smart.size()));
    for (const SmartVariable &s : p.smart) {
        w.str(s.name);
        w.code(s.code);
    }
    return std::move(w.bytes);
}

std::optional<Program> from_bytes(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);
    if (r.u32() != MAGIC || r.u32() != VERSION || !r.ok) return std::nullopt;
    Program p;
    p.nodes.resize(r.count(40));
    for (ProgramNode &n : p.nodes) {
        n.name = r.str();
        n.group = r.str();
        n.code = r.code();
        n.headers = r.pairs();
        n.tags = r.strings();
        n.tracked = r.u32() != 0;
        n.hub = r.u32() != 0;
        n.condition = r.code();
        n.complexity = r.i32();
        n.source_line = r.i32();
        if (!r.ok) return std::nullopt;
    }
    p.lines.resize(r.count(24));
    for (LineInfo &l : p.lines) {
        l.id = r.str();
        l.text = r.str();
        l.node = r.str();
        l.tags = r.strings();
        l.shadow = r.str();
        l.source_line = r.i32();
        if (!r.ok) return std::nullopt;
    }
    const std::size_t initial = r.count(20);
    for (std::size_t k = 0; k < initial && r.ok; ++k) {
        std::string name = r.str();
        p.initial[std::move(name)] = r.value();
    }
    p.smart.resize(r.count(8));
    for (SmartVariable &s : p.smart) {
        s.name = r.str();
        s.code = r.code();
    }
    if (!r.ok || !r.at_end()) return std::nullopt;
    // Every jump inside its node, every destination a place.
    for (const ProgramNode &n : p.nodes) {
        for (const Instruction &i : n.code) {
            const bool jumps = i.op == Op::JUMP_TO || i.op == Op::JUMP_IF_FALSE ||
                i.op == Op::ADD_OPTION || i.op == Op::SHOW_OPTIONS;
            if (jumps && (i.a < 0 || static_cast<std::size_t>(i.a) > n.code.size()))
                return std::nullopt;
            if (i.op == Op::ADD_SALIENCY_CANDIDATE &&
                (i.b < 0 || static_cast<std::size_t>(i.b) > n.code.size())) {
                return std::nullopt;
            }
        }
    }
    return p;
}

} // namespace rmp::yarn
