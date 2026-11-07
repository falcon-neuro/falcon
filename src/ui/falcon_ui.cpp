#pragma once
#include "imgui.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <glad/gl.h>
#include <iostream>
#include <print>
#include "main_window/main_window.cpp"

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

        window_ = glfwCreateWindow(1280, 720, "Falcon", nullptr, nullptr);
        if (!window_) {
            std::cerr << "Failed to create GLFW window\n";
            glfwTerminate();
            return false;
        }

        glfwMaximizeWindow(window_);

        glfwMakeContextCurrent(window_);
        glfwSwapInterval(1);

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
        style.PopupBorderSize = 0.0f;
        style.FrameBorderSize = 0.0f;
        style.WindowPadding = ImVec2(0.0f, 0.0f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);

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

    GLFWwindow* get_window() const { return window_; }

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

            int display_w = 0;
            int display_h = 0;
            glfwGetFramebufferSize(window_, &display_w, &display_h);
            glViewport(0, 0, display_w, display_h);

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
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

            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImColor(0xffffffff).Value);
            ImGui::Begin("Falcon", nullptr, window_flags);

            main_window_.render();

            ImGui::End();
            ImGui::PopStyleColor();

            ImGui::Render();

            glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            glfwSwapBuffers(window_);
        }
    }
};
