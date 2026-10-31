#pragma once
#include "TransportContrast.h"
#include "TransportVisualAudit.h"
#include <cmath>

namespace TransportValueText {
// Text only. Group adjacent ink runs and clip glyphs on the CPU so changing
// ink across a moving fill boundary does not create a GPU command per column.
template<class SurfaceAt>
inline void Draw(ImDrawList* draw, ImVec2 position, const char* text,
                 ImVec4 preferred, SurfaceAt surfaceAt, const char* auditId) {
    const auto size=ImGui::CalcTextSize(text);
    float runStart=std::floor(position.x);
    ImU32 runInk=0;
    bool haveRun=false;
    auto flush=[&](float end) {
        if (!haveRun || end<=runStart) return;
        const ImVec4 clip(runStart,position.y,end,position.y+size.y);
        draw->AddText(ImGui::GetFont(),ImGui::GetFontSize(),position,runInk,
            text,nullptr,0.0f,&clip);
    };
    for (float x=std::floor(position.x); x<position.x+size.x; x+=1.f) {
        const auto bg=TransportContrast::Quantize(surfaceAt(x+.5f));
        const auto ink=ImGui::GetColorU32(TransportContrast::ForSurface(preferred,bg,4.5));
        const ImVec2 a(x,position.y), b(std::min(x+1.f,position.x+size.x),position.y+size.y);
        if (!haveRun || ink!=runInk) {
            flush(x);
            runStart=x;
            runInk=ink;
            haveRun=true;
        }
        TransportVisualAudit::Record(auditId,true,ink,ImGui::GetColorU32(bg),a,b);
    }
    flush(position.x+size.x);
}
} // namespace TransportValueText
