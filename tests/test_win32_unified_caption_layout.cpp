#include "platform/win32_unified_caption_layout.h"

#include <cassert>

int main() {
    using namespace harken::caption;

    const Layout normal = CalculateLayout(800, 600, 8, 32, 46, true, true);
    assert(normal.caption.left == 8 && normal.caption.top == 8);
    assert(normal.caption.right == 792 && normal.caption.bottom == 40);
    assert(normal.close.left == 746 && normal.close.right == 792);
    assert(normal.maximize.left == 700 && normal.maximize.right == 746);
    assert(normal.minimize.left == 654 && normal.minimize.right == 700);
    assert(HitTest(normal, 746, 20) == Hit::Close);
    assert(HitTest(normal, 745, 20) == Hit::Maximize);
    assert(HitTest(normal, 80, 20) == Hit::Caption);
    assert(HitTest(normal, 80, 40) == Hit::None);

    const Layout scaled = CalculateLayout(1200, 800, 12, 48, 69, true, true);
    assert(scaled.close.right == 1188 && scaled.close.left == 1119);
    assert(scaled.caption.bottom == 60);

    const Layout closeOnly = CalculateLayout(500, 400, 8, 32, 46, false, false);
    assert(closeOnly.close.left == 446);
    assert(HitTest(closeOnly, 420, 20) == Hit::Caption);
    assert(HitTest(closeOnly, 446, 20) == Hit::Close);
    return 0;
}
