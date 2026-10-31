#pragma once
#include "imgui.h"
#include <array>
#include <iomanip>
#include <locale>
#include <sstream>
#include <vector>
namespace EmptyStateVisualAudit {
struct Item { const char* id; ImU32 foreground; std::array<ImVec4,4> backgrounds; ImVec2 min,max; };
inline int until=-1,frame=-1,theme=-1;
inline ImVec2 min,max;
inline std::vector<Item> items;
inline bool Collecting() { return ImGui::GetFrameCount()<=until; }
inline void Request() { until=ImGui::GetFrameCount()+300; }
inline void Begin(int id) {
    if (!Collecting()) return;
    frame=ImGui::GetFrameCount(); theme=id; items.clear();
}
inline void Record(const char* id, const std::array<ImVec4,4>& backgrounds) {
    if (!Collecting()) return;
    items.push_back({id,ImGui::GetColorU32(ImGuiCol_Text),backgrounds,
        ImGui::GetItemRectMin(),ImGui::GetItemRectMax()});
}
inline void End(ImVec2 a,ImVec2 b) { if(Collecting()) { min=a;max=b; } }
inline std::string Json() {
    std::ostringstream out; out.imbue(std::locale::classic()); out<<std::setprecision(9);
    auto rect=[&](ImVec2 a,ImVec2 b) { out<<'['<<a.x<<','<<a.y<<','<<b.x<<','<<b.y<<']'; };
    auto color=[&](ImU32 c) { out<<'['<<((c>>IM_COL32_R_SHIFT)&255)<<','<<((c>>IM_COL32_G_SHIFT)&255)<<','<<((c>>IM_COL32_B_SHIFT)&255)<<','<<((c>>IM_COL32_A_SHIFT)&255)<<']'; };
    out<<"{\"ok\":true,\"frame\":"<<frame<<",\"current_frame\":"<<ImGui::GetFrameCount()<<",\"theme\":"<<theme<<",\"panel\":";
    rect(min,max);out<<",\"checks\":[";
    for(size_t i=0;i<items.size();++i) {
        auto& v=items[i];if(i)out<<',';
        out<<"{\"id\":"<<std::quoted(v.id)<<",\"fg\":";color(v.foreground);
        out<<",\"backgrounds\":[";
        for(size_t j=0;j<v.backgrounds.size();++j) { if(j)out<<',';color(ImGui::ColorConvertFloat4ToU32(v.backgrounds[j])); }
        out<<"],\"bounds\":";rect(v.min,v.max);out<<'}';
    }
    out<<"]}";return out.str();
}
} // namespace EmptyStateVisualAudit
