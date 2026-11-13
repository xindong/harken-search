#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace aura::cover {

// Pure admission state; callers synchronize access. Tickets prevent a cancelled
// or invalidated probe from publishing over a newer request for the same file.
class IdentityRefreshState {
public:
    std::uint64_t request(const std::string& path, std::uint64_t generation,
                          double now) {
        auto found = entries_.find(path);
        if (found == entries_.end() && entries_.size() >= 10000) {
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                if (!it->second.pending || it->second.generation != generation) {
                    entries_.erase(it);
                    break;
                }
            }
            if (entries_.size() >= 10000) return 0;
        }
        auto& entry = entries_[path];
        if (entry.generation == generation &&
            (entry.pending || now < entry.nextCheck)) return 0;
        entry = {++nextTicket_, generation, 0.0, true};
        return entry.ticket;
    }

    bool current(const std::string& path, std::uint64_t generation,
                 std::uint64_t ticket) const {
        const auto it = entries_.find(path);
        return it != entries_.end() && it->second.pending &&
            it->second.generation == generation && it->second.ticket == ticket;
    }

    void finish(const std::string& path, std::uint64_t generation,
                std::uint64_t ticket, double now) {
        if (!current(path, generation, ticket)) return;
        auto& entry = entries_.at(path);
        entry.pending = false;
        entry.nextCheck = now + 30.0;
    }

    void invalidate(const std::string& path) { entries_.erase(path); }
    void clear() { entries_.clear(); } // Keep ticket numbers monotonic.

private:
    struct Entry {
        std::uint64_t ticket = 0;
        std::uint64_t generation = 0;
        double nextCheck = 0.0;
        bool pending = false;
    };
    std::unordered_map<std::string, Entry> entries_;
    std::uint64_t nextTicket_ = 0;
};

} // namespace aura::cover
