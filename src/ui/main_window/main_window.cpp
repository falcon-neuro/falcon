#include <imgui.h>

class MainWindow {
private:
    bool m_isVisible = false;

public:
    MainWindow() = default;
    ~MainWindow() = default;

    void show() {
        m_isVisible = true;
    }

    void hide() {
        m_isVisible = false;
    }

    void render() {
        if (!m_isVisible) return;

        // Render your layout seamlessly inside the frame loop
        ImGui::Begin("Falcon Control Center", &m_isVisible);

        ImGui::Text("Hello from falcon-core! The system is running smoothly.");
        ImGui::Separator();

        if (ImGui::Button("Exit Application")) {
            hide();
        }

        ImGui::End();
    }
};
