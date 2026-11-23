#pragma once
#include "imgui.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <glad/gl.h>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <print>
#include <string>
#include <utility>
#include <vector>
#include "common/colours.cpp"
#include "main_window/main_window.cpp"
#include "ui_driver/ui_driver.cpp"

// Everything about this UI is just a "demo" for checking
// capabilities of imgui and opengl and compute shaders.
// This UI only has signal viewer, yet it should be the
// main ui where graph design happens, as well as graph management,
// sort of like open ephys 2026 alternative but much more sophisticated
// and of course capable of interfacing with many more ephys systems
// and do closed loop with sub 1ms latency on massive electrode arrays.
class FalconUI {
private:
    GLFWwindow* window_ = nullptr;
    MainWindow main_window_;
    MainWindowController main_window_controller_;
    // The file a picture of the window is written to, from the environment variable
    // FALCON_SCREENSHOT; empty when that isn't set. With it, the program draws into a window that
    // isn't shown, writes the picture and quits, so the UI can be looked at without a screen.
    std::string screenshot_path_;
    int drawn_frame_count_ = 0;
    // The title the window was given last, so that the desktop is told of a new one only. Empty
    // before the first frame.
    std::string window_title_;
    // On when the environment variable FALCON_UI_DRIVER is set: a test then plays the mouse and
    // the keys, in a window that isn't shown either.
    UiDriver ui_driver_;

    static constexpr int screenshot_width = 1600;
    static constexpr int screenshot_height = 1000;
    // What sizes itself to what it holds has its size only from its second frame on, and what
    // that holds from the frame after.
    static constexpr int frames_before_screenshot = 8;

    // Writes what was just drawn as a PPM picture. OpenGL hands the rows over bottom first, and a
    // picture has its top row first.
    static void write_screenshot(const std::string& path, int width, int height) {
        if (width <= 0 || height <= 0) {
            return;
        }
        const std::size_t row_size = static_cast<std::size_t>(width) * 3;
        const auto row_count = static_cast<std::size_t>(height);
        std::vector<char> pixels(row_size * row_count);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
        std::ofstream file(path, std::ios::binary);
        file << "P6\n" << width << ' ' << height << "\n255\n";
        for (std::size_t row = row_count; row > 0; --row) {
            file.write(&pixels.at((row - 1) * row_size), static_cast<std::streamsize>(row_size));
        }
    }

    void update_window_title() {
        std::string title = main_window_.window_title();
        if (title == window_title_) {
            return;
        }
        glfwSetWindowTitle(window_, title.c_str());
        ui_driver_targets().set_window_title(title);
        window_title_ = std::move(title);
    }

    bool init_window_context() {
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);

        if (glfwInit() == GLFW_FALSE) {
            std::cerr << "Failed to initialize GLFW\n";
            return false;
        }

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

        if (const char* path = std::getenv("FALCON_SCREENSHOT")) {
            screenshot_path_ = path;
        }
        const bool is_window_hidden = !screenshot_path_.empty() || UiDriver::is_on();
        if (is_window_hidden) {
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        }
        window_ = glfwCreateWindow(is_window_hidden ? screenshot_width : 1280,
                                   is_window_hidden ? screenshot_height : 720, MainWindow::app_name,
                                   nullptr, nullptr);
        if (!window_) {
            std::cerr << "Failed to create GLFW window\n";
            glfwTerminate();
            return false;
        }

        if (!is_window_hidden) {
            glfwMaximizeWindow(window_);
        }

        glfwMakeContextCurrent(window_);
        // A window that isn't shown is never told that its frame reached the screen, which a swap
        // in step with the screen waits for.
        glfwSwapInterval(is_window_hidden ? 0 : 1);

        if (gladLoadGL(glfwGetProcAddress) == 0) {  // returns the GL version, 0 on failure
            std::cerr << "Failed to initialize GLAD loader\n";
            return false;
        }

        std::cout << "OpenGL Version: " << glGetString(GL_VERSION) << "\n";
        std::cout << "GLSL Version: " << glGetString(GL_SHADING_LANGUAGE_VERSION) << "\n";
        int major = 0;
        int minor = 0;
        int rev = 0;
        glfwGetVersion(&major, &minor, &rev);
        std::println("GLFW Version: {}.{}.{}", major, minor, rev);

        // TODO: remove after Intel iGPU diagnostics
        GLint max_tex = 0;
        GLint max_ssbo = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
        glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &max_ssbo);
        std::cout << "Renderer: " << glGetString(GL_RENDERER) << "\n"
                  << "MAX_TEXTURE_SIZE: " << max_tex << "\n"
                  << "MAX_SSBO_BLOCK: " << max_ssbo << "\n";
        return true;
    }

    bool init_imgui() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& imgui_io = ImGui::GetIO();
        // ImGui's flags are signed ints by its API
        // NOLINTNEXTLINE(bugprone-signed-bitwise)
        imgui_io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        register_fonts();
        ImGui::StyleColorsDark();

        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 0.0f;
        style.WindowBorderSize = 0.0f;
        style.ChildBorderSize = 0.0f;
        // A tooltip takes the rounding of a window and the border of a popup.
        style.WindowRounding = CARD_ROUNDING;
        style.PopupBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;
        style.FrameRounding = FRAME_ROUNDING;
        style.GrabRounding = FRAME_ROUNDING;
        style.ScrollbarRounding = FRAME_ROUNDING;
        style.PopupRounding = CARD_ROUNDING;
        style.FramePadding = ImVec2(10.0f, 5.0f);
        style.WindowPadding = ImVec2(0.0f, 0.0f);
        style.Colors[ImGuiCol_Text] = TEXT_COLOUR;
        style.Colors[ImGuiCol_Border] = CARD_BORDER_COLOUR;
        style.Colors[ImGuiCol_InputTextCursor] = TEXT_COLOUR;

        if (!ImGui_ImplGlfw_InitForOpenGL(window_, true)) {
            return false;
        }
        if (!ImGui_ImplOpenGL3_Init("#version 460")) {
            return false;
        }

        return true;
    }

    void shutdown() {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        if (window_) {
            glfwDestroyWindow(window_);
        }
        glfwTerminate();
    }

public:
    FalconUI() : main_window_(main_window_controller_) {}

    ~FalconUI() { shutdown(); }

    [[nodiscard]] GLFWwindow* get_window() const { return window_; }

    bool setup() {
        if (!init_window_context()) {
            return false;
        }
        if (!init_imgui()) {
            return false;
        }
        return true;
    }

    void run() {
        while (glfwWindowShouldClose(window_) == GLFW_FALSE) {
            glfwPollEvents();
            if (UiDriver::is_on() && !ui_driver_.prepare_frame()) {
                break;
            }

            int display_w = 0;
            int display_h = 0;
            glfwGetFramebufferSize(window_, &display_w, &display_h);
            glViewport(0, 0, display_w, display_h);

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            if (UiDriver::is_on()) {
                ImGui::GetIO().DeltaTime = UiDriver::frame_seconds;
            }
            ImGui::NewFrame();

            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);

            // ImGui's flags are signed ints by its API
            // NOLINTBEGIN(bugprone-signed-bitwise)
            ImGuiWindowFlags window_flags =
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
            // NOLINTEND(bugprone-signed-bitwise)

            ImGui::PushStyleColor(ImGuiCol_WindowBg, WINDOW_COLOUR);
            ImGui::Begin("Falcon", nullptr, window_flags);

            main_window_.render();
            update_window_title();

            ImGui::End();
            ImGui::PopStyleColor();

            ImGui::Render();

            glClearColor(WINDOW_COLOUR.x, WINDOW_COLOUR.y, WINDOW_COLOUR.z, WINDOW_COLOUR.w);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            if (!screenshot_path_.empty()) {
                ++drawn_frame_count_;
                if (drawn_frame_count_ == frames_before_screenshot) {
                    write_screenshot(screenshot_path_, display_w, display_h);
                    glfwSetWindowShouldClose(window_, GLFW_TRUE);
                }
            }
            if (UiDriver::is_on()) {
                if (const std::optional<std::string> picture = ui_driver_.picture_wanted()) {
                    write_screenshot(*picture, display_w, display_h);
                }
                ui_driver_.finish_frame();
            }

            glfwSwapBuffers(window_);
        }
    }
};
