#pragma once
#include "imgui.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <glad/gl.h>
#include <iostream>
#include "main_window/main_window.cpp"

class FalconUI {
   private:
    GLFWwindow* m_window = nullptr;
    MainWindow main_window_;
    MainWindowController main_window_controller_;

    bool initWindowContext() {
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);

        if (!glfwInit()) {
            std::cerr << "Failed to initialize GLFW\n";
            return false;
        }

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

        m_window = glfwCreateWindow(1280, 720, "Falcon", nullptr, nullptr);
        if (!m_window) {
            std::cerr << "Failed to create GLFW window\n";
            glfwTerminate();
            return false;
        }

        glfwMaximizeWindow(m_window);

        glfwMakeContextCurrent(m_window);
        glfwSwapInterval(1);

        if (!gladLoadGL(glfwGetProcAddress)) {
            std::cerr << "Failed to initialize GLAD loader\n";
            return false;
        }

        std::cout << "OpenGL Version: " << glGetString(GL_VERSION) << "\n";
        std::cout << "GLSL Version: " << glGetString(GL_SHADING_LANGUAGE_VERSION) << "\n";
        int major, minor, rev;
        glfwGetVersion(&major, &minor, &rev);
        printf("GLFW Version: %d.%d.%d\n", major, minor, rev);
        return true;
    }

    bool initImGui() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        RegisterFonts();
        ImGui::StyleColorsDark();

        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 0.0f;
        style.WindowBorderSize = 0.0f;
        style.ChildBorderSize = 0.0f;
        style.PopupBorderSize = 0.0f;
        style.FrameBorderSize = 0.0f;
        style.WindowPadding = ImVec2(0.0f, 0.0f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);

        if (!ImGui_ImplGlfw_InitForOpenGL(m_window, true)) return false;
        if (!ImGui_ImplOpenGL3_Init("#version 460")) return false;

        return true;
    }

    void shutdown() {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        if (m_window) {
            glfwDestroyWindow(m_window);
        }
        glfwTerminate();
    }

   public:
    FalconUI() : main_window_(main_window_controller_) {}

    ~FalconUI() { shutdown(); }

    GLFWwindow* getWindow() const { return m_window; }

    bool setup() {
        if (!initWindowContext()) return false;
        if (!initImGui()) return false;
        return true;
    }

    void run() {
        while (!glfwWindowShouldClose(m_window)) {
            glfwPollEvents();

            int display_w, display_h;
            glfwGetFramebufferSize(m_window, &display_w, &display_h);
            glViewport(0, 0, display_w, display_h);

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);

            ImGuiWindowFlags window_flags =
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

            ImGui::PushStyleColor(ImGuiCol_WindowBg, (ImVec4) ImColor(0xffffffff));
            ImGui::Begin("Falcon", nullptr, window_flags);

            main_window_.render();

            ImGui::End();
            ImGui::PopStyleColor();

            ImGui::Render();

            glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            glfwSwapBuffers(m_window);
        }
    }
};
