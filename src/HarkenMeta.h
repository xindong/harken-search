#pragma once
// Harken-owned metadata stored in audio_files.harken_meta as a flat JSON
// object. Keys: category, function, source. Unknown keys are preserved by
// Merge so later additions do not require a schema migration.

#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace HarkenMeta {

struct Values {
    std::string category;
    std::string function;
    std::string source;

    bool empty() const { return category.empty() && function.empty(); }
    bool operator==(const Values& other) const {
        return category == other.category && function == other.function && source == other.source;
    }
    bool operator!=(const Values& other) const { return !(*this == other); }
};

// Update is explicit so callers can clear a value by requesting an empty one.
struct Update {
    bool setCategory = false;
    bool setFunction = false;
    bool setSource = false;
    std::string category;
    std::string function;
    std::string source;
};

struct Pair {
    std::string key;
    std::string value;
};

inline bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

inline std::string JsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += "\\u00";
                    static const char* kHex = "0123456789abcdef";
                    out += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                    out += kHex[static_cast<unsigned char>(c) & 0xF];
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

inline bool JsonUnescape(const std::string& in, std::string& out) {
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '\\') {
            out += in[i];
            continue;
        }
        if (i + 1 >= in.size()) return false;
        const char e = in[++i];
        switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (i + 4 >= in.size()) return false;
                auto hex = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                int code = 0;
                for (int k = 0; k < 4; ++k) {
                    const int v = hex(in[i + 1 + k]);
                    if (v < 0) return false;
                    code = code * 16 + v;
                }
                i += 4;
                // Encode as UTF-8. Surrogate pairs are left as-is; Harken
                // values are written by our own serializer, not raw \u input.
                if (code < 0x80) {
                    out += static_cast<char>(code);
                } else if (code < 0x800) {
                    out += static_cast<char>(0xC0 | (code >> 6));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (code >> 12));
                    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                }
                break;
            }
            default: return false;
        }
    }
    return true;
}

inline std::vector<Pair> ParsePairs(const std::string& json) {
    std::vector<Pair> out;
    size_t i = 0;
    const size_t n = json.size();
    auto skip = [&]() { while (i < n && IsSpace(json[i])) ++i; };
    skip();
    if (i >= n || json[i] != '{') return out;
    ++i;
    for (;;) {
        skip();
        if (i >= n) return {};
        if (json[i] == '}') return out;
        if (json[i] != '"') return {};
        ++i;
        std::string keyRaw;
        while (i < n && json[i] != '"') {
            if (json[i] == '\\') {
                if (i + 1 >= n) return {};
                keyRaw += json[i++];
                keyRaw += json[i++];
            } else {
                keyRaw += json[i++];
            }
        }
        if (i >= n) return {};
        ++i;
        skip();
        if (i >= n || json[i] != ':') return {};
        ++i;
        skip();
        if (i >= n || json[i] != '"') return {};
        ++i;
        std::string valueRaw;
        while (i < n && json[i] != '"') {
            if (json[i] == '\\') {
                if (i + 1 >= n) return {};
                valueRaw += json[i++];
                valueRaw += json[i++];
            } else {
                valueRaw += json[i++];
            }
        }
        if (i >= n) return {};
        ++i;
        Pair pair;
        if (!JsonUnescape(keyRaw, pair.key) || !JsonUnescape(valueRaw, pair.value)) return {};
        if (!pair.key.empty()) out.push_back(std::move(pair));
        skip();
        if (i < n && json[i] == ',') { ++i; continue; }
        if (i < n && json[i] == '}') return out;
        return {};
    }
}

inline Values Parse(const std::string& json) {
    Values values;
    if (json.empty()) return values;
    for (const auto& pair : ParsePairs(json)) {
        if (pair.key == "category") values.category = pair.value;
        else if (pair.key == "function") values.function = pair.value;
        else if (pair.key == "source") values.source = pair.value;
    }
    return values;
}

inline std::string SerializePairs(const std::vector<Pair>& pairs) {
    std::string out = "{";
    bool first = true;
    for (const auto& pair : pairs) {
        if (pair.key.empty()) continue;
        if (!first) out += ',';
        first = false;
        out += '"';
        out += JsonEscape(pair.key);
        out += "\":\"";
        out += JsonEscape(pair.value);
        out += '"';
    }
    out += '}';
    return out;
}

inline std::string Merge(const std::string& existingJson, const Update& update) {
    Values current = Parse(existingJson);
    if (update.setCategory) current.category = update.category;
    if (update.setFunction) current.function = update.function;
    if (update.setSource) current.source = update.source;

    if (current.empty()) return std::string();

    std::vector<Pair> others;
    for (auto& pair : ParsePairs(existingJson)) {
        if (pair.key == "category" || pair.key == "function" || pair.key == "source") continue;
        others.push_back(std::move(pair));
    }

    std::vector<Pair> out;
    if (!current.category.empty()) out.push_back({"category", current.category});
    if (!current.function.empty()) out.push_back({"function", current.function});
    if (!current.source.empty()) out.push_back({"source", current.source});
    for (auto& pair : others) out.push_back(std::move(pair));
    return SerializePairs(out);
}

inline std::string SearchText(const std::string& json) {
    const Values values = Parse(json);
    std::string text = values.category;
    if (!values.function.empty()) {
        if (!text.empty()) text += ' ';
        text += values.function;
    }
    return text;
}

inline bool IsManual(const std::string& json) {
    return Parse(json).source == "manual";
}

inline std::string XmlEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c; break;
        }
    }
    return out;
}

inline std::string Section(const Values& values) {
    if (values.empty()) return std::string();
    std::string xml = "    <HARKEN>\n";
    if (!values.category.empty()) {
        xml += "      <CATEGORY>" + XmlEscape(values.category) + "</CATEGORY>\n";
    }
    if (!values.function.empty()) {
        xml += "      <FUNCTION>" + XmlEscape(values.function) + "</FUNCTION>\n";
    }
    xml += "    </HARKEN>\n";
    return xml;
}

inline size_t FindCaseInsensitive(const std::string& haystack, const std::string& needle, size_t from = 0) {
    if (needle.empty() || needle.size() > haystack.size()) return std::string::npos;
    for (size_t i = from; i + needle.size() <= haystack.size(); ++i) {
        size_t k = 0;
        for (; k < needle.size(); ++k) {
            if (std::tolower(static_cast<unsigned char>(haystack[i + k])) !=
                std::tolower(static_cast<unsigned char>(needle[k]))) break;
        }
        if (k == needle.size()) return i;
    }
    return std::string::npos;
}

// True when pos sits inside an XML comment or a CDATA section.
inline bool InsideMarkupLiteral(const std::string& xml, size_t pos) {
    const size_t commentOpen = xml.rfind("<!--", pos);
    if (commentOpen != std::string::npos) {
        const size_t commentClose = xml.rfind("-->", pos);
        if (commentClose == std::string::npos || commentOpen > commentClose) return true;
    }
    const size_t cdataOpen = xml.rfind("<![CDATA[", pos);
    if (cdataOpen != std::string::npos) {
        const size_t cdataClose = xml.rfind("]]>", pos);
        if (cdataClose == std::string::npos || cdataOpen > cdataClose) return true;
    }
    return false;
}

// Finds "<name" as a whole element name: "<USER_DATA/>" and "<USERNOTES>" are
// different elements, and a section that only appears inside a comment or a
// CDATA block must never be rewritten.
inline size_t FindElementStart(const std::string& xml, const std::string& name, size_t from = 0) {
    const std::string needle = "<" + name;
    size_t cursor = from;
    while (cursor < xml.size()) {
        const size_t found = FindCaseInsensitive(xml, needle, cursor);
        if (found == std::string::npos) return std::string::npos;
        const size_t next = found + needle.size();
        const char boundary = next < xml.size() ? xml[next] : '\0';
        const bool nameEnds = next >= xml.size() || boundary == ' ' || boundary == '\t' ||
                              boundary == '\r' || boundary == '\n' || boundary == '>' ||
                              boundary == '/';
        if (nameEnds && !InsideMarkupLiteral(xml, found)) return found;
        cursor = found + 1;
    }
    return std::string::npos;
}

// True when the start tag that ends at tagEnd closes itself ("<USER />").
inline bool StartTagIsSelfClosing(const std::string& xml, size_t start, size_t tagEnd) {
    if (tagEnd == std::string::npos || tagEnd <= start) return false;
    size_t index = tagEnd;
    while (index > start && (xml[index - 1] == ' ' || xml[index - 1] == '\t')) --index;
    return index > start && xml[index - 1] == '/';
}

inline std::string RemoveSection(std::string xml) {
    for (;;) {
        const size_t start = FindElementStart(xml, "HARKEN");
        if (start == std::string::npos) break;
        const size_t tagEnd = xml.find('>', start);
        if (tagEnd == std::string::npos) break;
        size_t end = std::string::npos;
        if (StartTagIsSelfClosing(xml, start, tagEnd)) {
            // A vendor may carry an empty "<HARKEN/>"; it still counts as one
            // section and must be replaced rather than duplicated.
            end = tagEnd + 1;
        } else {
            const size_t close = FindCaseInsensitive(xml, "</HARKEN>", tagEnd);
            if (close == std::string::npos) break;
            end = close + std::string("</HARKEN>").size();
        }
        size_t eraseStart = start;
        while (eraseStart > 0 && (xml[eraseStart - 1] == ' ' || xml[eraseStart - 1] == '\t')) --eraseStart;
        size_t eraseEnd = end;
        if (eraseStart > 0 && xml[eraseStart - 1] == '\n') --eraseStart;
        if (eraseEnd < xml.size() && xml[eraseEnd] == '\r') ++eraseEnd;
        if (eraseEnd < xml.size() && xml[eraseEnd] == '\n') ++eraseEnd;
        xml.erase(eraseStart, eraseEnd - eraseStart);
    }
    return xml;
}

inline std::string BuildIXML(const Values& values) {
    std::string xml;
    xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml += "<BWFXML>\n";
    xml += "  <IXML_VERSION>1.5</IXML_VERSION>\n";
    xml += "  <USER>\n";
    xml += Section(values);
    xml += "  </USER>\n";
    xml += "</BWFXML>\n";
    return xml;
}

inline std::string MergeIntoIXML(const std::string& existingXml, const Values& values) {
    std::string xml = RemoveSection(existingXml);
    if (values.empty()) return xml;
    const std::string section = Section(values);
    if (xml.empty()) return BuildIXML(values);

    const size_t userStart = FindElementStart(xml, "USER");
    if (userStart != std::string::npos) {
        const size_t userTagEnd = xml.find('>', userStart);
        if (StartTagIsSelfClosing(xml, userStart, userTagEnd)) {
            // "<USER/>" and "<USER />" are the same empty section.
            xml.replace(userStart, userTagEnd + 1 - userStart, "<USER>\n" + section + "  </USER>");
            return xml;
        }
        size_t userEnd = FindCaseInsensitive(xml, "</USER>", userTagEnd == std::string::npos ? userStart : userTagEnd);
        if (userEnd != std::string::npos) {
            if (userEnd > 0 && xml[userEnd - 1] != '\n') xml.insert(userEnd, "\n");
            userEnd = FindCaseInsensitive(xml, "</USER>");
            xml.insert(userEnd, section);
            return xml;
        }
    }
    const size_t bwfEnd = FindCaseInsensitive(xml, "</BWFXML>");
    if (bwfEnd != std::string::npos) {
        xml.insert(bwfEnd, "  <USER>\n" + section + "  </USER>\n");
        return xml;
    }
    return BuildIXML(values);
}

}  // namespace HarkenMeta
