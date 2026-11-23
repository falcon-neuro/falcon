#pragma once
#include <imgui.h>
#include <format>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// What a test that drives the UI clicks on and reads back (see ui_driver.cpp): the UI names what
// can be clicked, with where it drew it in the frame, and hands over its state as JSON. Nothing
// is kept unless the driver is on.

// The text as a JSON string, in its quotes.
[[nodiscard]] inline std::string json_text(std::string_view text) {
    constexpr unsigned first_printable = 0x20U;
    std::string json = "\"";
    for (const char character : text) {
        const auto code = static_cast<unsigned>(static_cast<unsigned char>(character));
        if (character == '"' || character == '\\') {
            json += '\\';
            json += character;
        } else if (code < first_printable) {
            json += std::format("\\u{:04x}", code);
        } else {
            json += character;
        }
    }
    json += '"';
    return json;
}

// Where something was drawn, on the screen.
struct ScreenRectangle {
    ImVec2 top_left;
    ImVec2 bottom_right;
};

// A named thing of the frame drawn last.
struct UiDriverTarget {
    std::string name;
    ScreenRectangle rectangle;
    // What a test may want to know of it besides its place, e.g. why a port can't be clicked.
    std::string note;
};

class UiDriverTargets {
public:
    [[nodiscard]] bool is_on() const { return is_on_; }
    void turn_on() { is_on_ = true; }

    // Called before a frame is drawn, which names its things anew.
    void forget_targets() { targets_.clear(); }

    void add(std::string name, const ScreenRectangle& rectangle, std::string note) {
        targets_.push_back(
            {.name = std::move(name), .rectangle = rectangle, .note = std::move(note)});
    }

    void set_state(std::string state_as_json) { state_as_json_ = std::move(state_as_json); }
    void set_window_title(std::string window_title) { window_title_ = std::move(window_title); }

    [[nodiscard]] const std::vector<UiDriverTarget>& targets() const { return targets_; }
    // JSON, or empty before the first frame.
    [[nodiscard]] const std::string& state() const { return state_as_json_; }
    // The title the window has on the desktop, which no frame shows; empty before the first one.
    [[nodiscard]] const std::string& window_title() const { return window_title_; }

private:
    bool is_on_ = false;
    std::vector<UiDriverTarget> targets_;
    std::string state_as_json_;
    std::string window_title_;
};

// One for the whole program: the UI's parts name their things from wherever they are drawn.
[[nodiscard]] inline UiDriverTargets& ui_driver_targets() {
    static UiDriverTargets targets;
    return targets;
}

// Names a rectangle for a test that drives the UI. The name is its parts joined by "/", e.g.
// {"port", "trigger", "input", "input"}. Does nothing unless the driver is on.
inline void name_rectangle_for_ui_driver(std::initializer_list<std::string_view> name_parts,
                                         const ScreenRectangle& rectangle, std::string note = {}) {
    UiDriverTargets& targets = ui_driver_targets();
    if (!targets.is_on()) {
        return;
    }
    std::string name;
    for (const std::string_view part : name_parts) {
        if (!name.empty()) {
            name += '/';
        }
        name += part;
    }
    targets.add(std::move(name), rectangle, std::move(note));
}

// Names the item Dear ImGui drew last.
inline void name_last_item_for_ui_driver(std::initializer_list<std::string_view> name_parts,
                                         std::string note = {}) {
    if (!ui_driver_targets().is_on()) {
        return;
    }
    name_rectangle_for_ui_driver(
        name_parts, {.top_left = ImGui::GetItemRectMin(), .bottom_right = ImGui::GetItemRectMax()},
        std::move(note));
}
