#pragma once
#include <imgui.h>
#include <iostream>
#include <map>

enum class FontStyle : std::uint8_t { Regular, Bold, Italic };

struct FontKey {
    int size;
    FontStyle style;

    bool operator<(const FontKey& other) const {
        if (size != other.size) return size < other.size;
        return style < other.style;
    }
};

inline std::map<FontKey, ImFont*> g_Fonts;

void RegisterFonts() {
    ImGuiIO& io = ImGui::GetIO();

    ImFontConfig font_config;
    font_config.OversampleH = 2;
    font_config.OversampleV = 2;
    font_config.PixelSnapH = true;

    const int sizes[] = {14, 16, 18, 24, 32, 48, 118};

    const char* regular_path = "assets/Inter-VariableFont_opsz,wght.ttf";
    const char* italic_path = "assets/Inter-Italic-VariableFont_opsz,wght.ttf";
    const char* bold_path = "assets/Inter_24pt-Bold.ttf";

    for (int size : sizes) {
        float pixel_size = static_cast<float>(size);

        g_Fonts[{size, FontStyle::Regular}] =
            io.Fonts->AddFontFromFileTTF(regular_path, pixel_size, &font_config);
        g_Fonts[{size, FontStyle::Bold}] =
            io.Fonts->AddFontFromFileTTF(bold_path, pixel_size, &font_config);
        g_Fonts[{size, FontStyle::Italic}] =
            io.Fonts->AddFontFromFileTTF(italic_path, pixel_size, &font_config);
    }

    ImFont* custom_font = g_Fonts[{16, FontStyle::Regular}];

    if (custom_font == nullptr) {
        std::cerr
            << "Warning: Failed to load custom TTF font file. Falling back to default font.\n";
    } else {
        io.FontDefault = custom_font;
    }
}

inline void PushUIFont(int size, FontStyle style) {
    auto it = g_Fonts.find({size, style});
    if (it != g_Fonts.end()) {
        ImGui::PushFont(it->second);
    } else {
        ImGui::PushFont(g_Fonts[{16, FontStyle::Regular}]);
    }
}

inline void PopUIFont() {
    ImGui::PopFont();
}
