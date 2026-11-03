#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace harken::sidebar {

class MetadataPickerDraft {
public:
    void Sync(std::string_view applied) {
        if (!open_ || std::string_view(committed_) != applied) {
            committed_.assign(applied);
            input_ = committed_;
            open_ = false;
            queryActive_ = false;
        }
    }

    void Open() {
        input_ = committed_;
        queryActive_ = false;
        open_ = true;
    }

    void Edit(std::string_view text) {
        if (!open_) Open();
        input_.assign(text);
        queryActive_ = true;
    }

    void ResetQuery() {
        input_ = committed_;
        queryActive_ = false;
    }

    void Cancel() {
        ResetQuery();
        open_ = false;
    }

    bool Select(std::string_view name) {
        const bool changed = std::string_view(committed_) != name;
        committed_.assign(name);
        input_ = committed_;
        queryActive_ = false;
        open_ = false;
        return changed;
    }

    bool IsOpen() const { return open_; }
    const std::string& DisplayText() const { return input_; }
    std::string InputDisplayText(std::string_view unreadable) const;
    const std::string& Committed() const { return committed_; }
    std::string_view Query() const { return open_ && queryActive_ ? std::string_view(input_) : std::string_view(); }

private:
    std::string committed_;
    std::string input_;
    bool open_ = false;
    bool queryActive_ = false;
};

inline unsigned char FoldAscii(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c - 'A' + 'a') : c;
}

inline bool NameContains(std::string_view name, std::string_view query) {
    if (query.empty()) return true;
    if (query.size() > name.size()) return false;
    for (size_t start = 0; start + query.size() <= name.size(); ++start) {
        size_t matched = 0;
        while (matched < query.size() &&
               FoldAscii(static_cast<unsigned char>(name[start + matched])) ==
                   FoldAscii(static_cast<unsigned char>(query[matched]))) {
            ++matched;
        }
        if (matched == query.size()) return true;
    }
    return false;
}

// The picker keeps the database value for selection. Only its visible label
// drops broken tag bytes, which may already be present in imported libraries.
inline std::string PickerDisplayName(std::string_view raw, std::string_view unreadable) {
    size_t end = raw.size();
    bool cutAtControl = false;
    for (size_t i = 0; i < raw.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c < 0x20 || c == 0x7f) {
            end = i;
            cutAtControl = true;
            break;
        }
    }

    std::string label;
    int replacementCount = 0;
    bool previousReplacement = false;
    for (size_t i = 0; i < end;) {
        if (i + 3 <= end && raw.substr(i, 3) == "\xef\xbf\xbd") {
            ++replacementCount;
            if (!previousReplacement) label += u8"…";
            previousReplacement = true;
            i += 3;
        } else {
            label += raw[i++];
            previousReplacement = false;
        }
    }

    const size_t last = label.find_last_not_of(" \t\r\n");
    if (last == std::string::npos) return std::string(unreadable);
    label.resize(last + 1);
    if (cutAtControl && !label.empty() && label.back() == '-') {
        label.pop_back();
        const size_t beforeHyphen = label.find_last_not_of(" \t\r\n");
        if (beforeHyphen == std::string::npos) return std::string(unreadable);
        label.resize(beforeHyphen + 1);
    }
    if (label.empty() || replacementCount > 2) return std::string(unreadable);
    return label;
}

inline std::string MetadataPickerDraft::InputDisplayText(std::string_view unreadable) const {
    if (queryActive_ || input_.empty()) return input_;
    return PickerDisplayName(input_, unreadable);
}

inline int FirstInitialIndex(const std::vector<std::string>& names, char letter) {
    const unsigned char target = FoldAscii(static_cast<unsigned char>(letter));
    if (target < 'a' || target > 'z') return -1;
    for (size_t i = 0; i < names.size(); ++i) {
        const std::string& name = names[i];
        size_t start = name.find_first_not_of(" \t\r\n");
        if (start != std::string::npos &&
            FoldAscii(static_cast<unsigned char>(name[start])) == target) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

struct SidebarPickerRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

inline SidebarPickerRect ComputePopupLayout(
    const SidebarPickerRect& anchor,
    const SidebarPickerRect& work,
    float preferredWidth = 560.0f,
    float minimumSideWidth = 320.0f,
    float margin = 6.0f,
    float spacing = 8.0f)
{
    const float left = work.x + margin;
    const float right = work.x + std::max(0.0f, work.width) - margin;
    const float availableWidth = std::max(1.0f, right - left);
    const float desiredWidth = std::min(preferredWidth, availableWidth);
    const float sideMinimum = std::min(std::max(minimumSideWidth, anchor.width + 80.0f), availableWidth);
    const float anchorRight = anchor.x + anchor.width;
    const float rightSpace = right - anchorRight - spacing;
    const float leftSpace = anchor.x - spacing - left;

    float x;
    float width;
    if (rightSpace >= sideMinimum) {
        width = std::min(desiredWidth, rightSpace);
        x = anchorRight + spacing;
    } else if (leftSpace >= sideMinimum) {
        width = std::min(desiredWidth, leftSpace);
        x = anchor.x - spacing - width;
    } else {
        width = desiredWidth;
        x = std::clamp(anchorRight + spacing, left, std::max(left, right - width));
    }

    const float top = work.y + margin;
    const float bottom = work.y + std::max(0.0f, work.height) - margin;
    const float latestTop = std::max(top, std::min(work.y + 44.0f, bottom - 80.0f));
    const float y = std::clamp(anchor.y, top, latestTop);
    return {x, y, width, std::max(1.0f, bottom - y)};
}

} // namespace harken::sidebar
