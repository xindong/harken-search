#pragma once

#include "ThemeEditorSchema.h"

#include <cmath>
#include <string>
#include <unordered_set>

inline bool ThemeEditorRelationColorsEqual(const ImVec4& left, const ImVec4& right) {
    return std::fabs(left.x - right.x) <= 0.0001f
        && std::fabs(left.y - right.y) <= 0.0001f
        && std::fabs(left.z - right.z) <= 0.0001f
        && std::fabs(left.w - right.w) <= 0.0001f;
}

struct ThemeEditorRelationState {
    std::unordered_set<std::string> independentKeys;

    void ResetToValues(const ThemeColorScheme& scheme) {
        independentKeys.clear();

        size_t relationCount = 0;
        const ThemeEditorRelation* relations = GetThemeEditorRelations(relationCount);
        for (size_t i = 0; i < relationCount; ++i) {
            const ThemeEditorRelation& relation = relations[i];
            if (!ThemeEditorRelationCanFollow(&relation)) continue;

            const ThemeEditorItem* target = FindThemeEditorItem(relation.key);
            const ThemeEditorItem* source = FindThemeEditorItem(relation.sourceKey);
            if (!target || !source) continue;

            const bool independent = target->colorOverride
                ? (scheme.*(target->colorOverride->member)).has_value()
                : !ThemeEditorRelationColorsEqual(target->Color(scheme), source->Color(scheme));
            if (independent)
                independentKeys.insert(relation.key);
        }
    }

    bool IsLinked(const ThemeEditorRelation* relation) const {
        return ThemeEditorRelationCanFollow(relation)
            && independentKeys.find(relation->key) == independentKeys.end();
    }

    void Detach(ThemeColorScheme& scheme, const char* key) {
        const ThemeEditorRelation* relation = FindThemeEditorRelation(key);
        if (!ThemeEditorRelationCanFollow(relation)) return;
        const auto* target = FindThemeEditorItem(key);
        if (target && target->colorOverride)
            target->SetColor(scheme, target->Color(scheme));
        independentKeys.insert(key);
    }

    void Relink(ThemeColorScheme& scheme, const char* key) {
        const ThemeEditorRelation* relation = FindThemeEditorRelation(key);
        if (!ThemeEditorRelationCanFollow(relation)) return;
        const auto* target = FindThemeEditorItem(key);
        if (target && target->colorOverride)
            (scheme.*(target->colorOverride->member)).reset();
        independentKeys.erase(key);
    }

    void Propagate(ThemeColorScheme& scheme) const {
        size_t relationCount = 0;
        const ThemeEditorRelation* relations = GetThemeEditorRelations(relationCount);
        for (size_t pass = 0; pass < relationCount; ++pass) {
            bool changed = false;
            for (size_t i = 0; i < relationCount; ++i) {
                const ThemeEditorRelation& relation = relations[i];
                if (!IsLinked(&relation)) continue;

                const ThemeEditorItem* target = FindThemeEditorItem(relation.key);
                const ThemeEditorItem* source = FindThemeEditorItem(relation.sourceKey);
                if (!target || !source || target->colorOverride) continue;

                ImVec4& targetValue = scheme.*(target->member);
                const ImVec4 sourceValue = source->Color(scheme);
                if (ThemeEditorRelationColorsEqual(targetValue, sourceValue)) continue;

                targetValue = sourceValue;
                changed = true;
            }
            if (!changed) break;
        }
    }
};
