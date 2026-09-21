#include <iostream>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include "main_window/main_window.cpp"

class FalconUI {
private:
    GLFWwindow* m_window = nullptr;
    MainWindow m_mainWindow;

    bool initWindowContext() {
        if (!glfwInit()) {
            std::cerr << "Failed to initialize GLFW\n";
            return false;
        }

        // Target OpenGL 3.3 Core Profile
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

        m_window = glfwCreateWindow(1280, 720, "Falcon Engine Core", nullptr, nullptr);
        if (!m_window) {
            std::cerr << "Failed to create GLFW window\n";
            glfwTerminate();
            return false;
        }

        glfwMakeContextCurrent(m_window);
        glfwSwapInterval(1); // Enable v-sync
        return true;
    }

    bool initImGui() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        ImGui::StyleColorsDark();

        // Setup Platform/Renderer backends
        if (!ImGui_ImplGlfw_InitForOpenGL(m_window, true)) return false;
        if (!ImGui_ImplOpenGL3_Init("#version 330")) return false;

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
    FalconUI() = default;
    ~FalconUI() {
        shutdown();
    }

    bool launch() {
        if (!initWindowContext()) return false;
        if (!initImGui()) return false;

        // Ready the window state without executing immediate draw commands
        m_mainWindow.show();

        // Core Frame Rendering Loop
        while (!glfwWindowShouldClose(m_window)) {
            glfwPollEvents();

            // Start the ImGui frame
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            // Let components declare their visual state safely
            m_mainWindow.render();

            // Finalize rendering pipelines
            ImGui::Render();

            int display_w, display_h;
            glfwGetFramebufferSize(m_window, &display_w, &display_h);
            glViewport(0, 0, display_w, display_h);
            glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(m_window);
        }

        return true;
    }
};
