#pragma once
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <initializer_list>
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
#include "../pipeline_designer/pipeline_writer.cpp"
#include "../pipeline_designer/tooltip.cpp"
#include "../ui_driver/ui_driver_targets.cpp"
#include "found_pipelines.cpp"
#include "name_and_folder_fields.cpp"
#include "pipeline_folders.cpp"

// The welcome screen: the pipelines that Falcon keeps, each opened in the designer by a click,
// and a new one made from a name. Falcon starts on it, and the user
// comes back to it to manage pipelines. A pipeline is deleted by moving it into the trash, which
// this screen shows too: from there it is brought back or deleted for good.
class PipelineExplorer {
public:
    // Has the pipelines looked for again, beside this thread.
    void refresh() { is_refresh_wanted_ = true; }

    void render() {
        take_what_ended();
        const ImVec2 space = ImGui::GetContentRegionAvail();
        const ImVec2 top_left = ImGui::GetCursorPos();
        const float column_width = std::min(widest_column, space.x - (2.0f * side_margin));
        const bool is_trash_button_shown = !is_trash_shown_ && !found_.trashed.empty();
        // The column ends above the trash button, so that no card lies under it in a window too
        // narrow for the button to stand beside the column.
        const float height_kept_for_trash_button =
            is_trash_button_shown ? ImGui::GetFrameHeight() + gap_between_parts + side_margin
                                  : 0.0f;
        ImGui::PushStyleColor(ImGuiCol_FrameBg, INPUT_COLOUR);
        ImGui::PushStyleColor(ImGuiCol_Button, INPUT_COLOUR);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, HOVERED_INPUT_COLOUR);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ACTIVE_INPUT_COLOUR);
        ImGui::SetCursorPos(ImVec2((space.x - column_width) / 2.0f, top_margin));
        ImGui::BeginChild("Explorer",
                          ImVec2(column_width, space.y - top_margin - height_kept_for_trash_button),
                          ImGuiChildFlags_None);
        if (is_trash_shown_) {
            render_trash();
        } else {
            render_pipelines();
        }
        ImGui::EndChild();
        if (is_trash_button_shown) {
            render_trash_button(top_left, space);
        }
        ImGui::PopStyleColor(4);
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
    // Stands between the facts of one line of a card: a dot at mid height, with room around it.
    static constexpr const char* facts_separator = "  \u00B7  ";
    static constexpr int title_size = 32;
    static constexpr int text_size = 16;

    PipelineSearch search_;
    PipelineFolderWork folder_work_;
    FoundPipelines found_;
    bool has_searched_ = false;
    bool is_search_running_ = false;
    bool is_refresh_wanted_ = false;
    bool is_trash_shown_ = false;
    // What is being typed for a new pipeline.
    TypedNameAndFolder new_pipeline_;
    std::string new_pipeline_problem_;
    // The pipeline whose card shows the two fields that rename it, by its folder's name; empty
    // for none.
    std::string asked_to_rename_;
    TypedNameAndFolder renamed_;
    std::string rename_problem_;
    // Set when a card starts to rename, for the one frame in which its name's field takes the
    // keys.
    bool is_renamed_name_to_focus_ = false;
    // Why the last move into the trash or out of it, delete, copy or rename couldn't be done;
    // empty if it could.
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
        if (!is_refresh_wanted_ || folder_work_.is_busy()) {
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
               !folder_work_.is_busy();
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
        close_rename();
        folder_work_.start([directory = *directory, work = std::move(work_in_directory)] {
            return work(directory);
        });
    }

    void close_rename() {
        asked_to_rename_.clear();
        rename_problem_.clear();
        is_renamed_name_to_focus_ = false;
    }

    // Hands a pipeline over to be opened in the designer, which this screen then gives way to.
    void open_in_designer(OpenedPipeline pipeline) {
        opened_ = std::move(pipeline);
        close_rename();
    }

    // Why a pipeline can't be given this name and this folder, or nothing if it can.
    // `own_folder_name` is the folder it lies in, which it may keep whatever that is called;
    // empty for a pipeline that isn't there yet.
    [[nodiscard]] std::string problem_of(const NameAndFolder& wanted,
                                         std::string_view own_folder_name) const {
        if (wanted.name.empty()) {
            return "A pipeline needs a name.";
        }
        if (wanted.folder_name.empty()) {
            return "A pipeline needs a folder.";
        }
        if (wanted.folder_name == own_folder_name) {
            return {};
        }
        if (!is_typeable_folder_name(wanted.folder_name)) {
            return "A folder's name takes letters, digits and _ only.";
        }
        if (std::ranges::contains(found_.folder_names, wanted.folder_name)) {
            return std::format("The folder {} is there already.", wanted.folder_name);
        }
        return {};
    }

    // Makes the pipeline that the two fields ask for and opens it, or says why it can't be made.
    // Its folder and its pipeline.cpp are written by the designer, with its first frame.
    void create_pipeline() {
        const NameAndFolder wanted = new_pipeline_.typed();
        const std::optional<std::filesystem::path> directory = pipelines_directory();
        if (!directory.has_value()) {
            new_pipeline_problem_ = "No home directory is known.";
            return;
        }
        new_pipeline_problem_ = problem_of(wanted, {});
        if (!new_pipeline_problem_.empty()) {
            return;
        }
        OpenedPipeline pipeline;
        pipeline.directory = *directory / wanted.folder_name;
        pipeline.graph.name = wanted.name;
        pipeline.is_new = true;
        open_in_designer(std::move(pipeline));
        new_pipeline_.clear();
    }

    // The pipeline.cpp of a pipeline under another name, or why there is none. It is read back,
    // so that no text is handed out that couldn't be opened.
    [[nodiscard]] static std::expected<std::string, std::string> text_under_name(
        const FoundPipeline& found, const std::string& name) {
        PipelineGraph graph = found.pipeline.graph;
        graph.name = name;
        WrittenText written = write_pipeline_to_keep(graph);
        if (!written.has_value()) {
            return std::unexpected(sentence_of(written.error()));
        }
        const ReadPipeline read_back = read_pipeline(*written, ALL_PROCESSORS);
        if (!read_back.has_value()) {
            return std::unexpected(read_back.error());
        }
        return std::move(*written);
    }

    // A name for a copy of the pipeline called `name`: that name and "copy", and where the folder
    // of such a name is there already, the first number from 2 on whose folder isn't.
    [[nodiscard]] std::string name_for_copy_of(std::string_view name) const {
        constexpr int highest_number = 99;
        std::string copy_name = std::format("{} copy", name);
        for (int number = 2; number <= highest_number; ++number) {
            if (!std::ranges::contains(found_.folder_names, folder_name_for(copy_name))) {
                break;
            }
            copy_name = std::format("{} copy {}", name, number);
        }
        return copy_name;
    }

    // A pipeline.cpp without the line of its header that says the pipeline's name.
    [[nodiscard]] static std::string without_name_line(std::string_view pipeline_text) {
        const std::string line_start = std::format("// {}: ", PIPELINE_HEADER_NAME_KEY);
        std::size_t line_at = 0;
        if (!pipeline_text.starts_with(line_start)) {
            const std::size_t before_line = pipeline_text.find(std::format("\n{}", line_start));
            if (before_line == std::string_view::npos) {
                return std::string(pipeline_text);
            }
            line_at = before_line + 1;
        }
        std::string without(pipeline_text.substr(0, line_at));
        const std::size_t line_end = pipeline_text.find('\n', line_at);
        if (line_end != std::string_view::npos) {
            without += pipeline_text.substr(line_end + 1);
        }
        return without;
    }

    // The text that marks a pipeline's executable as built from the pipeline under another name,
    // or none where the executable isn't to be marked so. It is, where it was built from the
    // pipeline as it stands and the new name changes nothing of the compiled text but the
    // header's line: a name isn't compiled, so the executable is the renamed pipeline's too.
    [[nodiscard]] static std::optional<std::string> compiled_text_under_name(
        const FoundPipeline& found, const std::string& name) {
        if (found.pipeline.compiled_text.empty()) {
            return std::nullopt;
        }
        const WrittenText current = write_pipeline(found.pipeline.graph);
        if (!current.has_value() || *current != found.pipeline.compiled_text) {
            return std::nullopt;
        }
        PipelineGraph graph = found.pipeline.graph;
        graph.name = name;
        WrittenText renamed = write_pipeline(graph);
        if (!renamed.has_value() || without_name_line(*renamed) != without_name_line(*current)) {
            return std::nullopt;
        }
        return std::move(*renamed);
    }

    // Makes a copy of a pipeline: a new pipeline that holds the same graph under a name of its
    // own, without the first one's runs or its executable. Its file is written beside this
    // thread, and the list shows it once it is there; it is opened by a click, as any other.
    void duplicate_pipeline(const FoundPipeline& found) {
        const std::string name = name_for_copy_of(shown_name_of(found));
        const NameAndFolder copy{.name = name, .folder_name = folder_name_for(name)};
        folder_problem_ = problem_of(copy, {});
        if (!folder_problem_.empty()) {
            return;
        }
        std::expected<std::string, std::string> text = text_under_name(found, copy.name);
        if (!text.has_value()) {
            folder_problem_ =
                std::format("{} can't be copied: {}", found.folder_name, text.error());
            return;
        }
        start_folder_work([folder_name = copy.folder_name, pipeline_text = std::move(*text)](
                              const std::filesystem::path& directory) {
            return write_new_pipeline(directory, folder_name, pipeline_text);
        });
    }

    // What a pipeline is called on its card: its name, or its folder's where it has none.
    [[nodiscard]] static const std::string& shown_name_of(const FoundPipeline& found) {
        return found.name.empty() ? found.folder_name : found.name;
    }

    // Has the pipeline's card show the two fields that rename it, with its name and its folder
    // as they are.
    void start_rename(const FoundPipeline& found) {
        renamed_.start_with({.name = shown_name_of(found), .folder_name = found.folder_name});
        asked_to_rename_ = found.folder_name;
        rename_problem_.clear();
        is_renamed_name_to_focus_ = true;
    }

    // Renames the pipeline as the two fields ask, beside this thread, or says why it can't be: its
    // name goes into its pipeline.cpp, and another folder's name moves its folder with all it
    // holds.
    void apply_rename(const FoundPipeline& found) {
        const NameAndFolder wanted = renamed_.typed();
        rename_problem_ = problem_of(wanted, found.folder_name);
        if (!rename_problem_.empty()) {
            return;
        }
        PipelineRename rename{
            .folder_name = found.folder_name,
            .new_folder_name = wanted.folder_name,
            .new_pipeline_text = std::nullopt,
            .new_compiled_text = std::nullopt,
        };
        if (wanted.name != shown_name_of(found)) {
            std::expected<std::string, std::string> text = text_under_name(found, wanted.name);
            if (!text.has_value()) {
                rename_problem_ = std::format("It can't be renamed: {}", text.error());
                return;
            }
            rename.new_pipeline_text = std::move(*text);
            rename.new_compiled_text = compiled_text_under_name(found, wanted.name);
        }
        if (!rename.new_pipeline_text.has_value() && rename.new_folder_name == rename.folder_name) {
            close_rename();
            return;
        }
        start_folder_work([rename = std::move(rename)](const std::filesystem::path& directory) {
            return rename_pipeline(directory, rename);
        });
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

    // The way into the trash, in the bottom right corner of the screen, whose top left corner and
    // size are given.
    void render_trash_button(ImVec2 screen_top_left, ImVec2 screen_size) {
        const std::string label =
            std::format("{} Trash ({})", icon("delete-bin-line"), found_.trashed.size());
        ImGui::SetCursorPos(
            ImVec2(screen_top_left.x + screen_size.x - side_margin - button_width_of(label.c_str()),
                   screen_top_left.y + screen_size.y - side_margin - ImGui::GetFrameHeight()));
        if (ImGui::Button(label.c_str())) {
            is_trash_shown_ = true;
            folder_problem_.clear();
            close_rename();
        }
        name_last_item_for_ui_driver({"explorer", "open_trash"});
        render_item_tooltip("Pipelines that were deleted.");
    }

    // The welcome screen's first view: a new pipeline's name and the pipelines.
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
    }

    // A new pipeline's name in a field, the button that makes it, and under them, once a name
    // is typed, the line that says where it will lie.
    void render_new_pipeline() {
        const char* const create_label = "Create";
        // Until the folders are known, a new pipeline's folder can't be told from one of them.
        ImGui::BeginDisabled(!is_list_current());
        ImGui::PushID("new_pipeline");
        bool is_entered =
            render_name_field(new_pipeline_, {
                                                 .width = ImGui::GetContentRegionAvail().x -
                                                          button_width_of(create_label) -
                                                          ImGui::GetStyle().ItemSpacing.x,
                                                 .hint = "Name of a new pipeline",
                                                 .target = "new_name",
                                                 .is_to_focus = false,
                                             });
        ImGui::SameLine();
        const bool is_clicked = ImGui::Button(create_label);
        name_last_item_for_ui_driver({"explorer", "create"});
        if (new_pipeline_.is_touched()) {
            const FolderLineOutcome folder_line = render_folder_line(
                new_pipeline_,
                {.label = "new_folder_label", .pencil = "new_folder_edit", .field = "new_folder"});
            is_entered = is_entered || folder_line.is_entered;
        } else {
            // The line's room is kept while it doesn't show, so that the list under it stays
            // where it is when the first letter of a name is typed.
            ImGui::Dummy(ImVec2(0.0f, ImGui::GetFrameHeight()));
        }
        ImGui::PopID();
        ImGui::EndDisabled();
        if (is_entered || is_clicked) {
            create_pipeline();
        }
        if (!new_pipeline_problem_.empty()) {
            ImGui::TextColored(FAILURE_COLOUR, "%s", new_pipeline_problem_.c_str());
        }
    }

    struct CardIconLook {
        const char* icon_text;
        const char* tooltip;
    };

    struct CardIcon {
        bool is_clicked = false;
        bool is_under_mouse = false;
    };

    // An icon of a card that is clicked, drawn faint and without a frame, with a tooltip.
    [[nodiscard]] CardIcon render_card_icon(
        const CardIconLook& look,
        std::initializer_list<std::string_view> name_for_ui_driver) const {
        ImGui::PushStyleColor(ImGuiCol_Button, no_colour);
        ImGui::PushStyleColor(ImGuiCol_Text, FAINT_TEXT_COLOUR);
        ImGui::BeginDisabled(!is_list_current());
        CardIcon card_icon;
        card_icon.is_clicked = ImGui::SmallButton(look.icon_text);
        ImGui::EndDisabled();
        ImGui::PopStyleColor(2);
        card_icon.is_under_mouse =
            ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        name_last_item_for_ui_driver(name_for_ui_driver);
        render_item_tooltip(look.tooltip);
        return card_icon;
    }

    // What a card shows in place of its name, folder and icons while its pipeline is renamed: the
    // name in a field with Rename and Cancel, and under it the line that says where the pipeline
    // lies. Enter in a field renames too. Escape cancels, unless the folder's field took it to
    // close itself. Returns whether the pipeline is to be renamed.
    [[nodiscard]] bool render_rename_fields() {
        const char* const rename_label = "Rename";
        const char* const cancel_label = "Cancel";
        const bool is_name_entered = render_name_field(
            renamed_,
            {
                .width = ImGui::GetContentRegionAvail().x - button_width_of(rename_label) -
                         button_width_of(cancel_label) - (2.0f * ImGui::GetStyle().ItemSpacing.x),
                .hint = "Name",
                .target = "rename_name",
                .is_to_focus = is_renamed_name_to_focus_,
            });
        is_renamed_name_to_focus_ = false;
        ImGui::SameLine();
        ImGui::BeginDisabled(!is_list_current());
        const bool is_rename_clicked = ImGui::Button(rename_label);
        name_last_item_for_ui_driver({"explorer", "rename_apply"});
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool is_cancel_clicked = ImGui::Button(cancel_label);
        name_last_item_for_ui_driver({"explorer", "rename_cancel"});
        const FolderLineOutcome folder_line =
            render_folder_line(renamed_, {
                                             .label = "rename_folder_label",
                                             .pencil = "rename_folder_edit",
                                             .field = "rename_folder",
                                         });
        const bool is_escape_for_the_rename =
            ImGui::IsKeyPressed(ImGuiKey_Escape) && !folder_line.has_taken_escape;
        if (is_cancel_clicked || is_escape_for_the_rename) {
            close_rename();
            return false;
        }
        return (is_name_entered || folder_line.is_entered || is_rename_clicked) &&
               is_list_current();
    }

    // What the user asked of a card's pipeline by its icons in this frame.
    struct CardIcons {
        CardIcon rename;
        CardIcon duplicate;
        CardIcon trash;

        [[nodiscard]] bool is_one_under_mouse() const {
            return rename.is_under_mouse || duplicate.is_under_mouse || trash.is_under_mouse;
        }
    };

    // The icons at a card's right end: they rename the pipeline, duplicate it and move it into
    // the trash. A pipeline that can't be read has the last one alone.
    [[nodiscard]] CardIcons render_card_icons(const FoundPipeline& found) const {
        const char* const rename_icon = icon("pencil-line");
        const char* const duplicate_icon = icon("file-copy-line");
        const char* const trash_icon = icon("delete-bin-line");
        CardIcons icons;
        if (found.problem.empty()) {
            move_to_right_end(button_width_of(rename_icon) + button_width_of(duplicate_icon) +
                              button_width_of(trash_icon) +
                              (2.0f * ImGui::GetStyle().ItemSpacing.x));
            icons.rename = render_card_icon({.icon_text = rename_icon, .tooltip = "Rename"},
                                            {"explorer", "rename", found.folder_name});
            ImGui::SameLine();
            icons.duplicate =
                render_card_icon({.icon_text = duplicate_icon, .tooltip = "Duplicate"},
                                 {"explorer", "duplicate", found.folder_name});
            ImGui::SameLine();
        } else {
            move_to_right_end(button_width_of(trash_icon));
        }
        icons.trash = render_card_icon({.icon_text = trash_icon, .tooltip = "Move to trash"},
                                       {"explorer", "move_to_trash", found.folder_name});
        return icons;
    }

    // The line under a card's name: where the pipeline lies, and when it was last changed.
    static void render_where_and_when(const FoundPipeline& found) {
        const std::string path = std::format("{}{}", PIPELINES_DIRECTORY_TEXT, found.folder_name);
        ImGui::TextColored(FAINT_TEXT_COLOUR, "%s", path.c_str());
        name_last_item_for_ui_driver({"explorer", "pipeline_path", found.folder_name}, path);
        if (found.modified_at.empty()) {
            return;
        }
        const std::string modified = std::format("Modified {}", found.modified_at);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(FAINT_TEXT_COLOUR, "%s%s", facts_separator, modified.c_str());
        name_last_item_for_ui_driver({"explorer", "pipeline_modified", found.folder_name},
                                     modified);
    }

    // One pipeline, in a card of its own: its name, its folder and its description, and for one
    // that can't be read why. A click on a card that can be read opens its pipeline, unless the
    // card shows the fields that rename it. Returns whether the mouse is over the card.
    [[nodiscard]] bool render_pipeline(const FoundPipeline& found) {
        const bool is_readable = found.problem.empty();
        const bool is_renamed = asked_to_rename_ == found.folder_name;
        begin_card(found.folder_name,
                   is_readable && !is_renamed && hovered_folder_ == found.folder_name);
        CardIcons icons;
        bool is_rename_wanted = false;
        if (is_renamed) {
            is_rename_wanted = render_rename_fields();
            if (!rename_problem_.empty()) {
                ImGui::TextColored(FAILURE_COLOUR, "%s", rename_problem_.c_str());
            }
        } else {
            render_bold(shown_name_of(found).c_str());
            icons = render_card_icons(found);
            render_where_and_when(found);
        }
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
        // A card that is renamed takes its clicks for its fields, never to open its pipeline.
        const bool is_opened_by_a_click = is_readable && !is_renamed && is_hovered &&
                                          !icons.is_one_under_mouse() && is_list_current();
        if (icons.trash.is_clicked) {
            start_folder_work(
                [folder_name = found.folder_name](const std::filesystem::path& directory) {
                    return move_pipeline_to_trash(directory, folder_name);
                });
        } else if (icons.duplicate.is_clicked) {
            duplicate_pipeline(found);
        } else if (icons.rename.is_clicked) {
            start_rename(found);
        } else if (is_rename_wanted) {
            apply_rename(found);
        } else if (is_opened_by_a_click) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                open_in_designer(found.pipeline);
            }
        }
        ImGui::Dummy(ImVec2(0.0f, gap_between_cards));
        return is_hovered && !is_renamed && !icons.is_one_under_mouse();
    }

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
        // Where it lay, and where Restore puts it back; when it was deleted; and what goes with
        // it when it is deleted for good.
        std::string facts = std::format("{}{}", PIPELINES_DIRECTORY_TEXT, trashed.folder_name);
        if (!trashed.deleted_at.empty()) {
            facts += std::format("{}Deleted {}", facts_separator, trashed.deleted_at);
        }
        facts += std::format("{}{}", facts_separator, runs_text);
        ImGui::TextColored(FAINT_TEXT_COLOUR, "%s", facts.c_str());
        name_last_item_for_ui_driver({"trash", "facts", trashed.trash_name}, facts);
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
            pipelines.push_back(std::format(
                R"({{"folder":{},"name":{},"description":{},"problem":{},"modified":{}}})",
                json_text(pipeline.folder_name), json_text(pipeline.name),
                json_text(pipeline.description), json_text(pipeline.problem),
                json_text(pipeline.modified_at)));
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
            R"("new_pipeline_problem":{},"new_folder":{},"is_trash_shown":{},"trash":{},)"
            R"("folder_problem":{},"asked_to_rename":{},"rename_name":{},"rename_folder":{},)"
            R"("rename_problem":{}}})",
            has_searched_ ? "true" : "false", is_list_current() ? "true" : "false",
            json_array(pipelines), json_text(new_pipeline_problem_),
            json_text(new_pipeline_.typed().folder_name), is_trash_shown_ ? "true" : "false",
            json_array(trashed_pipelines), json_text(folder_problem_), json_text(asked_to_rename_),
            json_text(renamed_.typed().name), json_text(renamed_.typed().folder_name),
            json_text(rename_problem_)));
    }
};
