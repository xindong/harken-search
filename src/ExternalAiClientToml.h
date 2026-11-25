#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace HarkenSearch::ClientToml {

struct OwnedTable {
    std::size_t begin = 0;
    std::size_t end = 0;
    bool present = false;
    bool enabled = true;
    std::string command;
    std::vector<std::string> args;
};

class Reader {
public:
    explicit Reader(std::string_view text) : text_(text) {}

    std::optional<OwnedTable> Read() {
        if (!ValidUtf8()) return std::nullopt;
        Key table;
        while (!AtEnd()) {
            const auto begin = offset_;
            Horizontal();
            if (AtEnd() || Peek() == '#' || Peek() == '\r' || Peek() == '\n') {
                if (!FinishLine()) return std::nullopt;
                continue;
            }
            if (Take('[')) {
                if (!ReadKey(table) || !Take(']') || !FinishLine() ||
                    HasAncestor(values_, table) || dottedTables_.count(table) ||
                    !tables_.insert(table).second ||
                    (OwnsPrefix(table) && table.size() != 2)) return std::nullopt;
                if (OwnsPrefix(table)) {
                    owned_.present = true;
                    owned_.begin = begin;
                    owned_.end = offset_;
                }
                continue;
            }
            Key key;
            if (!ReadKey(key) || !Take('=')) return std::nullopt;
            Horizontal();
            if (OwnsPrefix(table)) {
                if (key.size() != 1 || !ReadOwnedValue(key.front())) return std::nullopt;
            } else if (!Value(0)) {
                return std::nullopt;
            }
            if (!FinishLine()) return std::nullopt;
            Key full = table;
            for (const auto& part : key) {
                full.push_back(part);
                if (full.size() < table.size() + key.size()) {
                    if (tables_.count(full)) return std::nullopt;
                    dottedTables_.insert(full);
                }
            }
            if ((OwnsPrefix(full) && !OwnsPrefix(table)) ||
                (full.size() == 1 && full.front() == "mcp_servers") ||
                !AddValueKey(values_, full) || HasDescendant(tables_, full)) return std::nullopt;
            if (OwnsPrefix(table)) owned_.end = offset_;
        }
        return owned_;
    }

private:
    using Key = std::vector<std::string>;
    using Keys = std::set<Key>;

    static bool StartsWith(const Key& value, const Key& prefix) {
        return value.size() >= prefix.size() &&
            std::equal(prefix.begin(), prefix.end(), value.begin());
    }

    static bool OwnsPrefix(const Key& key) {
        return key.size() >= 2 && key[0] == "mcp_servers" && key[1] == "harken";
    }

    static bool HasAncestor(const Keys& keys, Key key) {
        while (!key.empty()) {
            if (keys.count(key)) return true;
            key.pop_back();
        }
        return false;
    }

    static bool HasDescendant(const Keys& keys, const Key& key) {
        const auto position = keys.lower_bound(key);
        return position != keys.end() && StartsWith(*position, key);
    }

    static bool AddValueKey(Keys& keys, const Key& key) {
        return !HasAncestor(keys, key) && !HasDescendant(keys, key) && keys.insert(key).second;
    }

    bool AtEnd() const { return offset_ == text_.size(); }
    char Peek() const { return AtEnd() ? '\0' : text_[offset_]; }

    bool Take(char unit) {
        if (AtEnd() || Peek() != unit) return false;
        ++offset_;
        return true;
    }

    bool Take(std::string_view token) {
        if (text_.substr(offset_, token.size()) != token) return false;
        offset_ += token.size();
        return true;
    }

    void Horizontal() {
        while (Peek() == ' ' || Peek() == '\t') ++offset_;
    }

    bool Newline() { return Take('\n') || Take("\r\n"); }

    static bool Control(char unit) {
        const auto byte = static_cast<unsigned char>(unit);
        return (byte < 0x20 && byte != '\t') || byte == 0x7f;
    }

    bool Comment() {
        if (!Take('#')) return true;
        while (!AtEnd() && Peek() != '\r' && Peek() != '\n') {
            if (Control(Peek())) return false;
            ++offset_;
        }
        return true;
    }

    bool FinishLine() {
        Horizontal();
        return Comment() && (AtEnd() || Newline());
    }

    bool ArraySpace() {
        do {
            Horizontal();
            if (!Comment()) return false;
        } while (Newline());
        return true;
    }

    static bool BareKey(char unit) {
        return (unit >= 'a' && unit <= 'z') || (unit >= 'A' && unit <= 'Z') ||
            (unit >= '0' && unit <= '9') || unit == '_' || unit == '-';
    }

    bool ReadKey(Key& key) {
        key.clear();
        do {
            Horizontal();
            std::string part;
            if (Peek() == '\'' || Peek() == '"') {
                if (!String(part, false)) return false;
            } else {
                const auto begin = offset_;
                while (BareKey(Peek())) ++offset_;
                if (begin == offset_) return false;
                part = text_.substr(begin, offset_ - begin);
            }
            key.push_back(std::move(part));
            Horizontal();
        } while (Take('.'));
        return true;
    }

    static bool Scalar(std::uint32_t value) {
        return value <= 0x10ffff && !(value >= 0xd800 && value <= 0xdfff);
    }

    bool ValidUtf8() const {
        std::uint32_t scalar = 0;
        std::uint32_t minimum = 0;
        unsigned int remaining = 0;
        for (const unsigned char unit : text_) {
            if (remaining != 0) {
                if ((unit & 0xc0) != 0x80) return false;
                scalar = (scalar << 6) | (unit & 0x3f);
                if (--remaining == 0 && (scalar < minimum || !Scalar(scalar))) return false;
            } else if ((unit & 0xe0) == 0xc0) {
                scalar = unit & 0x1f; minimum = 0x80; remaining = 1;
            } else if ((unit & 0xf0) == 0xe0) {
                scalar = unit & 0x0f; minimum = 0x800; remaining = 2;
            } else if ((unit & 0xf8) == 0xf0) {
                scalar = unit & 0x07; minimum = 0x10000; remaining = 3;
            } else if (unit >= 0x80) {
                return false;
            }
        }
        return remaining == 0;
    }

    bool UnicodeEscape(std::string& out, int digits) {
        std::uint32_t scalar = 0;
        for (int index = 0; index < digits; ++index) {
            const char unit = Peek();
            unsigned int nibble = 0;
            if (unit >= '0' && unit <= '9') nibble = unit - '0';
            else if (unit >= 'a' && unit <= 'f') nibble = unit - 'a' + 10;
            else if (unit >= 'A' && unit <= 'F') nibble = unit - 'A' + 10;
            else return false;
            ++offset_;
            scalar = (scalar << 4) | nibble;
        }
        if (!Scalar(scalar)) return false;
        if (scalar < 0x80) {
            out.push_back(static_cast<char>(scalar));
            return true;
        }
        const unsigned int trailing = scalar < 0x800 ? 1 : (scalar < 0x10000 ? 2 : 3);
        const unsigned int prefix = trailing == 1 ? 0xc0 : (trailing == 2 ? 0xe0 : 0xf0);
        out.push_back(static_cast<char>(prefix | (scalar >> (6 * trailing))));
        for (unsigned int index = trailing; index != 0; --index) {
            out.push_back(static_cast<char>(0x80 | ((scalar >> (6 * (index - 1))) & 0x3f)));
        }
        return true;
    }

    bool Escape(std::string& out, bool multiline) {
        if (multiline) {
            const auto begin = offset_;
            Horizontal();
            if (Newline()) {
                do { Horizontal(); } while (Newline());
                return true;
            }
            offset_ = begin;
        }
        if (AtEnd()) return false;
        const char unit = text_[offset_++];
        switch (unit) {
        case '"': case '\\': out.push_back(unit); return true;
        case 'b': out.push_back('\b'); return true;
        case 't': out.push_back('\t'); return true;
        case 'n': out.push_back('\n'); return true;
        case 'f': out.push_back('\f'); return true;
        case 'r': out.push_back('\r'); return true;
        case 'u': return UnicodeEscape(out, 4);
        case 'U': return UnicodeEscape(out, 8);
        default: return false;
        }
    }

    bool String(std::string& out, bool allowMultiline = true) {
        const char quote = Peek();
        if (quote != '"' && quote != '\'') return false;
        const bool multiline = allowMultiline && Take(std::string(3, quote));
        if (multiline) Newline();
        else ++offset_;
        while (!AtEnd()) {
            if (Take(quote)) {
                if (!multiline) return true;
                std::size_t count = 1;
                while (count < 6 && Take(quote)) ++count;
                if (count > 5) return false;
                out.append(count >= 3 ? count - 3 : count, quote);
                if (count >= 3) return true;
            } else if (quote == '"' && Take('\\')) {
                if (!Escape(out, multiline)) return false;
            } else if (multiline && Newline()) {
                out.push_back('\n');
            } else {
                if (Control(Peek())) return false;
                out.push_back(text_[offset_++]);
            }
        }
        return false;
    }

    template<typename ReadItem>
    bool Array(ReadItem readItem) {
        if (!Take('[') || !ArraySpace()) return false;
        while (!Take(']')) {
            if (!readItem() || !ArraySpace()) return false;
            if (Take(']')) return true;
            if (!Take(',') || !ArraySpace()) return false;
        }
        return true;
    }

    bool InlineTable(unsigned int depth) {
        if (!Take('{')) return false;
        Horizontal();
        if (Take('}')) return true;
        Keys keys;
        do {
            Key key;
            if (!ReadKey(key) || !AddValueKey(keys, key) || !Take('=')) return false;
            Horizontal();
            if (!Value(depth)) return false;
            Horizontal();
            if (Take('}')) return true;
        } while (Take(','));
        return false;
    }

    bool Atom() {
        static const std::regex scalar(
            R"((true|false|[+-]?(inf|nan)|[+-]?(0|[1-9](_?[0-9])*)(\.[0-9](_?[0-9])*)?([eE][+-]?[0-9](_?[0-9])*)?|0(x[0-9a-fA-F](_?[0-9a-fA-F])*|o[0-7](_?[0-7])*|b[01](_?[01])*)))");
        static const std::string date = R"([0-9]{4}-(0[1-9]|1[0-2])-(0[1-9]|[12][0-9]|3[01]))";
        static const std::string time = R"(([01][0-9]|2[0-3]):[0-5][0-9]:([0-5][0-9]|60)(\.[0-9]+)?)";
        static const std::regex timestamp(date + "([Tt ]" + time +
            R"(([Zz]|[+-]([01][0-9]|2[0-3]):[0-5][0-9])?)?)" + "|" + time);
        const auto begin = offset_;
        const auto token = [this] {
            while (!AtEnd() && std::string_view(" \t\r\n#,]}").find(Peek()) == std::string_view::npos) ++offset_;
        };
        token();
        if (offset_ - begin == 10 && Peek() == ' ' && text_.substr(offset_ + 1, 3).size() == 3 &&
            text_[offset_ + 3] == ':') {
            ++offset_;
            token();
        }
        const auto value = text_.substr(begin, offset_ - begin);
        return value.size() <= 128 && (std::regex_match(value.begin(), value.end(), scalar) ||
            std::regex_match(value.begin(), value.end(), timestamp));
    }

    bool Value(unsigned int depth) {
        if (depth >= 32) return false;
        switch (Peek()) {
        case '"': case '\'': {
            std::string ignored;
            return String(ignored);
        }
        case '[': return Array([&] { return Value(depth + 1); });
        case '{': return InlineTable(depth + 1);
        default: return Atom();
        }
    }

    bool ReadOwnedValue(const std::string& key) {
        if (key == "command") return String(owned_.command);
        if (key == "args") {
            return Array([this] {
                std::string value;
                if (!String(value)) return false;
                owned_.args.push_back(std::move(value));
                return true;
            });
        }
        if (key == "enabled") {
            if (Take("true")) return true;
            owned_.enabled = false;
            return Take("false");
        }
        return Value(0);
    }

    std::string_view text_;
    std::size_t offset_ = 0;
    Keys tables_;
    Keys dottedTables_;
    Keys values_;
    OwnedTable owned_;
};

inline std::optional<OwnedTable> Parse(std::string_view text) {
    return Reader(text).Read();
}

}
