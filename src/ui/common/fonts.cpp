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
    // Inter has glyphs of its own in the private use area, where every icon is. A character is
    // drawn from the first font that has it, so without this Inter's glyphs would hide the icons.
    static constexpr std::array<ImWchar, 3> private_use_area{0xE000, 0xF8FF, 0};
    font_config.GlyphExcludeRanges = private_use_area.data();

    // The icons are merged into every font, so an icon is a character of the text around it and
    // follows that text's size.
    ImFontConfig icon_config;
    icon_config.MergeMode = true;
    icon_config.PixelSnapH = true;
    // No oversampling: it blurs the thin strokes of a small icon, and PixelSnapH already puts an
    // icon on whole pixels.
    icon_config.OversampleH = 1;
    icon_config.OversampleV = 1;
    // An icon is as tall as the text line, yet it is placed on Inter's baseline with its own
    // ascent: 85% of its height, against 80% for Inter. This lowers it by the difference, into the
    // line. To be measured again if either font is replaced.
    constexpr float icon_drop_in_font_sizes = 0.05f;

    constexpr std::array sizes{14, 16, 18, 24, 32, 48, 118};

    const char* regular_path = "assets/Inter-VariableFont_opsz,wght.ttf";
    const char* italic_path = "assets/Inter-Italic-VariableFont_opsz,wght.ttf";
    const char* bold_path = "assets/Inter_24pt-Bold.ttf";
    const char* icon_path = "assets/remixicon.ttf";

    bool is_icon_font_missing = false;
    const auto add_font_with_icons = [&](const char* text_font_path, float pixel_size) -> ImFont* {
        ImFont* text_font =
            imgui_io.Fonts->AddFontFromFileTTF(text_font_path, pixel_size, &font_config);
        // The icons merge into the font added last: without this one, that is another font, or
        // none at all.
        if (text_font == nullptr) {
            return nullptr;
        }
        icon_config.GlyphOffset.y = pixel_size * icon_drop_in_font_sizes;
        if (imgui_io.Fonts->AddFontFromFileTTF(icon_path, pixel_size, &icon_config) == nullptr) {
            is_icon_font_missing = true;
        }
        return text_font;
    };

    for (int size : sizes) {
        auto pixel_size = static_cast<float>(size);

        ui_fonts[{.size = size, .style = FontStyle::Regular}] =
            add_font_with_icons(regular_path, pixel_size);
        ui_fonts[{.size = size, .style = FontStyle::Bold}] =
            add_font_with_icons(bold_path, pixel_size);
        ui_fonts[{.size = size, .style = FontStyle::Italic}] =
            add_font_with_icons(italic_path, pixel_size);
    }

    if (is_icon_font_missing) {
        std::cerr << "Warning: Failed to load the icon font file. Icons will not be drawn.\n";
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

// The size of the text in a side panel, and of the smaller notes that go with it.
inline constexpr int PANEL_TEXT_SIZE = 16;
inline constexpr int PANEL_NOTE_SIZE = 14;

// Text in a size, a style and a colour of its own, wrapped where a wrap position is set.
inline void render_styled_text(const char* text, int size, FontStyle style, const ImVec4& colour) {
    push_ui_font(size, style);
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    pop_ui_font();
}
