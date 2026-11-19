#pragma once
// AudioDragCard — 拖出音效时跟手的波形小卡。
// 单拖显示波形与文件名；多拖在前卡后加两层薄边与总件数。
// 目标接收后，卡片保持原位轻轻下沉再收起；取消或拒收时直接收起。
// 仅实现 Windows 原生拖影，不参与 OLE 数据交接。

#ifdef _WIN32

#include <functional>
#include <string>
#include <vector>

namespace HarkenSearch::AudioDragCard {

struct CardSpec {
    std::wstring fileName;              // 显示在主卡上的文件名
    int itemCount = 1;                  // 同一次拖出的文件数（>1 显示薄叠层与 ×N）
    std::vector<float> envelope;        // 每根波形柱的幅度（0..1，从左到右）
    bool hasSourceRect = false;         // 从主波形起拖时，卡片由波形区域缩成小卡
    int sourceLeft = 0;                 // 源波形区域（屏幕像素）
    int sourceTop = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
    bool animationsEnabled = true;      // 系统关闭动画时保留卡片信息，不播缩放与落地
    unsigned int hideMainDelayMs = 0;   // >0 时在起拖后按原设置延时收起主窗口
    std::function<void()> onHideMainDelayElapsed;
};

// 把卡片按给定缩放渲染成 BGRA 预乘像素（自上而下），供测试与离屏预览核对。
bool RenderToPixels(const CardSpec& spec, float scaleX, float scaleY,
                    unsigned char captionOpacity, unsigned char stackOpacity,
                    std::vector<unsigned char>& outPixels, int& outWidth, int& outHeight);

// 起拖时调用：创建并显示小卡，开始跟手。
void BeginDrag(const CardSpec& spec);

// OLE 拖放结束：dropped=true 播放轻放动效后收起，false 立即收起。
void EndDrag(bool dropped);

// 安全清理：立即销毁窗口与位图，不播放动效。
void CancelNow();

bool IsActive();

}  // namespace HarkenSearch::AudioDragCard

#endif  // _WIN32
