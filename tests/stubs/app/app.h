#pragma once
#include <cstdint>
#include <string>

// Lightweight host Settings stub matching XboxXBE-Recomp/runtime/app/app.h fields used by mod_schema.
namespace app {

enum class DisplayMode : uint32_t { Fullscreen, Borderless, Windowed };

struct Settings {
    DisplayMode display_mode = DisplayMode::Borderless;
    uint32_t width = 0, height = 0;
    uint32_t render_percent = 100;
    uint32_t fps_cap = 60;
    bool vsync = true;
    uint32_t aniso = 0;
    std::string join_ip;
};

}  // namespace app

