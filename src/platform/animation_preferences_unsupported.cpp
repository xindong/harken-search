#include "platform/animation_preferences.h"
#ifndef _WIN32
namespace Platform::Animation {
bool ClientAreaAnimationEnabled() noexcept { return false; }
}
#endif
