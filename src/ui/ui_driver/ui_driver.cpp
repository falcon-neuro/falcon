#pragma once
#include <imgui.h>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include "ui_driver_targets.cpp"

// Lets a test play the mouse and the keys. With the environment variable FALCON_UI_DRIVER set,
// the program draws into a window that isn't shown and, whenever it has nothing left to play,
// waits for a command on its standard input, one a line:
//   frames <count>          draws that many frames
//   mouse_move <x> <y>      in the window's coordinates
//   mouse_down <button>     0 is the left button, 1 the right one
//   mouse_up <button>
//   key <name>[+<name>]     pressed in one frame and let go in the next, e.g. Escape or Ctrl+A
//   text <text>             typed, to the end of the line
//   picture <file>          writes the frame as a PPM picture
//   targets                 answers with what the last frame named (see ui_driver_targets.cpp)
//   state                   answers with the state the UI handed over
//   quit
// Each command is answered with one line on the standard output, once its frames are drawn: the
// marker below, then JSON with "ok" and, if that is false, "error". The end of the input quits.
class UiDriver {
public:
    // Tells an answer from whatever else the program prints.
    static constexpr std::string_view answer_marker = "FALCON_UI_DRIVER ";
    // What a frame lasts for Dear ImGui while the driver is on, whatever it really took: a
    // tooltip's delay or a scroll's speed then takes the same number of frames in every run.
    static constexpr float frame_seconds = 1.0f / 60.0f;

    UiDriver() {
        if (std::getenv("FALCON_UI_DRIVER") != nullptr) {
            ui_driver_targets().turn_on();
        }
    }

    [[nodiscard]] static bool is_on() { return ui_driver_targets().is_on(); }

    // Called before Dear ImGui begins a frame. Takes commands until one needs a frame, and feeds
    // that frame its input. Returns false when the program is to quit.
    [[nodiscard]] bool prepare_frame() {
        while (steps_.empty()) {
            std::string line;
            if (!std::getline(std::cin, line) || !take_command(line)) {
                return false;
            }
        }
        ui_driver_targets().forget_targets();
        feed(steps_.front());
        return true;
    }

    // The file a picture of the frame being drawn is to be written to, if a command asked for one.
    [[nodiscard]] std::optional<std::string> picture_wanted() const {
        if (steps_.empty() || steps_.front().kind != FrameStep::Kind::Picture) {
            return std::nullopt;
        }
        return steps_.front().text;
    }

    // Called after a frame is drawn.
    void finish_frame() {
        if (steps_.empty()) {
            return;
        }
        steps_.pop_front();
        if (steps_.empty()) {
            answer(R"({"ok":true})");
        }
    }

private:
    // What one frame of a command feeds into Dear ImGui before it is drawn.
    struct FrameStep {
        enum class Kind : std::uint8_t { Wait, MouseMove, MouseButton, Keys, Text, Picture };
        Kind kind = Kind::Wait;
        ImVec2 mouse_position;
        int mouse_button = 0;
        // For a mouse button and for keys.
        bool is_down = false;
        std::vector<ImGuiKey> keys;
        // The text to type, or the picture's file.
        std::string text;
    };

    // The frames left of the command being played.
    std::deque<FrameStep> steps_;

    static constexpr int most_frames_in_one_command = 10000;

    static void answer(std::string_view json) {
        std::cout << answer_marker << json << '\n';
        std::cout.flush();
    }

    static void answer_error(std::string_view message) {
        answer(std::format(R"({{"ok":false,"error":{}}})", json_text(message)));
    }

    static void feed(const FrameStep& step) {
        ImGuiIO& imgui_io = ImGui::GetIO();
        switch (step.kind) {
            case FrameStep::Kind::Wait:
            case FrameStep::Kind::Picture:
                break;
            case FrameStep::Kind::MouseMove:
                imgui_io.AddMousePosEvent(step.mouse_position.x, step.mouse_position.y);
                break;
            case FrameStep::Kind::MouseButton:
                imgui_io.AddMouseButtonEvent(step.mouse_button, step.is_down);
                break;
            case FrameStep::Kind::Keys:
                for (const ImGuiKey key : step.keys) {
                    imgui_io.AddKeyEvent(key, step.is_down);
                }
                break;
            case FrameStep::Kind::Text:
                imgui_io.AddInputCharactersUTF8(step.text.c_str());
                break;
        }
    }

    // A command's first word and what follows it on the line.
    struct CommandLine {
        std::string_view word;
        std::string_view rest;
    };

    [[nodiscard]] static CommandLine split_first_word(std::string_view line) {
        const std::size_t word_start = line.find_first_not_of(' ');
        if (word_start == std::string_view::npos) {
            return {};
        }
        line.remove_prefix(word_start);
        const std::size_t word_end = line.find(' ');
        if (word_end == std::string_view::npos) {
            return {.word = line, .rest = {}};
        }
        return {.word = line.substr(0, word_end), .rest = line.substr(word_end + 1)};
    }

    template <typename Number>
    [[nodiscard]] static std::optional<Number> number_written_as(std::string_view text) {
        Number number{};
        const char* const text_begin = std::to_address(text.begin());
        const char* const text_end = std::to_address(text.end());
        const std::from_chars_result result = std::from_chars(text_begin, text_end, number);
        if (result.ec != std::errc{} || result.ptr != text_end) {
            return std::nullopt;
        }
        return number;
    }

    // Returns false for the command that quits.
    [[nodiscard]] bool take_command(std::string_view line) {
        const CommandLine command = split_first_word(line);
        if (command.word == "quit") {
            answer(R"({"ok":true})");
            return false;
        }
        if (command.word == "targets") {
            answer(targets_as_json());
        } else if (command.word == "state") {
            const std::string& state = ui_driver_targets().state();
            answer(std::format(R"({{"ok":true,"state":{}}})", state.empty() ? "null" : state));
        } else if (const std::optional<std::string> error = queue_steps(command)) {
            answer_error(*error);
        }
        return true;
    }

    [[nodiscard]] static std::string targets_as_json() {
        std::string json = R"({"ok":true,"targets":[)";
        bool is_first = true;
        for (const UiDriverTarget& target : ui_driver_targets().targets()) {
            if (!is_first) {
                json += ',';
            }
            is_first = false;
            json += std::format(
                R"({{"name":{},"rectangle":[{},{},{},{}],"note":{}}})", json_text(target.name),
                json_number(target.rectangle.top_left.x), json_number(target.rectangle.top_left.y),
                json_number(target.rectangle.bottom_right.x),
                json_number(target.rectangle.bottom_right.y), json_text(target.note));
        }
        json += "]}";
        return json;
    }

    // JSON has no number for what isn't finite.
    [[nodiscard]] static std::string json_number(float number) {
        return std::isfinite(number) ? std::format("{}", number) : "null";
    }

    // Queues the frames of a command that needs some. Returns what is wrong with the command, if
    // anything is.
    [[nodiscard]] std::optional<std::string> queue_steps(const CommandLine& command) {
        if (command.word == "frames") {
            return queue_waiting_frames(command.rest);
        }
        if (command.word == "mouse_move") {
            return queue_mouse_move(command.rest);
        }
        if (command.word == "mouse_down" || command.word == "mouse_up") {
            return queue_mouse_button(command.rest, command.word == "mouse_down");
        }
        if (command.word == "key") {
            return queue_key(command.rest);
        }
        if (command.word == "text" || command.word == "picture") {
            if (command.rest.empty()) {
                return std::format("{} needs something after it", command.word);
            }
            FrameStep step;
            step.kind = command.word == "text" ? FrameStep::Kind::Text : FrameStep::Kind::Picture;
            step.text = std::string(command.rest);
            steps_.push_back(std::move(step));
            return std::nullopt;
        }
        return std::format("unknown command: {}", command.word);
    }

    [[nodiscard]] std::optional<std::string> queue_waiting_frames(std::string_view count_text) {
        const std::optional<int> count = number_written_as<int>(count_text);
        if (!count.has_value() || *count < 1 || *count > most_frames_in_one_command) {
            return std::format("frames needs a count from 1 to {}", most_frames_in_one_command);
        }
        for (int frame = 0; frame < *count; ++frame) {
            steps_.emplace_back();
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> queue_mouse_move(std::string_view position_text) {
        const CommandLine position = split_first_word(position_text);
        const std::optional<float> horizontal = number_written_as<float>(position.word);
        const std::optional<float> vertical = number_written_as<float>(position.rest);
        if (!horizontal.has_value() || !vertical.has_value()) {
            return "mouse_move needs two numbers";
        }
        FrameStep step;
        step.kind = FrameStep::Kind::MouseMove;
        step.mouse_position = ImVec2(*horizontal, *vertical);
        steps_.push_back(std::move(step));
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> queue_mouse_button(std::string_view button_text,
                                                                bool is_down) {
        const std::optional<int> button = number_written_as<int>(button_text);
        if (!button.has_value() || *button < 0 || *button >= ImGuiMouseButton_COUNT) {
            return "a mouse button is 0 for the left one or 1 for the right one";
        }
        FrameStep step;
        step.kind = FrameStep::Kind::MouseButton;
        step.mouse_button = *button;
        step.is_down = is_down;
        steps_.push_back(std::move(step));
        return std::nullopt;
    }

    [[nodiscard]] static std::optional<ImGuiKey> key_named(std::string_view name) {
        struct NamedKey {
            std::string_view name;
            ImGuiKey key = ImGuiKey_None;
        };
        static constexpr auto named_keys = std::to_array<NamedKey>({
            {.name = "Escape", .key = ImGuiKey_Escape},
            {.name = "Enter", .key = ImGuiKey_Enter},
            {.name = "Tab", .key = ImGuiKey_Tab},
            {.name = "Backspace", .key = ImGuiKey_Backspace},
            {.name = "Delete", .key = ImGuiKey_Delete},
            {.name = "Home", .key = ImGuiKey_Home},
            {.name = "End", .key = ImGuiKey_End},
            {.name = "Left", .key = ImGuiKey_LeftArrow},
            {.name = "Right", .key = ImGuiKey_RightArrow},
            {.name = "Up", .key = ImGuiKey_UpArrow},
            {.name = "Down", .key = ImGuiKey_DownArrow},
            {.name = "Ctrl", .key = ImGuiMod_Ctrl},
            {.name = "Shift", .key = ImGuiMod_Shift},
            {.name = "A", .key = ImGuiKey_A},
        });
        for (const NamedKey& named_key : named_keys) {
            if (named_key.name == name) {
                return named_key.key;
            }
        }
        return std::nullopt;
    }

    // The keys go down together in one frame and come up together in the next.
    [[nodiscard]] std::optional<std::string> queue_key(std::string_view names) {
        std::vector<ImGuiKey> keys;
        while (!names.empty()) {
            const std::size_t name_end = names.find('+');
            const std::string_view name = names.substr(0, name_end);
            const std::optional<ImGuiKey> key = key_named(name);
            if (!key.has_value()) {
                return std::format("unknown key: {}", name);
            }
            keys.push_back(*key);
            names = name_end == std::string_view::npos ? std::string_view{}
                                                       : names.substr(name_end + 1);
        }
        if (keys.empty()) {
            return "key needs a key's name";
        }
        FrameStep keys_down;
        keys_down.kind = FrameStep::Kind::Keys;
        keys_down.is_down = true;
        keys_down.keys = std::move(keys);
        FrameStep keys_up = keys_down;
        keys_up.is_down = false;
        steps_.push_back(std::move(keys_down));
        steps_.push_back(std::move(keys_up));
        return std::nullopt;
    }
};
