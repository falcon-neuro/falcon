#pragma once
#include <glad/gl.h>
#include <imgui.h>
#include <stb_image.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <format>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>

// A picture the UI draws, e.g. a processor's icon or Falcon's logo, loaded from a PNG under
// assets/png. Only how solid each pixel is comes from the file: the colour is given where the
// picture is drawn.
struct Picture {
    // The texture's name in OpenGL, 0 for a picture that couldn't be loaded.
    GLuint texture = 0;
    float width = 0.0f;
    float height = 0.0f;
    // The smallest box that holds every pixel that isn't clear, in pixels from the picture's top
    // left corner: a picture may have clear room around what it shows.
    ImVec2 solid_top_left{0.0f, 0.0f};
    ImVec2 solid_bottom_right{0.0f, 0.0f};

    [[nodiscard]] bool is_loaded() const { return texture != 0; }
};

struct StbImageDeleter {
    void operator()(stbi_uc* pixels) const { stbi_image_free(pixels); }
};

// Reads assets/png/<name>.png into a texture. A file that can't be read gives a picture that
// isn't loaded, and a line on the error output.
[[nodiscard]] inline Picture load_picture(std::string_view name) {
    constexpr int channels_per_pixel = 4;
    constexpr stbi_uc white = 255;
    const std::string path = std::format("assets/png/{}.png", name);
    int width = 0;
    int height = 0;
    int channels_in_file = 0;
    const std::unique_ptr<stbi_uc, StbImageDeleter> file_pixels(
        stbi_load(path.c_str(), &width, &height, &channels_in_file, channels_per_pixel));
    if (file_pixels == nullptr || width <= 0 || height <= 0) {
        std::cerr << "Warning: Failed to load the picture " << path << ". It will not be drawn.\n";
        return {};
    }
    const std::span<stbi_uc> pixels(
        file_pixels.get(),
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * channels_per_pixel);
    // The file's own colour is dropped: every pixel is white and as solid as the file has it, so
    // that the picture takes whichever colour it is drawn in.
    for (std::size_t pixel_start = 0; pixel_start < pixels.size();
         pixel_start += channels_per_pixel) {
        pixels[pixel_start] = white;
        pixels[pixel_start + 1] = white;
        pixels[pixel_start + 2] = white;
    }

    // The box of the pixels that aren't clear, which stays empty for a picture that is all clear.
    constexpr std::size_t solidity_in_pixel = 3;
    const auto pixels_in_row = static_cast<std::size_t>(width);
    const auto row_count = static_cast<std::size_t>(height);
    std::size_t solid_left = pixels_in_row;
    std::size_t solid_top = row_count;
    std::size_t solid_right = 0;
    std::size_t solid_bottom = 0;
    for (std::size_t row = 0; row < row_count; ++row) {
        for (std::size_t column = 0; column < pixels_in_row; ++column) {
            const std::size_t pixel_start = ((row * pixels_in_row) + column) * channels_per_pixel;
            if (pixels[pixel_start + solidity_in_pixel] == 0) {
                continue;
            }
            solid_left = std::min(solid_left, column);
            solid_top = std::min(solid_top, row);
            solid_right = std::max(solid_right, column + 1);
            solid_bottom = std::max(solid_bottom, row + 1);
        }
    }
    const bool has_solid_pixel = solid_right > solid_left;

    // Each smaller size down to one pixel, which OpenGL makes from the file's: a picture drawn
    // smaller than its file is taken from them and stays smooth.
    const auto size_count =
        static_cast<GLsizei>(std::bit_width(static_cast<unsigned>(std::max(width, height))));
    GLuint texture = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &texture);
    glTextureStorage2D(texture, size_count, GL_RGBA8, width, height);
    glTextureSubImage2D(texture, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glGenerateTextureMipmap(texture);
    glTextureParameteri(texture, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(texture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(texture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(texture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return {
        .texture = texture,
        .width = static_cast<float>(width),
        .height = static_cast<float>(height),
        .solid_top_left =
            has_solid_pixel ? ImVec2(static_cast<float>(solid_left), static_cast<float>(solid_top))
                            : ImVec2(0.0f, 0.0f),
        .solid_bottom_right = has_solid_pixel ? ImVec2(static_cast<float>(solid_right),
                                                       static_cast<float>(solid_bottom))
                                              : ImVec2(0.0f, 0.0f),
    };
}

// The picture assets/png/<name>.png, loaded when it is first asked for. It is kept until the app
// ends, when its texture goes with the window's OpenGL context. A picture that couldn't be loaded
// is kept too, so its file is tried once.
[[nodiscard]] inline const Picture& picture_named(std::string_view name) {
    static std::map<std::string, Picture, std::less<>> pictures;
    const auto found = pictures.find(name);
    if (found != pictures.end()) {
        return found->second;
    }
    return pictures.emplace(std::string(name), load_picture(name)).first->second;
}

// Where a picture is drawn on the screen.
struct PictureBox {
    ImVec2 top_left;
    ImVec2 size;
};

// Draws the part of a picture that isn't clear into a box, in a colour, leaving out the clear
// room around it. The box starts on a whole pixel, where a picture is sharp. A picture that isn't
// loaded draws nothing.
inline void draw_solid_part_of_picture(ImDrawList* draw_list, const Picture& picture,
                                       const PictureBox& box, const ImVec4& colour) {
    if (!picture.is_loaded()) {
        return;
    }
    const ImVec2 top_left(std::floor(box.top_left.x), std::floor(box.top_left.y));
    draw_list->AddImage(
        ImTextureRef(static_cast<ImTextureID>(picture.texture)), top_left,
        ImVec2(top_left.x + box.size.x, top_left.y + box.size.y),
        ImVec2(picture.solid_top_left.x / picture.width, picture.solid_top_left.y / picture.height),
        ImVec2(picture.solid_bottom_right.x / picture.width,
               picture.solid_bottom_right.y / picture.height),
        ImGui::GetColorU32(colour));
}

// Draws a picture into a box, in a colour. The box starts on a whole pixel, where a picture is
// sharp. A picture that isn't loaded draws nothing.
inline void draw_picture(ImDrawList* draw_list, const Picture& picture, const PictureBox& box,
                         const ImVec4& colour) {
    if (!picture.is_loaded()) {
        return;
    }
    const ImVec2 top_left(std::floor(box.top_left.x), std::floor(box.top_left.y));
    draw_list->AddImage(ImTextureRef(static_cast<ImTextureID>(picture.texture)), top_left,
                        ImVec2(top_left.x + box.size.x, top_left.y + box.size.y),
                        ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), ImGui::GetColorU32(colour));
}
