#pragma once
#include <functional>
#include <initializer_list>
#include <vector>

namespace harken::settings {
template<class T> struct PreferenceField {
    std::function<bool(const T&, const T&)> equal;
    std::function<void(T&, const T&)> copy;
};

template<class T, class V, class Equal = std::equal_to<V>>
PreferenceField<T> Field(V T::* member, Equal equal = {}) {
    return {
        [=](const T& a, const T& b) { return equal(a.*member, b.*member); },
        [=](T& destination, const T& source) { destination.*member = source.*member; }
    };
}

// UI-thread-owned draft. Only edited preferences are merged into the current
// live value; unrelated layout/runtime changes are never rolled back by Apply.
template<class T> class PreferenceDraft {
public:
    explicit PreferenceDraft(std::initializer_list<PreferenceField<T>> fields) : fields_(fields) {}
    void Begin(const T& value) { baseline_ = value; draft_ = value; }
    T& Value() { return draft_; }
    const T& Value() const { return draft_; }
    const T& Baseline() const { return baseline_; }
    size_t ChangedCount() const {
        size_t count = 0;
        for (const auto& field : fields_) if (!field.equal(baseline_, draft_)) ++count;
        return count;
    }
    T MergeInto(const T& latest) const {
        T result = latest;
        for (const auto& field : fields_)
            if (!field.equal(baseline_, draft_)) field.copy(result, draft_);
        return result;
    }
    void Discard(const T& latest) { Begin(latest); }
    template<class Persist> bool Commit(T& live, Persist persist) {
        if (!ChangedCount()) { Begin(live); return true; }
        T previous = live;
        live = MergeInto(live);
        if (!persist()) { live = std::move(previous); return false; }
        Begin(live);
        return true;
    }
private:
    std::vector<PreferenceField<T>> fields_;
    T baseline_{};
    T draft_{};
};
}
