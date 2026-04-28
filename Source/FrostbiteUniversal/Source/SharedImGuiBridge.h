#pragma once

#if defined(__has_include)
#if __has_include(<imgui.h>)
#include <imgui.h>
#define FROSTBITEUNIVERSAL_HAS_SHARED_IMGUI 1
#else
#define FROSTBITEUNIVERSAL_HAS_SHARED_IMGUI 0
#endif
#else
#define FROSTBITEUNIVERSAL_HAS_SHARED_IMGUI 0
#endif
