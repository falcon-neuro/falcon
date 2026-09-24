#include <thread>
#include "ui/falcon_ui.hpp"

int main() {
    FalconUI ui;
    if (!ui.setup()) {
        return -1;
    }

    GLFWwindow* window = ui.getWindow();

    auto lastFrameTime = std::chrono::steady_clock::now();
    const std::chrono::milliseconds frameTargetDelay{16};

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        auto currentTime = std::chrono::steady_clock::now();
        auto elapsedTime =
            std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastFrameTime);

        if (elapsedTime < frameTargetDelay) {
            std::this_thread::sleep_for(frameTargetDelay - elapsedTime);
            continue;
        }

        lastFrameTime = std::chrono::steady_clock::now();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);

        ImGuiWindowFlags window_flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus;

        ImGui::PushStyleColor(ImGuiCol_WindowBg, (ImVec4) ImColor(0xffffffff));
        ImGui::Begin("Falcon", nullptr, window_flags);
        PushUIFont(118, FontStyle::Bold);
        ImGui::Text("hello");
        PopUIFont();
        ImGui::End();
        ImGui::PopStyleColor();

        ImGui::Render();

        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);

        glViewport(0, 0, display_w, display_h);
        glClearColor(1, 1, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    return 0;
}
