#pragma once
#include <imgui.h>
#include <array>
#include <iostream>
#include <map>

enum class FontStyle : std::uint8_t { Regular, Bold, Italic };

struct FontKey {
    int size;
    FontStyle style;

    bool operator<(const FontKey& other) const {
        if (size != other.size) {
            return size < other.size;
        }
        return style < other.style;
    }
};

inline std::map<FontKey, ImFont*> ui_fonts;

void register_fonts() {
    ImGuiIO& imgui_io = ImGui::GetIO();

    ImFontConfig font_config;
    font_config.OversampleH = 2;
    font_config.OversampleV = 2;
    font_config.PixelSnapH = true;

    constexpr std::array sizes{14, 16, 18, 24, 32, 48, 118};

    const char* regular_path = "assets/Inter-VariableFont_opsz,wght.ttf";
    const char* italic_path = "assets/Inter-Italic-VariableFont_opsz,wght.ttf";
    const char* bold_path = "assets/Inter_24pt-Bold.ttf";

    for (int size : sizes) {
        auto pixel_size = static_cast<float>(size);

        ui_fonts[{.size = size, .style = FontStyle::Regular}] =
            imgui_io.Fonts->AddFontFromFileTTF(regular_path, pixel_size, &font_config);
        ui_fonts[{.size = size, .style = FontStyle::Bold}] =
            imgui_io.Fonts->AddFontFromFileTTF(bold_path, pixel_size, &font_config);
        ui_fonts[{.size = size, .style = FontStyle::Italic}] =
            imgui_io.Fonts->AddFontFromFileTTF(italic_path, pixel_size, &font_config);
    }

    ImFont* custom_font = ui_fonts[{.size = 16, .style = FontStyle::Regular}];

    if (custom_font == nullptr) {
        std::cerr
            << "Warning: Failed to load custom TTF font file. Falling back to default font.\n";
    } else {
        imgui_io.FontDefault = custom_font;
    }
}

inline void push_ui_font(int size, FontStyle style) {
    auto found = ui_fonts.find({.size = size, .style = style});
    if (found != ui_fonts.end()) {
        ImGui::PushFont(found->second);
    } else {
        ImGui::PushFont(ui_fonts[{.size = 16, .style = FontStyle::Regular}]);
    }
}

inline void pop_ui_font() {
    ImGui::PopFont();
}
