#pragma once
#include <string>

struct InfoEditValues {
    std::string description, keywords, category;
    bool operator==(const InfoEditValues& other) const {
        return description == other.description && keywords == other.keywords && category == other.category;
    }
};

struct InfoEditSession {
    bool active = false;
    bool awaitingChoice = false;
    InfoEditValues original;
    void Begin(const InfoEditValues& values) { original=values; active=true; awaitingChoice=false; }
    bool Dirty(const InfoEditValues& values) const { return active && !(values == original); }
    bool RequestLeave(const InfoEditValues& values) {
        if (awaitingChoice) return true;
        if (!Dirty(values)) { active=false; return false; }
        awaitingChoice=true;
        return true;
    }
    bool Resolve(bool save, bool saveSucceeded) {
        if (!awaitingChoice || (save && !saveSucceeded)) return false;
        active=false; awaitingChoice=false; return true;
    }
};
