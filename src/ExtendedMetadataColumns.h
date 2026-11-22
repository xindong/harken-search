#pragma once

#include <cstddef>
#include <vector>

// Extended metadata column policy, shared so that every writer of search_data
// agrees on which fields are part of the searchable text.
// rescanOverwrite: a rescan refreshes the value; false preserves user edits.
// searchable: the value contributes to the aggregated search_data column.
struct ExtMetaCol { const char* col; bool rescanOverwrite; bool searchable; };

// Keep this policy header-only. Several behavior tests compile Database.cpp as
// a standalone translation unit, so putting the table in a .cpp file would
// make those otherwise independent targets fail at link time.
inline constexpr ExtMetaCol kExtMetaCols[] = {
    // Basic / Common
    {"subcategory",        true,  true },
    {"source",             true,  false},
    {"cat_id",             true,  false},
    {"category_full",      true,  false},
    {"fx_name",            true,  true },
    {"notes",              true,  false},
    {"user_comments",      false, true },
    {"url",                true,  false},
    {"user_category",      false, false},
    {"vendor_category",    true,  false},
    {"open_tier",          true,  false},
    {"genre",              true,  false},
    {"comment",            true,  false},
    {"copyright",          true,  false},
    {"software",           true,  false},
    // Library / Album
    {"library",            true,  true },
    {"volume",             true,  false},
    {"cd_title",           true,  true },
    {"cd_description",     true,  false},
    {"manufacturer",       true,  false},
    // Music
    {"composer",           true,  true },
    {"arranger",           true,  false},
    {"conductor",          true,  false},
    {"performer",          true,  false},
    {"publisher",          true,  false},
    {"bpm",                true,  false},
    {"musical_key",        true,  false},
    {"tempo",              true,  false},
    {"mood",               false, true },
    {"usage",              false, false},
    {"featured_instrument",true,  false},
    {"version_tag",        true,  false},
    {"era",                true,  false},
    {"track_year",         true,  false},
    {"isrc",               true,  false},
    {"iswc",               true,  false},
    {"lyrics",             true,  false},
    // SFX / Post
    {"show_name",          true,  true },
    {"show_category",      true,  false},
    {"show_subcategory",   true,  false},
    {"designer",           true,  true },
    {"location",           true,  false},
    {"microphone",         true,  false},
    {"mic_perspective",    true,  false},
    {"rec_type",           true,  false},
    {"rec_medium",         true,  false},
    {"short_id",           true,  true },
    {"long_id",            true,  false},
    // Production
    {"scene",              true,  false},
    {"take_num",           true,  false},
    {"tape",               true,  false},
    // BWF / BEXT
    {"bw_description",     true,  false},
    {"bw_originator",      true,  false},
    {"bw_originator_ref",  true,  false},
    {"bw_date",            true,  false},
    {"bw_time",            true,  false},
    {"bw_umid",            true,  false},
    {"bw_coding_history",  true,  false},
    // iXML
    {"ixml_project",       true,  false},
    {"ixml_note",          true,  false},
    {"ixml_file_uid",      true,  false},
    {"ixml_track_layout",  true,  false},
    // Location
    {"shoot_date",         true,  false},
    // System / User
    {"track_colour",       false, false},
};

inline constexpr std::size_t kExtMetaCount = sizeof(kExtMetaCols) / sizeof(kExtMetaCols[0]);

namespace aura::metadata {

// Names of the extended columns whose values belong in search_data, in the order
// the aggregated text appends them.
inline std::vector<const char*> SearchableExtendedColumns() {
    std::vector<const char*> columns;
    for (std::size_t index = 0; index < kExtMetaCount; ++index) {
        if (kExtMetaCols[index].searchable) {
            columns.push_back(kExtMetaCols[index].col);
        }
    }
    return columns;
}

}  // namespace aura::metadata
