#pragma once
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "../common/colours.cpp"
#include "../common/fonts.cpp"
#include "../common/icons.cpp"
#include "../pipeline_designer/designer_state_json.cpp"
#include "../pipeline_designer/opened_pipeline.cpp"
#include "../pipeline_designer/pipeline_reader.cpp"
#include "../pipeline_designer/tooltip.cpp"
#include "../ui_driver/ui_driver_targets.cpp"
#include "found_pipelines.cpp"
#include "pipeline_folders.cpp"

// The welcome screen: the pipelines that Falcon keeps, each opened in the designer by a click,
// and a new one made from a name. Falcon starts on it, and the user comes back to it to manage
// pipelines. A pipeline is deleted by moving it into the trash, which this screen shows too: from
// there it is brought back or deleted for good.
class PipelineExplorer {
public:
    // Has the pipelines looked for again, beside this thread.
    void refresh() { is_refresh_wanted_ = true; }

    void render() {
        take_what_ended();
        const ImVec2 space = ImGui::GetContentRegionAvail();
        const float column_width = std::min(widest_column, space.x - (2.0f * side_margin));
        ImGui::SetCursorPos(ImVec2((space.x - column_width) / 2.0f, top_margin));
        ImGui::BeginChild("Explorer", ImVec2(column_width, space.y - top_margin),
                          ImGuiChildFlags_None);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, INPUT_COLOUR);
        ImGui::PushStyleColor(ImGuiCol_Button, INPUT_COLOUR);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, HOVERED_INPUT_COLOUR);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ACTIVE_INPUT_COLOUR);
        if (is_trash_shown_) {
            render_trash();
        } else {
            render_pipelines();
        }
        ImGui::PopStyleColor(4);
        ImGui::EndChild();
        hand_state_to_ui_driver();
    }

    // The pipeline that the user opened or made since the last call, or none.
    [[nodiscard]] std::optional<OpenedPipeline> take_opened() {
        return std::exchange(opened_, std::nullopt);
    }

private:
    static constexpr float widest_column = 720.0f;
    static constexpr float side_margin = 24.0f;
    static constexpr float top_margin = 64.0f;
    static constexpr float gap_between_parts = 12.0f;
    static constexpr float gap_between_cards = 2.0f;
    static constexpr ImVec2 card_padding{14.0f, 10.0f};
    static constexpr ImVec4 no_colour{0.0f, 0.0f, 0.0f, 0.0f};
    static constexpr int title_size = 32;
    static constexpr int text_size = 16;
    static constexpr std::size_t longest_name = 120;

    PipelineSearch search_;
    PipelineFolderWork folder_work_;
    FoundPipelines found_;
    bool has_searched_ = false;
    bool is_search_running_ = false;
    bool is_refresh_wanted_ = false;
    bool is_trash_shown_ = false;
    // The name that is being typed for a new pipeline, as ImGui's input holds it.
    std::array<char, longest_name + 1> new_name_{};
    std::string new_pipeline_problem_;
    // Why the last move into the trash, out of it, or delete couldn't be done; empty if it could.
    std::string folder_problem_;
    // The pipeline of the trash that the user asked to delete for good and hasn't confirmed yet,
    // by its name in the trash; empty for none.
    std::string asked_to_delete_;
    // The card under the mouse in the frame before, which is drawn as such.
    std::string hovered_folder_;
    std::optional<OpenedPipeline> opened_;

    // Takes what the threads beside this one have ended, and starts the search that is wanted.
    void take_what_ended() {
        if (std::optional<std::string> problem = folder_work_.take_outcome()) {
            folder_problem_ = std::move(*problem);
            is_refresh_wanted_ = true;
        }
        if (std::optional<FoundPipelines> found = search_.take_result()) {
            found_ = std::move(*found);
            has_searched_ = true;
            is_search_running_ = false;
        }
        if (!is_refresh_wanted_ || folder_work_.is_running()) {
            return;
        }
        const std::optional<std::filesystem::path> directory = pipelines_directory();
        if (!directory.has_value()) {
            has_searched_ = true;
            is_refresh_wanted_ = false;
        } else if (search_.start(*directory)) {
            is_search_running_ = true;
            is_refresh_wanted_ = false;
        }
    }

    // Whether the list shows the folders as they are: nothing is being moved or deleted, and no
    // search is on or wanted. Only then is a pipeline opened, made, moved or deleted.
    [[nodiscard]] bool is_list_current() const {
        return has_searched_ && !is_search_running_ && !is_refresh_wanted_ &&
               !folder_work_.is_running();
    }

    // Starts work on a folder of the pipelines' directory, beside this thread.
    void start_folder_work(
        std::function<std::string(const std::filesystem::path&)> work_in_directory) {
        const std::optional<std::filesystem::path> directory = pipelines_directory();
        if (!directory.has_value()) {
            folder_problem_ = "No home directory is known.";
            return;
        }
        folder_problem_.clear();
        asked_to_delete_.clear();
        folder_work_.start([directory = *directory, work = std::move(work_in_directory)] {
            return work(directory);
        });
    }

    // A folder's name for a pipeline's name: its letters and digits in lower case, with one
    // underscore for whatever lies between them. Empty for a name that has neither.
    [[nodiscard]] static std::string folder_name_for(std::string_view pipeline_name) {
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

    // Makes the pipeline that the typed name asks for and opens it, or says why it can't be made.
    // Its folder and its pipeline.cpp are written by the designer, with its first frame.
    void create_pipeline() {
        const std::string name(without_spaces_around(std::string_view(new_name_.data())));
        const std::optional<std::filesystem::path> directory = pipelines_directory();
        if (!directory.has_value()) {
            new_pipeline_problem_ = "No home directory is known.";
            return;
        }
        if (name.empty()) {
            new_pipeline_problem_ = "A pipeline needs a name.";
            return;
        }
        const std::string folder_name = folder_name_for(name);
        if (folder_name.empty()) {
            new_pipeline_problem_ = "A pipeline's name needs a letter or a digit.";
            return;
        }
        if (std::ranges::contains(found_.folder_names, folder_name)) {
            new_pipeline_problem_ = std::format("The folder {} is there already.", folder_name);
            return;
        }
        OpenedPipeline pipeline;
        pipeline.directory = *directory / folder_name;
        pipeline.graph.name = name;
        pipeline.is_new = true;
        opened_ = std::move(pipeline);
        new_name_.fill('\0');
        new_pipeline_problem_.clear();
    }

    static void render_title(const char* title) {
        push_ui_font(title_size, FontStyle::Bold);
        ImGui::TextUnformatted(title);
        pop_ui_font();
    }

    static void render_bold(const char* text) {
        push_ui_font(text_size, FontStyle::Bold);
        ImGui::TextUnformatted(text);
        pop_ui_font();
    }

    // How wide a button with this label is drawn.
    [[nodiscard]] static float button_width_of(const char* label) {
        return ImGui::CalcTextSize(label).x + (2.0f * ImGui::GetStyle().FramePadding.x);
    }

    // Puts what is drawn next at the right end of the line, this wide.
    static void move_to_right_end(float width) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width);
    }

    // The frame of a card, which whatever is drawn until end_card() lies in.
    static void begin_card(const std::string& card_id, bool is_hovered) {
        ImGui::PushID(card_id.c_str());
        ImGui::PushStyleColor(ImGuiCol_ChildBg, is_hovered ? HOVERED_CARD_COLOUR : CARD_COLOUR);
        ImGui::PushStyleColor(ImGuiCol_Border, CARD_BORDER_COLOUR);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, CARD_ROUNDING);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, card_padding);
        // ImGui's flags are signed ints by its API
        // NOLINTBEGIN(bugprone-signed-bitwise)
        const ImGuiChildFlags card_flags = ImGuiChildFlags_Borders |
                                           ImGuiChildFlags_AlwaysUseWindowPadding |
                                           ImGuiChildFlags_AutoResizeY;
        // NOLINTEND(bugprone-signed-bitwise)
        ImGui::BeginChild("Card", ImVec2(0.0f, 0.0f), card_flags);
    }

    static void end_card() {
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        ImGui::PopID();
    }

    void render_problem_of_folders() const {
        if (!folder_problem_.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(FAILURE_COLOUR, "%s", folder_problem_.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    // The welcome screen's first view: a new pipeline's name, the pipelines, and the way into
    // the trash.
    void render_pipelines() {
        render_title("Falcon");
        ImGui::TextColored(FAINT_TEXT_COLOUR, "Open a pipeline, or make a new one.");
        ImGui::Dummy(ImVec2(0.0f, gap_between_parts));
        render_new_pipeline();
        render_problem_of_folders();
        ImGui::Dummy(ImVec2(0.0f, gap_between_parts));
        if (!has_searched_) {
            ImGui::TextColored(FAINT_TEXT_COLOUR, "Looking for pipelines...");
        } else if (found_.pipelines.empty()) {
            ImGui::TextColored(FAINT_TEXT_COLOUR, "No pipelines yet.");
        }
        std::string hovered_folder;
        for (const FoundPipeline& pipeline : found_.pipelines) {
            if (render_pipeline(pipeline)) {
                hovered_folder = pipeline.folder_name;
            }
        }
        hovered_folder_ = std::move(hovered_folder);
        if (!found_.trashed.empty()) {
            ImGui::Dummy(ImVec2(0.0f, gap_between_parts));
            const std::string label =
                std::format("{} Trash ({})", icon("delete-bin-line"), found_.trashed.size());
            if (ImGui::Button(label.c_str())) {
                is_trash_shown_ = true;
                folder_problem_.clear();
            }
            name_last_item_for_ui_driver({"explorer", "open_trash"});
            render_item_tooltip("Pipelines that were deleted.");
        }
    }

    void render_new_pipeline() {
        const char* const create_label = "Create";
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - button_width_of(create_label) -
                                ImGui::GetStyle().ItemSpacing.x);
        // Until the folders are known, a new pipeline's folder can't be told from one of them.
        ImGui::BeginDisabled(!is_list_current());
        const bool is_entered =
            ImGui::InputTextWithHint("##new_name", "Name of a new pipeline", new_name_.data(),
                                     new_name_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        name_last_item_for_ui_driver({"explorer", "new_name"});
        ImGui::SameLine();
        const bool is_clicked = ImGui::Button(create_label);
        name_last_item_for_ui_driver({"explorer", "create"});
        ImGui::EndDisabled();
        if (is_entered || is_clicked) {
            create_pipeline();
        }
        if (!new_pipeline_problem_.empty()) {
            ImGui::TextColored(FAILURE_COLOUR, "%s", new_pipeline_problem_.c_str());
        }
    }

    // One pipeline, in a card of its own: its name, its folder and its description, and for one
    // that can't be read why. A click on a card that can be read opens its pipeline, and the
    // icon at its right end moves the pipeline into the trash. Returns whether the mouse is over
    // the card.
    [[nodiscard]] bool render_pipeline(const FoundPipeline& found) {
        const bool is_readable = found.problem.empty();
        begin_card(found.folder_name, is_readable && hovered_folder_ == found.folder_name);
        render_bold(found.name.empty() ? found.folder_name.c_str() : found.name.c_str());
        ImGui::SameLine();
        ImGui::TextColored(FAINT_TEXT_COLOUR, "%s", found.folder_name.c_str());
        const char* const trash_icon = icon("delete-bin-line");
        move_to_right_end(button_width_of(trash_icon));
        ImGui::PushStyleColor(ImGuiCol_Button, no_colour);
        ImGui::PushStyleColor(ImGuiCol_Text, FAINT_TEXT_COLOUR);
        ImGui::BeginDisabled(!is_list_current());
        const bool is_trash_clicked = ImGui::SmallButton(trash_icon);
        ImGui::EndDisabled();
        ImGui::PopStyleColor(2);
        const bool is_over_trash_icon =
            ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        name_last_item_for_ui_driver({"explorer", "move_to_trash", found.folder_name});
        render_item_tooltip("Move to trash");
        ImGui::PushTextWrapPos(0.0f);
        if (!found.description.empty()) {
            ImGui::TextColored(FAINT_TEXT_COLOUR, "%s", found.description.c_str());
        }
        if (!is_readable) {
            ImGui::TextColored(FAILURE_COLOUR, "Can't be opened: %s", found.problem.c_str());
        }
        ImGui::PopTextWrapPos();
        end_card();
        name_last_item_for_ui_driver({"explorer", "pipeline", found.folder_name}, found.problem);
        const bool is_hovered =
            ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (is_trash_clicked) {
            start_folder_work(
                [folder_name = found.folder_name](const std::filesystem::path& directory) {
                    return move_pipeline_to_trash(directory, folder_name);
                });
        } else if (is_readable && is_hovered && !is_over_trash_icon && is_list_current()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                opened_ = found.pipeline;
            }
        }
        ImGui::Dummy(ImVec2(0.0f, gap_between_cards));
        return is_hovered && !is_over_trash_icon;
    }

    // The welcome screen's second view: the pipelines that were deleted, each to be brought back
    // or deleted for good.
    void render_trash() {
        render_title("Trash");
        ImGui::TextColored(FAINT_TEXT_COLOUR,
                           "Deleted pipelines stay here until they are deleted for good.");
        ImGui::Dummy(ImVec2(0.0f, gap_between_parts));
        if (ImGui::Button("Pipelines")) {
            is_trash_shown_ = false;
            asked_to_delete_.clear();
            folder_problem_.clear();
        }
        name_last_item_for_ui_driver({"trash", "back"});
        render_item_tooltip("Back to the list of pipelines.");
        render_problem_of_folders();
        ImGui::Dummy(ImVec2(0.0f, gap_between_parts));
        if (found_.trashed.empty()) {
            ImGui::TextColored(FAINT_TEXT_COLOUR, "The trash is empty.");
        }
        for (const TrashedPipeline& trashed : found_.trashed) {
            render_trashed_pipeline(trashed);
        }
    }

    // One pipeline of the trash, in a card of its own, with a button that brings it back and one
    // that deletes it for good. The second only asks: the pipeline is deleted by the button that
    // then shows under what is lost with it.
    void render_trashed_pipeline(const TrashedPipeline& trashed) {
        begin_card(trashed.trash_name, false);
        render_bold(trashed.name.empty() ? trashed.folder_name.c_str() : trashed.name.c_str());
        ImGui::SameLine();
        ImGui::TextColored(FAINT_TEXT_COLOUR, "%s", trashed.folder_name.c_str());
        const char* const restore_label = "Restore";
        const char* const delete_label = "Delete for good";
        move_to_right_end(button_width_of(restore_label) + ImGui::GetStyle().ItemSpacing.x +
                          button_width_of(delete_label));
        ImGui::BeginDisabled(!is_list_current());
        const bool is_restore_clicked = ImGui::SmallButton(restore_label);
        name_last_item_for_ui_driver({"trash", "restore", trashed.trash_name});
        ImGui::SameLine();
        if (ImGui::SmallButton(delete_label)) {
            asked_to_delete_ = trashed.trash_name;
        }
        name_last_item_for_ui_driver({"trash", "delete", trashed.trash_name});
        ImGui::EndDisabled();
        const std::string runs_text = trashed.run_count == 1
                                          ? std::string("1 run")
                                          : std::format("{} runs", trashed.run_count);
        const std::string facts =
            trashed.deleted_at.empty()
                ? runs_text
                : std::format("Deleted {}, {}", trashed.deleted_at, runs_text);
        ImGui::TextColored(FAINT_TEXT_COLOUR, "%s", facts.c_str());
        if (!trashed.description.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(FAINT_TEXT_COLOUR, "%s", trashed.description.c_str());
            ImGui::PopTextWrapPos();
        }
        bool is_delete_confirmed = false;
        if (asked_to_delete_ == trashed.trash_name) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(FAILURE_COLOUR, "%s",
                               trashed.run_count == 0
                                   ? "This can't be undone."
                                   : "Its runs are deleted too. This can't be undone.");
            const char* const confirm_label = "Delete";
            const char* const keep_label = "Keep";
            move_to_right_end(button_width_of(confirm_label) + ImGui::GetStyle().ItemSpacing.x +
                              button_width_of(keep_label));
            ImGui::BeginDisabled(!is_list_current());
            is_delete_confirmed = ImGui::SmallButton(confirm_label);
            name_last_item_for_ui_driver({"trash", "confirm", trashed.trash_name});
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton(keep_label)) {
                asked_to_delete_.clear();
            }
            name_last_item_for_ui_driver({"trash", "keep", trashed.trash_name});
        }
        end_card();
        name_last_item_for_ui_driver({"trash", "pipeline", trashed.trash_name});
        ImGui::Dummy(ImVec2(0.0f, gap_between_cards));
        if (is_restore_clicked) {
            start_folder_work([trash_name = trashed.trash_name, folder_name = trashed.folder_name](
                                  const std::filesystem::path& directory) {
                return restore_pipeline(directory, trash_name, folder_name);
            });
        } else if (is_delete_confirmed) {
            start_folder_work(
                [trash_name = trashed.trash_name](const std::filesystem::path& directory) {
                    return delete_pipeline_for_good(directory, trash_name);
                });
        }
    }

    // For a test that drives the UI (see ../ui_driver/ui_driver.cpp); nothing unless one does.
    void hand_state_to_ui_driver() const {
        if (!ui_driver_targets().is_on()) {
            return;
        }
        std::vector<std::string> pipelines;
        pipelines.reserve(found_.pipelines.size());
        for (const FoundPipeline& pipeline : found_.pipelines) {
            pipelines.push_back(
                std::format(R"({{"folder":{},"name":{},"description":{},"problem":{}}})",
                            json_text(pipeline.folder_name), json_text(pipeline.name),
                            json_text(pipeline.description), json_text(pipeline.problem)));
        }
        std::vector<std::string> trashed_pipelines;
        trashed_pipelines.reserve(found_.trashed.size());
        for (const TrashedPipeline& trashed : found_.trashed) {
            trashed_pipelines.push_back(std::format(
                R"({{"trash_name":{},"folder":{},"name":{},"deleted_at":{},"runs":{}}})",
                json_text(trashed.trash_name), json_text(trashed.folder_name),
                json_text(trashed.name), json_text(trashed.deleted_at), trashed.run_count));
        }
        ui_driver_targets().set_state(std::format(
            R"({{"screen":"explorer","has_searched":{},"is_list_current":{},"pipelines":{},)"
            R"("new_pipeline_problem":{},"is_trash_shown":{},"trash":{},"folder_problem":{}}})",
            has_searched_ ? "true" : "false", is_list_current() ? "true" : "false",
            json_array(pipelines), json_text(new_pipeline_problem_),
            is_trash_shown_ ? "true" : "false", json_array(trashed_pipelines),
            json_text(folder_problem_)));
    }
};
