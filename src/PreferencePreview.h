#pragma once
#include "PreferenceDraft.h"

namespace harken::settings {
// A UI-thread-only render overlay. Persistence and editing always see the
// committed fields; unrelated live changes survive restoration.
template<class T> class PreferencePreview {
public:
    explicit PreferencePreview(std::initializer_list<PreferenceField<T>> fields) : fields_(fields) {}
    void Begin(T& live, const T& baseline, const T& draft) {
        Restore(live);
        committed_ = live;
        for (const auto& field : fields_)
            if (!field.equal(baseline, draft)) {
                selected_.push_back(field);
                field.copy(live, draft);
            }
    }
    T ForPersistence(const T& live) const {
        T result = live;
        for (const auto& field : selected_) field.copy(result, committed_);
        return result;
    }
    void Restore(T& live) {
        for (const auto& field : selected_) field.copy(live, committed_);
        selected_.clear();
    }
    bool Active() const { return !selected_.empty(); }
private:
    std::vector<PreferenceField<T>> fields_, selected_;
    T committed_{};
};
}
