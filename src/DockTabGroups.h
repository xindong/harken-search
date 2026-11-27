#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aura::ui::dock {

inline bool IsDockTabPanel(std::string_view id) {
    return id == "filter" || id == "info" || id == "similar-sounds" ||
        id == "mod-ucs-browser" || id == "mod-tag-browser" ||
        id == "mod-workflow" || id == "mod-transfer" || id == "mod-batch-edit";
}

class DockTabGroups {
public:
    struct Group {
        unsigned id;
        std::vector<std::string> members;
        std::string active;
        std::string anchor;
    };

    const Group* Find(std::string_view member) const {
        for (const auto& group : groups_)
            if (std::find(group.members.begin(), group.members.end(), member) != group.members.end())
                return &group;
        return nullptr;
    }

    const std::vector<Group>& All() const { return groups_; }

    bool Merge(std::string_view source, std::string_view target) {
        if (source == target || !IsDockTabPanel(source) || !IsDockTabPanel(target)) return false;
        const auto* targetGroup = Find(target);
        const auto* sourceGroup = Find(source);
        if (sourceGroup && sourceGroup == targetGroup) return false;
        const std::string selected(source);
        if (!targetGroup) {
            const std::string anchor(target);
            if (!sourceGroup) {
                groups_.push_back({nextId_++, {anchor, selected}, selected, anchor});
            } else {
                auto& group = *FindById(sourceGroup->id);
                group.members.insert(group.members.begin(), anchor);
                group.active = selected;
                group.anchor = anchor;
            }
        } else {
            auto& destination = *FindById(targetGroup->id);
            if (sourceGroup) {
                const unsigned sourceId = sourceGroup->id;
                destination.members.insert(destination.members.end(),
                    sourceGroup->members.begin(), sourceGroup->members.end());
                destination.active = selected;
                groups_.erase(std::remove_if(groups_.begin(), groups_.end(),
                    [sourceId](const Group& group) { return group.id == sourceId; }), groups_.end());
            } else {
                destination.members.push_back(selected);
                destination.active = selected;
            }
        }
        return true;
    }

    bool Select(unsigned id, std::string_view member) {
        auto* group = FindById(id);
        if (!group || group->active == member ||
            std::find(group->members.begin(), group->members.end(), member) == group->members.end())
            return false;
        group->active = member;
        return true;
    }

    bool Reorder(unsigned id, const std::vector<std::string>& visibleOrder) {
        auto* group = FindById(id);
        if (!group || visibleOrder.size() < 2 || visibleOrder.size() > group->members.size()) return false;
        for (const auto& member : visibleOrder) {
            if (std::count(visibleOrder.begin(), visibleOrder.end(), member) != 1 ||
                std::find(group->members.begin(), group->members.end(), member) == group->members.end())
                return false;
        }
        auto members = group->members;
        auto next = visibleOrder.begin();
        for (auto& member : members) {
            if (std::find(visibleOrder.begin(), visibleOrder.end(), member) != visibleOrder.end())
                member = *next++;
        }
        if (group->members == members) return false;
        group->members = std::move(members);
        return true;
    }

    bool Detach(std::string_view member) {
        const auto* found = Find(member);
        if (!found) return false;
        const unsigned id = found->id;
        auto& group = *FindById(id);
        group.members.erase(std::remove(group.members.begin(), group.members.end(), member), group.members.end());
        if (group.members.size() < 2) {
            groups_.erase(std::remove_if(groups_.begin(), groups_.end(),
                [id](const Group& value) { return value.id == id; }), groups_.end());
        } else {
            if (group.active == member) group.active = group.members.front();
            if (group.anchor == member) group.anchor = group.members.front();
        }
        return true;
    }

    bool Remove(unsigned id) {
        const auto found = std::find_if(groups_.begin(), groups_.end(),
            [id](const Group& group) { return group.id == id; });
        if (found == groups_.end()) return false;
        groups_.erase(found);
        return true;
    }

    std::string Save() const {
        std::string text;
        for (const auto& group : groups_) {
            if (!text.empty()) text += ';';
            text += group.active;
            if (group.anchor != group.members.front()) {
                text += '@';
                text += group.anchor;
            }
            text += ':';
            for (size_t index = 0; index < group.members.size(); ++index) {
                if (index > 0) text += ',';
                text += group.members[index];
            }
        }
        return text;
    }

    void Load(std::string_view text) {
        groups_.clear();
        if (text.size() > 1024) return;
        while (!text.empty()) {
            const size_t end = text.find(';');
            const auto entry = text.substr(0, end);
            const size_t separator = entry.find(':');
            if (separator != std::string_view::npos) {
                const auto header = entry.substr(0, separator);
                const auto anchorSeparator = header.find('@');
                Group group{nextId_, {}, std::string(header.substr(0, anchorSeparator)),
                    anchorSeparator == std::string_view::npos ? std::string{} : std::string(header.substr(anchorSeparator + 1))};
                auto members = entry.substr(separator + 1);
                bool valid = !members.empty();
                while (!members.empty() && valid) {
                    const size_t comma = members.find(',');
                    const auto member = members.substr(0, comma);
                    valid = (IsDockTabPanel(member) || member == "search-results") && !Find(member) &&
                        std::find(group.members.begin(), group.members.end(), member) == group.members.end();
                    if (valid && member != "search-results") group.members.emplace_back(member);
                    if (comma == std::string_view::npos) break;
                    members.remove_prefix(comma + 1);
                    if (members.empty()) valid = false;
                }
                if (group.active == "search-results" && !group.members.empty())
                    group.active = group.members.front();
                if (group.anchor.empty() && !group.members.empty()) group.anchor = group.members.front();
                if (valid && group.members.size() > 1 &&
                    std::find(group.members.begin(), group.members.end(), group.active) != group.members.end() &&
                    std::find(group.members.begin(), group.members.end(), group.anchor) != group.members.end()) {
                    ++nextId_;
                    groups_.push_back(std::move(group));
                }
            }
            if (end == std::string_view::npos) break;
            text.remove_prefix(end + 1);
        }
    }

private:
    Group* FindById(unsigned id) {
        for (auto& group : groups_)
            if (group.id == id) return &group;
        return nullptr;
    }

    std::vector<Group> groups_;
    unsigned nextId_ = 1;
};

}
