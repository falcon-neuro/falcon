#pragma once
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include "../common/colours.cpp"
#include "../common/icons.cpp"
#include "../common/instrument.cpp"
#include "../pipeline_designer/pipeline_writer.cpp"
#include "../pipeline_designer/tooltip.cpp"
#include "../ui_driver/ui_driver_targets.cpp"

// A pipeline's name and the name of its folder, when a pipeline is made and when one is renamed,
// so that the user sees and sets both. The name is typed in a field. Under it, once the name is
// touched, a line shows where the pipeline lies, with a pencil that turns the folder's name into a
// field. The folder's name follows the pipeline's while that is typed, until the user types a
// folder of their own.

inline constexpr std::size_t LONGEST_PIPELINE_NAME = 120;

// A folder's name for a pipeline's name: its letters and digits in lower case, with one
// underscore for whatever lies between them. Empty for a name that has neither.
[[nodiscard]] inline std::string folder_name_for(std::string_view pipeline_name) {
    std::string folder_name;
    for (const char character : pipeline_name) {
        const bool is_lower_case = character >= 'a' && character <= 'z';
        const bool is_upper_case = character >= 'A' && character <= 'Z';
        const bool is_digit = character >= '0' && character <= '9';
        if (is_lower_case || is_digit) {
            folder_name += character;
        } else if (is_upper_case) {
            folder_name += static_cast<char>(character - 'A' + 'a');
        } else if (!folder_name.empty() && folder_name.back() != '_') {
            folder_name += '_';
        }
    }
    while (!folder_name.empty() && folder_name.back() == '_') {
        folder_name.pop_back();
    }
    return folder_name;
}

// Whether the folder's field takes this character: a letter, a digit or an underscore. So a typed
// folder's name is one folder's and nothing else, without a space or a dot or a slash.
[[nodiscard]] inline bool is_folder_name_character(unsigned int character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_';
}

[[nodiscard]] inline bool is_typeable_folder_name(std::string_view folder_name) {
    return std::ranges::all_of(folder_name, [](char character) {
        return is_folder_name_character(static_cast<unsigned char>(character));
    });
}

struct NameAndFolder {
    std::string name;
    std::string folder_name;
};

// The two fields' texts as they are typed, in the form ImGui's inputs hold them.
class TypedNameAndFolder {
public:
    std::array<char, LONGEST_PIPELINE_NAME + 1> name{};
    std::array<char, LONGEST_PIPELINE_NAME + 1> folder_name{};
    // Whether the folder's name is the user's own. Until then it follows the pipeline's name.
    bool is_folder_name_own = false;
    // Whether the folder's name shows as a field, which the pencil beside it asks for; a line of
    // text otherwise.
    bool is_folder_name_edited = false;
    // For the one frame in which the folder's field, just shown, is to take the keys.
    bool is_folder_name_to_focus = false;
    // What `is_folder_name_own` was when the folder's field was shown, which Escape puts back.
    bool was_folder_name_own_before_edit = false;

    void clear() {
        name.fill('\0');
        folder_name.fill('\0');
        is_folder_name_own = false;
        is_folder_name_edited = false;
        is_folder_name_to_focus = false;
        was_folder_name_own_before_edit = false;
    }

    // Whether there is anything to show a folder for: a name was typed, or a folder was.
    [[nodiscard]] bool is_touched() const {
        return name.front() != '\0' || is_folder_name_own || is_folder_name_edited;
    }

    void start_editing_folder_name() {
        follow_name();
        was_folder_name_own_before_edit = is_folder_name_own;
        is_folder_name_edited = true;
        is_folder_name_to_focus = true;
    }

    // Starts the fields with a pipeline's name and folder as they are. A folder that isn't the
    // one its name gives counts as the user's own, and stays while the name is typed anew.
    void start_with(const NameAndFolder& current) {
        clear();
        copy_into(name, current.name);
        copy_into(folder_name, current.folder_name);
        is_folder_name_own = current.folder_name != folder_name_for(current.name);
    }

    // What is typed: the name without the spaces around it, and the folder's name, which is the
    // one the name gives until the user types their own.
    [[nodiscard]] NameAndFolder typed() const {
        std::string typed_name(without_spaces_around(std::string_view(name.data())));
        std::string typed_folder_name =
            is_folder_name_own ? std::string(folder_name.data()) : folder_name_for(typed_name);
        return {.name = std::move(typed_name), .folder_name = std::move(typed_folder_name)};
    }

    // Has the folder's field show the name's folder, unless the user typed their own.
    void follow_name() {
        if (!is_folder_name_own) {
            copy_into(folder_name, typed().folder_name);
        }
    }

private:
    // Copies as much of the text as the field holds, never ending inside a character of several
    // bytes.
    static void copy_into(std::array<char, LONGEST_PIPELINE_NAME + 1>& field,
                          std::string_view text) {
        constexpr unsigned char marker_bits = 0xC0U;
        constexpr unsigned char following_byte_marker = 0x80U;
        std::size_t size = std::min(text.size(), LONGEST_PIPELINE_NAME);
        while (size > 0 && size < text.size() &&
               (static_cast<unsigned char>(text.at(size)) & marker_bits) == following_byte_marker) {
            --size;
        }
        field.fill('\0');
        std::ranges::copy(text.substr(0, size), field.begin());
    }
};

struct NameFieldLook {
    float width = 0.0f;
    const char* hint = "";
    // What a test that drives the UI calls the field, after "explorer".
    std::string_view target;
    // For the one frame in which the field is to take the keys, with its text selected.
    bool is_to_focus = false;
};

// Draws the field for a pipeline's name. Returns whether Enter was pressed in it.
[[nodiscard]] inline bool render_name_field(TypedNameAndFolder& typed, const NameFieldLook& look) {
    if (look.is_to_focus) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(look.width);
    const bool is_entered =
        ImGui::InputTextWithHint("##name", look.hint, typed.name.data(), typed.name.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue);
    draw_last_item_as_sunken();
    name_last_item_for_ui_driver({"explorer", look.target});
    typed.follow_name();
    return is_entered;
}

// What a test that drives the UI calls the parts of the line under the name, after "explorer".
struct FolderLineTargets {
    std::string_view label;
    std::string_view pencil;
    std::string_view field;
};

struct FolderLineOutcome {
    // Whether Enter was pressed in the folder's field.
    bool is_entered = false;
    // Whether Escape was pressed in the folder's field, which it then closed and nothing else.
    bool has_taken_escape = false;
};

// Draws the line under the name's field that says where the pipeline lies: the directory that
// holds every pipeline's folder, the folder's name, and a pencil. A click on the pencil turns the
// folder's name into a field, which becomes a line of text again once it is left.
[[nodiscard]] inline FolderLineOutcome render_folder_line(TypedNameAndFolder& typed,
                                                          const FolderLineTargets& targets) {
    constexpr const char* directory_text = "~/falcon/";
    constexpr float gap_after_directory = 2.0f;
    constexpr float field_width = 280.0f;
    constexpr ImVec4 no_colour{0.0f, 0.0f, 0.0f, 0.0f};
    FolderLineOutcome outcome;

    // As tall with a text as with a field, so that nothing under the line moves when the pencil
    // is clicked.
    ImGui::AlignTextToFramePadding();
    if (!typed.is_folder_name_edited) {
        const std::string path = std::format("{}{}", directory_text, typed.typed().folder_name);
        ImGui::TextColored(FAINT_TEXT_COLOUR, "%s", path.c_str());
        name_last_item_for_ui_driver({"explorer", targets.label}, path);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, no_colour);
        ImGui::PushStyleColor(ImGuiCol_Text, FAINT_TEXT_COLOUR);
        const bool is_pencil_clicked = ImGui::SmallButton(icon("pencil-line"));
        ImGui::PopStyleColor(2);
        name_last_item_for_ui_driver({"explorer", targets.pencil});
        render_item_tooltip("Change the folder");
        if (is_pencil_clicked) {
            typed.start_editing_folder_name();
        }
        return outcome;
    }

    ImGui::TextColored(FAINT_TEXT_COLOUR, "%s", directory_text);
    ImGui::SameLine(0.0f, gap_after_directory);
    if (typed.is_folder_name_to_focus) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(std::min(field_width, ImGui::GetContentRegionAvail().x));
    const auto take_folder_characters_only = [](ImGuiInputTextCallbackData* data) -> int {
        return is_folder_name_character(data->EventChar) ? 0 : 1;
    };
    // ImGui's flags are signed ints by its API
    // NOLINTNEXTLINE(bugprone-signed-bitwise)
    outcome.is_entered = ImGui::InputTextWithHint(
        "##folder", "folder", typed.folder_name.data(), typed.folder_name.size(),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter,
        take_folder_characters_only);
    draw_last_item_as_sunken();
    // An emptied field follows the name again.
    if (ImGui::IsItemEdited()) {
        typed.is_folder_name_own = typed.folder_name.front() != '\0';
    }
    name_last_item_for_ui_driver({"explorer", targets.field});
    // The field takes the keys a frame after it asked for them, so it isn't left in that one.
    if (ImGui::IsItemDeactivated() && !typed.is_folder_name_to_focus) {
        typed.is_folder_name_edited = false;
        // Escape puts the field's text back as it was, and with it whether that was the user's.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            outcome.has_taken_escape = true;
            typed.is_folder_name_own = typed.was_folder_name_own_before_edit;
            typed.follow_name();
        }
    }
    typed.is_folder_name_to_focus = false;
    return outcome;
}
