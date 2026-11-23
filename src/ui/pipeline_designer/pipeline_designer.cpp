#pragma once
#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include "../common/colours.cpp"
#include "../common/fonts.cpp"
#include "../common/icons.cpp"
#include "../common/remembered_numbers.cpp"
#include "../ui_driver/ui_driver_targets.cpp"
#include "all_processors.cpp"
#include "build_problems.cpp"
#include "core_layout.cpp"
#include "cpu_cores.cpp"
#include "designer_state_json.cpp"
#include "opened_pipeline.cpp"
#include "pipeline_build.cpp"
#include "pipeline_graph.cpp"
#include "pipeline_reader.cpp"
#include "pipeline_run.cpp"
#include "pipeline_writer.cpp"
#include "processors_panel.cpp"
#include "tooltip.cpp"

// What a processor of the graph carries while it is dragged by its block: its ProcessorId.
inline constexpr const char* PROCESSOR_MOVE_PAYLOAD = "falcon_processor_move";

// The pipeline designer: the panel of processors at the left, and a box for each CPU core in use,
// into which a processor is dragged from the panel. A connection is made by a click on an output
// and one on an input, in either order; Escape, a right click, or a click anywhere but on a port
// that fits forgets the first click. While no port is picked, a right click on an input removes
// its connections. Nothing here has coordinates of its own: where a processor is drawn follows
// from the graph.
class PipelineDesigner {
public:
    // A small pipeline that lies in no folder, for a picture of the window (see FALCON_SCREENSHOT
    // in falcon_ui.cpp), which then shows more than an empty box. Nothing of it is kept.
    struct ExamplePipeline {
        bool is_one_selected = false;  // the options of one processor are open too
    };

    // `remembered` holds what the app keeps between two sessions, the sizes of this designer's
    // panels among it, and must outlive the designer.
    PipelineDesigner(OpenedPipeline pipeline, RememberedNumbers& remembered)
        : remembered_(&remembered) {
        right_panel_width_ = remembered.value_or(right_panel_width_name, right_panel_width_);
        log_panel_height_ = remembered.value_or(log_panel_height_name, log_panel_height_);
        graph_ = std::move(pipeline.graph);
        directory_ = std::move(pipeline.directory);
        compiled_text_ = std::move(pipeline.compiled_text);
        // A pipeline that was read is kept already: its file is written when the user changes
        // something, never by opening it. A new one has no file, and gets it with the first frame.
        if (!pipeline.is_new) {
            last_kept_text_ = write_pipeline_to_keep(graph_).value_or(std::string());
        }
    }

    explicit PipelineDesigner(ExamplePipeline example) {
        add_example_pipeline(example.is_one_selected);
    }

    // Whether the user asked to go back to the list of pipelines.
    [[nodiscard]] bool is_leaving_asked() const { return is_leaving_asked_; }

    // What the pipeline is called in the top bar and in the window's title: its name, or its
    // folder's where it has none. Empty for a pipeline that has neither.
    [[nodiscard]] std::string shown_pipeline_name() const {
        if (!graph_.name.empty() || !directory_.has_value()) {
            return graph_.name;
        }
        return directory_->filename().string();
    }

    void render() {
        const ImVec2 whole_space = ImGui::GetContentRegionAvail();
        const BuildOutcome build_outcome = build_.outcome();
        // The Problems panel comes up by itself when a compilation fails, and goes when one
        // succeeds.
        if (build_outcome.state != last_build_state_) {
            problems_ = problems_of_outcome(build_outcome);
            name_processors_of(problems_, compiled_pipeline_source(),
                               compiled_processors_of_lines_);
            if (build_outcome.state == BuildState::Failed) {
                open_problems_panel();
            } else if (build_outcome.state == BuildState::Succeeded) {
                is_problems_panel_open_ = false;
                compiled_text_ = text_being_compiled_;
            }
            last_build_state_ = build_outcome.state;
        }
        const PipelineState pipeline_state{
            .build = build_outcome.state,
            .run = run_.state(),
            .is_compiled_current = is_compiled_pipeline_current(),
        };
        take_new_run_lines();
        const float row_gap = ImGui::GetStyle().ItemSpacing.y;
        const float bar_height = ImGui::GetFrameHeight() + (2.0f * bar_vertical_padding);
        // Between a bar of buttons at the top and a bar at the bottom that says how things went.
        const float height_between_bars = whole_space.y - (2.0f * (bar_height + row_gap));
        // The Log panel leaves the cores' area a height worth working in, also in a low window.
        const float log_panel_frame = row_gap + panel_edge_width;
        log_panel_height_ = std::clamp(
            log_panel_height_, lowest_log_panel,
            std::max(lowest_log_panel, height_between_bars - log_panel_frame - lowest_cores_area));
        const ImVec2 space{
            whole_space.x,
            height_between_bars - (is_log_panel_open_ ? log_panel_frame + log_panel_height_ : 0.0f),
        };
        if (selected_processor_.has_value() &&
            std::as_const(graph_).find_processor(*selected_processor_) == nullptr) {
            selected_processor_.reset();
        }
        // The right side holds a processor's options or the problems, never both: a processor
        // that is selected takes the place of the problems.
        if (selected_processor_ != last_selected_processor_) {
            if (selected_processor_.has_value()) {
                is_problems_panel_open_ = false;
            }
            last_selected_processor_ = selected_processor_;
        }
        const bool is_right_panel_shown =
            is_problems_panel_open_ || selected_processor_.has_value();
        // The panel keeps the cores' area a width worth working in, also in a window made narrow.
        right_panel_width_ =
            std::clamp(right_panel_width_, narrowest_right_panel,
                       std::max(narrowest_right_panel,
                                space.x - processors_panel_width - narrowest_cores_area));
        const float right_panel_width_now =
            is_right_panel_shown ? right_panel_width_ + panel_edge_width : 0.0f;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
        render_top_bar(pipeline_state, ImVec2(whole_space.x, bar_height));
        // ImGui's flags are signed ints by its API
        // NOLINTNEXTLINE(bugprone-signed-bitwise)
        ImGui::BeginChild("ProcessorsPanel", ImVec2(processors_panel_width, space.y),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        ProcessorsPanel::render();
        ImGui::EndChild();
        ImGui::SameLine(0.0f, 0.0f);

        // ImGui's flags are signed ints by its API
        // NOLINTNEXTLINE(bugprone-signed-bitwise)
        ImGui::BeginChild("Cores",
                          ImVec2(space.x - processors_panel_width - right_panel_width_now, space.y),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_HorizontalScrollbar);
        render_isolation_note();
        core_under_drag_ = core_under_dragged_processor();
        core_rectangles_.clear();
        block_rectangles_.clear();
        is_mouse_over_picked_or_fitting_port_ = false;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            leave_on_escape();
        }
        for (const int core_number : shown_core_numbers()) {
            render_core(core_number);
            ImGui::SameLine(0.0f, padding);
        }
        forget_picked_port_on_click_elsewhere();
        render_connections();
        scroll_while_dragging_at_an_edge();
        ImGui::EndChild();

        // The panel's width was settled before the cores were drawn, so a processor picked in
        // this frame gets its panel in the next one.
        if (is_right_panel_shown) {
            ImGui::SameLine(0.0f, 0.0f);
            render_right_panel_edge(space.y);
            ImGui::SameLine(0.0f, 0.0f);
            const bool is_showing_problems = is_problems_panel_open_;
            // ImGui's flags are signed ints by its API
            // NOLINTNEXTLINE(bugprone-signed-bitwise)
            ImGui::BeginChild(is_showing_problems ? "ProblemsPanel" : "OptionsPanel",
                              ImVec2(right_panel_width_, space.y),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
            if (is_showing_problems) {
                render_problems_panel(build_outcome);
            } else {
                render_options_panel();
            }
            ImGui::EndChild();
            name_last_item_for_ui_driver(
                {is_showing_problems ? "problems_panel" : "options_panel"});
        }
        if (is_log_panel_open_) {
            render_log_panel_edge(whole_space.x);
            render_log_panel(ImVec2(whole_space.x, log_panel_height_));
        }
        render_bottom_bar(pipeline_state, ImVec2(whole_space.x, bar_height));
        ImGui::PopStyleVar();
        remove_requested_processor();
        keep_pipeline_if_changed();
        remember_panel_sizes();
        hand_state_to_ui_driver(build_outcome, pipeline_state);
    }

private:
    static constexpr float processors_panel_width = 380.0f;
    static constexpr float narrowest_right_panel = 240.0f;
    static constexpr float narrowest_cores_area = 300.0f;
    static constexpr float panel_edge_width = 6.0f;
    static constexpr std::string_view right_panel_width_name = "right_panel_width";
    static constexpr std::string_view log_panel_height_name = "log_panel_height";
    static constexpr float lowest_log_panel = 90.0f;
    static constexpr float lowest_cores_area = 200.0f;
    static constexpr std::size_t most_run_lines = 10'000;
    static constexpr float border_line_width = 1.0f;
    static constexpr float grabbed_edge_line_width = 3.0f;
    static constexpr float padding = 10.0f;
    static constexpr float bar_vertical_padding = 3.0f;
    static constexpr float block_width = 220.0f;
    static constexpr float gap_between_rows = 40.0f;
    static constexpr float core_rounding = 12.0f;
    static constexpr float block_rounding = CARD_ROUNDING;
    static constexpr ImVec4 core_colour{0.949f, 0.949f, 0.953f, 1.0f};
    static constexpr ImVec4 not_isolated_core_colour{1.0f, 0.90f, 0.72f, 1.0f};
    static constexpr ImVec4 warning_colour{0.78f, 0.30f, 0.0f, 1.0f};
    static constexpr ImVec4 idle_state_colour{0.55f, 0.55f, 0.57f, 1.0f};
    static constexpr ImVec4 busy_state_colour{0.85f, 0.55f, 0.05f, 1.0f};
    static constexpr ImVec4 ready_state_colour{0.18f, 0.45f, 0.80f, 1.0f};
    static constexpr ImVec4 running_state_colour{0.13f, 0.55f, 0.27f, 1.0f};
    static constexpr ImVec4 failed_state_colour = FAILURE_COLOUR;
    static constexpr ImVec4 text_on_state_colour{1.0f, 1.0f, 1.0f, 1.0f};
    static constexpr ImVec4 hovered_on_state_colour{1.0f, 1.0f, 1.0f, 0.35f};
    static constexpr ImVec4 block_colour = CARD_COLOUR;
    static constexpr ImVec4 block_border_colour = CARD_BORDER_COLOUR;
    static constexpr ImVec4 block_separator_colour{0.90f, 0.90f, 0.92f, 1.0f};
    static constexpr ImVec4 hovered_row_colour = HOVERED_CARD_COLOUR;
    static constexpr ImVec4 pressed_row_colour{0.88f, 0.90f, 0.94f, 1.0f};
    static constexpr ImVec4 faint_colour = FAINT_TEXT_COLOUR;
    static constexpr ImVec4 samples_colour{0.18f, 0.49f, 0.88f, 1.0f};
    static constexpr ImVec4 events_colour{0.85f, 0.33f, 0.17f, 1.0f};
    static constexpr ImVec4 state_colour{0.10f, 0.58f, 0.28f, 1.0f};
    static constexpr ImVec4 any_kind_colour{0.40f, 0.40f, 0.40f, 1.0f};
    static constexpr ImVec4 marked_port_colour{0.78f, 0.93f, 0.78f, 1.0f};
    static constexpr ImVec4 drop_place_colour{0.10f, 0.58f, 0.28f, 1.0f};
    static constexpr ImVec4 selected_block_colour = SELECTED_COLOUR;
    static constexpr ImVec4 input_colour = INPUT_COLOUR;
    static constexpr ImVec4 input_hovered_colour = HOVERED_INPUT_COLOUR;
    static constexpr ImVec4 input_active_colour = ACTIVE_INPUT_COLOUR;

    // What the bars show of the pipeline in one frame.
    struct PipelineState {
        BuildState build = BuildState::None;
        RunState run = RunState::None;
        bool is_compiled_current = false;  // the executable was built from the graph as it stands
    };

    // Where a block was drawn in this frame, on the screen.
    struct BlockRectangle {
        ProcessorId processor = 0;
        ImVec2 top_left;
        ImVec2 bottom_right;
    };

    // Where a core's box was drawn, on the screen.
    struct CoreRectangle {
        int core_number = 0;
        ImVec2 top_left;
        ImVec2 bottom_right;
    };

    PipelineGraph graph_;
    CpuCores cpu_cores_;
    // What the app keeps between two sessions; null for the example pipeline of a picture.
    RememberedNumbers* remembered_ = nullptr;
    // The width of the panel at the right, which the user changes by dragging its left edge.
    float right_panel_width_ = 360.0f;
    PipelineBuild build_;
    BackgroundFileWriter file_writer_;
    // The Problems panel: what the last compilation that failed says is wrong, as a list or as
    // the compiler wrote it.
    std::vector<BuildProblem> problems_;
    // The processor of each line of the pipeline.cpp that was compiled last, by which a
    // compiler's error finds its processor. The graph may have changed since.
    std::vector<std::string> compiled_processors_of_lines_;
    bool is_problems_panel_open_ = false;
    bool is_raw_output_shown_ = false;
    // The processor that the last frame saw selected, to tell when the user selects another.
    std::optional<ProcessorId> last_selected_processor_;
    // The state of the compilation as the last frame saw it, to tell when it changes.
    BuildState last_build_state_ = BuildState::None;
    // The pipeline.cpp that the prototype's directory holds, as this designer wrote or read it.
    std::string last_kept_text_;
    // The last text that wasn't kept, so that the same one isn't tried again in every frame.
    std::string last_refused_text_;
    // Why the pipeline isn't kept as it stands, for the bar; empty while it is.
    std::string keeping_note_;
    // The pipeline's directory, which holds its pipeline.cpp, its executable and its runs. None
    // for the example pipeline of a picture, which is neither kept, compiled nor run.
    std::optional<std::filesystem::path> directory_;
    bool is_leaving_asked_ = false;
    // The pipeline.cpp that the directory's executable was built from; empty while there is no
    // executable. The pipeline can be run while the graph would still be written as this text.
    std::string compiled_text_;
    // The text that is being compiled, which becomes the compiled one if that goes well.
    std::string text_being_compiled_;
    PipelineRun run_;
    // What the pipeline's program wrote in its last run, the latest lines of it.
    std::deque<std::string> run_lines_;
    // The Log panel, at the bottom, which shows those lines. The user changes its height by
    // dragging its top edge.
    bool is_log_panel_open_ = false;
    float log_panel_height_ = 220.0f;
    // The boxes of the frame before, until this frame's are drawn.
    std::vector<CoreRectangle> core_rectangles_;
    // The core over whose box a processor is dragged, which shows its drop places.
    std::optional<int> core_under_drag_;
    // The port clicked first, an output or an input, while its connection waits for the click on a
    // port of the other direction.
    std::variant<std::monostate, OutputReference, InputReference> picked_port_;
    std::vector<BlockRectangle> block_rectangles_;
    // The processor whose remove icon was clicked in this frame, if any.
    std::optional<ProcessorId> processor_to_remove_;
    // The processor whose block was clicked last, whose options the panel at the right edits.
    std::optional<ProcessorId> selected_processor_;
    // Whether, in this frame, the mouse is over the picked port or over one that fits it: the
    // ports whose row handles a click itself while a port is picked.
    bool is_mouse_over_picked_or_fitting_port_ = false;

    [[nodiscard]] bool is_port_picked() const {
        return !std::holds_alternative<std::monostate>(picked_port_);
    }

    // The picked port if it is an output, or nullptr.
    [[nodiscard]] const OutputReference* picked_output() const {
        return std::get_if<OutputReference>(&picked_port_);
    }

    // The picked port if it is an input, or nullptr.
    [[nodiscard]] const InputReference* picked_input() const {
        return std::get_if<InputReference>(&picked_port_);
    }

    void forget_picked_port() { picked_port_ = std::monostate{}; }

    [[nodiscard]] static std::string_view build_state_name(BuildState state) {
        switch (state) {
            case BuildState::None:
                return "none";
            case BuildState::Running:
                return "running";
            case BuildState::Succeeded:
                return "succeeded";
            case BuildState::Failed:
                return "failed";
        }
        return "none";
    }

    [[nodiscard]] static std::string_view run_state_name(RunState state) {
        switch (state) {
            case RunState::None:
                return "none";
            case RunState::Starting:
                return "starting";
            case RunState::Running:
                return "running";
            case RunState::Stopping:
                return "stopping";
            case RunState::StartFailed:
                return "start_failed";
            case RunState::Failed:
                return "failed";
        }
        return "none";
    }

    // For a test that drives the UI (see ../ui_driver/ui_driver.cpp); nothing unless one does.
    void hand_state_to_ui_driver(const BuildOutcome& build_outcome,
                                 const PipelineState& pipeline_state) const {
        if (!ui_driver_targets().is_on()) {
            return;
        }
        ui_driver_targets().set_state(
            designer_state_as_json(graph_, {
                                               .selected_processor = selected_processor_,
                                               .picked_output = picked_output(),
                                               .picked_input = picked_input(),
                                               .shown_core_numbers = shown_core_numbers(),
                                               .offered_core_numbers = cpu_cores_.offered(),
                                               .build_state = build_state_name(build_outcome.state),
                                               .build_text = build_outcome.text,
                                               .problems = problems_,
                                               .is_problems_panel_open = is_problems_panel_open_,
                                               .is_raw_output_shown = is_raw_output_shown_,
                                               .keeping_note = keeping_note_,
                                               .run_state = run_state_name(pipeline_state.run),
                                               .bar_text = bar_state_of(pipeline_state).text,
                                               .run_lines = &run_lines_,
                                           }));
    }

    // Whether the executable was built from the graph as it stands.
    [[nodiscard]] bool is_compiled_pipeline_current() const {
        if (compiled_text_.empty()) {
            return false;
        }
        const WrittenText written = write_pipeline(graph_);
        return written.has_value() && *written == compiled_text_;
    }

    // Has the panels' sizes remembered once the user has let go of an edge, not at every step of
    // a drag.
    void remember_panel_sizes() {
        if (remembered_ == nullptr || ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            return;
        }
        remembered_->remember(right_panel_width_name, right_panel_width_);
        remembered_->remember(log_panel_height_name, log_panel_height_);
    }

    // Starts the directory's executable beside this thread. Its lines take the place of the last
    // run's.
    void start_run() {
        if (directory_.has_value()) {
            run_lines_.clear();
            is_log_panel_open_ = true;
            run_.start(*directory_ / "pipeline");
        }
    }

    // Adds what the pipeline's program wrote since the last frame. Only the latest lines are
    // held: a run may last for days.
    void take_new_run_lines() {
        for (std::string& line : run_.take_new_lines()) {
            run_lines_.push_back(std::move(line));
        }
        while (run_lines_.size() > most_run_lines) {
            run_lines_.pop_front();
        }
    }

    // Keeps the graph as the pipeline.cpp of the pipeline's directory as soon as it has changed,
    // so that no action is lost, also while the graph isn't finished. A text that can't be read
    // back is not kept, and the file stays what it was: a value that is half typed, e.g. a text
    // whose quote isn't closed yet, would else leave a file that no longer opens.
    void keep_pipeline_if_changed() {
        if (!directory_.has_value()) {
            return;
        }
        WrittenText written = write_pipeline_to_keep(graph_);
        if (!written.has_value()) {
            keeping_note_ = std::format("Not saved: {}", sentence_of(written.error()));
            return;
        }
        if (*written == last_kept_text_) {
            keeping_note_ =
                file_writer_.has_failed()
                    ? std::format("Not saved: {} can't be written to.", directory_->string())
                    : std::string();
            return;
        }
        if (*written == last_refused_text_) {
            return;
        }
        const ReadPipeline read_back = read_pipeline(*written, ALL_PROCESSORS);
        if (!read_back.has_value()) {
            keeping_note_ = std::format("Not saved: {}", read_back.error());
            last_refused_text_ = std::move(*written);
            return;
        }
        // The file is written beside this thread: a disk may be slow for a moment.
        file_writer_.write(*directory_ / "pipeline.cpp", *written);
        last_kept_text_ = std::move(*written);
        keeping_note_.clear();
    }

    // Writes the graph as its pipeline.cpp and starts compiling it beside this thread. A graph
    // that can't be written fails at once, with the reason.
    void start_build() {
        // So that a failure counts as a change of state also where the last one failed too.
        last_build_state_ = BuildState::None;
        run_.forget_last_run();
        if (!directory_.has_value()) {
            build_.fail({
                .title = "The pipeline has no folder",
                .body = "This pipeline is an example and lies in no folder.",
                .place = {},
                .found_at = {},
                .required_from = {},
                .processor_name = {},
            });
            return;
        }
        WrittenText written = write_pipeline(graph_);
        if (!written.has_value()) {
            PipelineProblem& problem = written.error();
            build_.fail({
                .title = std::move(problem.title),
                .body = std::move(problem.body),
                .place = {},
                .found_at = {},
                .required_from = {},
                .processor_name = std::move(problem.processor_name),
            });
            return;
        }
        compiled_processors_of_lines_ = processors_of_lines(*written, graph_);
        text_being_compiled_ = *written;
        build_.start(std::move(*written), *directory_);
    }

    // The bar at the top of the window, which holds the buttons at its right end: Compile, which
    // can't be clicked while a compilation runs. Its left end is kept for a menu.
    void render_top_bar(const PipelineState& state, ImVec2 size) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, bar_vertical_padding));
        // ImGui's flags are signed ints by its API
        // NOLINTNEXTLINE(bugprone-signed-bitwise)
        ImGui::BeginChild("TopBar", size,
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushStyleColor(ImGuiCol_Button, input_colour);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, input_hovered_colour);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, input_active_colour);
        const bool is_running = is_run_on(state.run);
        const bool is_compiling = state.build == BuildState::Running;
        // Leaving ends this designer, which would wait for a compilation or a run to end.
        ImGui::BeginDisabled(is_compiling || is_running);
        if (ImGui::Button("Pipelines")) {
            is_leaving_asked_ = true;
        }
        ImGui::EndDisabled();
        name_last_item_for_ui_driver({"top_bar", "pipelines"});
        render_item_tooltip("Back to the list of pipelines.");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        push_bold_font();
        ImGui::TextUnformatted(shown_pipeline_name().c_str());
        ImGui::PopFont();
        name_last_item_for_ui_driver({"top_bar", "pipeline_name"});
        ImGui::SameLine();
        // One button starts the pipeline and stops it again: it never offers both.
        const char* const run_label = is_running ? "Stop" : "Run";
        const char* const compile_label = "Compile";
        const float frame_padding = 2.0f * ImGui::GetStyle().FramePadding.x;
        const float run_width = ImGui::CalcTextSize(run_label).x + frame_padding;
        const float compile_width = ImGui::CalcTextSize(compile_label).x + frame_padding;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - run_width -
                             ImGui::GetStyle().ItemSpacing.x - compile_width);
        // A run that is stopping can't be stopped again, and only the pipeline that was compiled
        // from the graph as it stands is run.
        const bool is_run_button_usable = is_running ? state.run != RunState::Stopping
                                                     : (state.is_compiled_current && !is_compiling);
        ImGui::BeginDisabled(!is_run_button_usable);
        const bool is_run_button_clicked = ImGui::Button(run_label);
        ImGui::EndDisabled();
        name_last_item_for_ui_driver({"top_bar", "run"});
        ImGui::SameLine();
        // A compilation would replace the executable under the run that uses it.
        ImGui::BeginDisabled(is_compiling || is_running);
        const bool is_compile_asked_for = ImGui::Button(compile_label);
        ImGui::EndDisabled();
        name_last_item_for_ui_driver({"top_bar", "compile"});
        ImGui::PopStyleColor(3);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        // The bar's border lies where the panels under it begin, so that their edges meet it.
        const float border_height =
            ImGui::GetItemRectMax().y + ImGui::GetStyle().ItemSpacing.y - border_line_width;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(ImGui::GetItemRectMin().x, border_height),
                                            ImVec2(ImGui::GetItemRectMax().x, border_height),
                                            ImGui::GetColorU32(block_border_colour),
                                            border_line_width);
        if (is_compile_asked_for) {
            start_build();
        }
        if (is_run_button_clicked) {
            if (is_running) {
                run_.stop();
            } else {
                start_run();
            }
        }
    }

    // What the bar at the bottom says of the pipeline, and in which colour: amber while something
    // is going on, red after a failure, green while the pipeline runs, blue for one that is
    // compiled and ready to run, and grey before any of it.
    struct BarState {
        const char* text = "";
        ImVec4 colour;
    };

    // A run that is on comes first, then a compilation that is on, then the failure of the last
    // run, and then what stands: the last compilation's failure, or whether the executable is the
    // graph's.
    [[nodiscard]] static BarState bar_state_of(const PipelineState& state) {
        switch (state.run) {
            case RunState::Starting:
                return {.text = "Starting", .colour = busy_state_colour};
            case RunState::Running:
                return {.text = "Running", .colour = running_state_colour};
            case RunState::Stopping:
                return {.text = "Stopping", .colour = busy_state_colour};
            case RunState::None:
            case RunState::StartFailed:
            case RunState::Failed:
                break;
        }
        if (state.build == BuildState::Running) {
            return {.text = "Compiling", .colour = busy_state_colour};
        }
        if (state.run == RunState::StartFailed) {
            return {.text = "Start failed", .colour = failed_state_colour};
        }
        if (state.run == RunState::Failed) {
            return {.text = "Run failed", .colour = failed_state_colour};
        }
        if (state.build == BuildState::Failed) {
            return {.text = "Compile failed", .colour = failed_state_colour};
        }
        return state.is_compiled_current
                   ? BarState{.text = "Compiled", .colour = ready_state_colour}
                   : BarState{.text = "Not compiled yet", .colour = idle_state_colour};
    }

    // The bar at the bottom of the window: a warning with the number of problems, which opens
    // and closes the Problems panel, and how the last compilation went, in that state's colour.
    void render_bottom_bar(const PipelineState& state, ImVec2 size) {
        const BarState bar_state = bar_state_of(state);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, bar_state.colour);
        ImGui::PushStyleColor(ImGuiCol_Text, text_on_state_colour);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered_on_state_colour);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, hovered_on_state_colour);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, bar_vertical_padding));
        // ImGui's flags are signed ints by its API
        // NOLINTNEXTLINE(bugprone-signed-bitwise)
        ImGui::BeginChild("BottomBar", size,
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        render_problems_toggle();
        ImGui::SameLine();
        if (ImGui::Button("Log")) {
            is_log_panel_open_ = !is_log_panel_open_;
        }
        name_last_item_for_ui_driver({"bottom_bar", "log"});
        render_item_tooltip("Show or hide what the running pipeline writes.");
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(bar_state.text);
        name_last_item_for_ui_driver({"bottom_bar", "state"});
        if (!keeping_note_.empty()) {
            ImGui::SameLine(0.0f, 2.0f * padding);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(keeping_note_.c_str());
            name_last_item_for_ui_driver({"bottom_bar", "keeping_note"});
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(5);
    }

    // What a compilation that failed says is wrong: the designer's own problem where it has
    // one, e.g. for a graph that can't be written, and else what the compiler's text names.
    [[nodiscard]] static std::vector<BuildProblem> problems_of_outcome(
        const BuildOutcome& build_outcome) {
        if (build_outcome.state != BuildState::Failed) {
            return {};
        }
        if (build_outcome.problem.has_value()) {
            return {*build_outcome.problem};
        }
        return problems_of(build_outcome.text);
    }

    // Whether a problem of the list is about this processor, which then shows it on its block.
    [[nodiscard]] bool has_problem(const GraphProcessor& processor) const {
        return std::ranges::contains(problems_, processor.name, &BuildProblem::processor_name);
    }

    // The border of a block whose processor has a problem: it goes from the border's own colour
    // to red and back, again and again.
    [[nodiscard]] static ImVec4 problem_border_colour() {
        constexpr double seconds_there_and_back = 1.6;
        const double phase =
            std::fmod(ImGui::GetTime(), seconds_there_and_back) / seconds_there_and_back;
        const auto red_share = static_cast<float>(1.0 - std::abs((2.0 * phase) - 1.0));
        const auto mixed = [red_share](float own, float red) {
            return own + ((red - own) * red_share);
        };
        return {
            mixed(block_border_colour.x, failed_state_colour.x),
            mixed(block_border_colour.y, failed_state_colour.y),
            mixed(block_border_colour.z, failed_state_colour.z),
            1.0f,
        };
    }

    // The left edge of the panel at the right: dragged, it makes the panel wider or narrower. A
    // line shows it while the mouse is on it.
    void render_right_panel_edge(float height) {
        ImGui::InvisibleButton("right_panel_edge", ImVec2(panel_edge_width, height));
        name_last_item_for_ui_driver({"right_panel_edge"});
        const bool is_dragged = ImGui::IsItemActive();
        const bool is_under_mouse = is_dragged || ImGui::IsItemHovered();
        const ImVec2 top_left = ImGui::GetItemRectMin();
        const ImVec2 bottom_right = ImGui::GetItemRectMax();
        const float middle = (top_left.x + bottom_right.x) / 2.0f;
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(middle, top_left.y), ImVec2(middle, bottom_right.y),
            ImGui::GetColorU32(is_under_mouse ? selected_block_colour : block_border_colour),
            is_under_mouse ? grabbed_edge_line_width : border_line_width);
        if (!is_under_mouse) {
            return;
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (is_dragged) {
            right_panel_width_ -= ImGui::GetIO().MouseDelta.x;
        }
    }

    // The top edge of the Log panel, which is dragged to make the panel higher or lower.
    void render_log_panel_edge(float width) {
        ImGui::InvisibleButton("log_panel_edge", ImVec2(width, panel_edge_width));
        name_last_item_for_ui_driver({"log_panel_edge"});
        const bool is_dragged = ImGui::IsItemActive();
        const bool is_under_mouse = is_dragged || ImGui::IsItemHovered();
        const ImVec2 top_left = ImGui::GetItemRectMin();
        const ImVec2 bottom_right = ImGui::GetItemRectMax();
        const float middle = (top_left.y + bottom_right.y) / 2.0f;
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(top_left.x, middle), ImVec2(bottom_right.x, middle),
            ImGui::GetColorU32(is_under_mouse ? selected_block_colour : block_border_colour),
            is_under_mouse ? grabbed_edge_line_width : border_line_width);
        // The panel lies right under its edge, without the gap between two rows.
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y);
        if (!is_under_mouse) {
            return;
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (is_dragged) {
            log_panel_height_ -= ImGui::GetIO().MouseDelta.y;
        }
    }

    // The Log panel, at the bottom over the window's whole width: what the pipeline's program
    // wrote in its last run. It stays at its newest line while the user hasn't scrolled away
    // from it.
    void render_log_panel(ImVec2 size) {
        // ImGui's flags are signed ints by its API
        // NOLINTNEXTLINE(bugprone-signed-bitwise)
        ImGui::BeginChild("LogPanel", size,
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        const float icon_size = ImGui::GetTextLineHeight();
        push_bold_font();
        ImGui::TextUnformatted("Log");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - icon_size);
        if (render_cross_button("close_log", icon_size, "Close this panel.")) {
            is_log_panel_open_ = false;
        }
        name_last_item_for_ui_driver({"log_panel", "close"});
        ImGui::Separator();
        if (run_lines_.empty()) {
            ImGui::TextColored(faint_colour, "Nothing has run yet.");
        } else {
            ImGui::BeginChild("LogLines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                              ImGuiWindowFlags_HorizontalScrollbar);
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(run_lines_.size()));
            while (clipper.Step()) {
                for (int line_index = clipper.DisplayStart; line_index < clipper.DisplayEnd;
                     ++line_index) {
                    const std::string& line = run_lines_.at(static_cast<std::size_t>(line_index));
                    ImGui::TextUnformatted(line.data(), line.data() + line.size());
                }
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();
        }
        ImGui::EndChild();
        name_last_item_for_ui_driver({"log_panel"});
    }

    // Opens the Problems panel in the place of a processor's options, with its list.
    void open_problems_panel() {
        is_problems_panel_open_ = true;
        is_raw_output_shown_ = false;
        selected_processor_.reset();
        last_selected_processor_.reset();
    }

    // A warning with the number of problems, always in the bar: a click opens the Problems
    // panel, and another closes it.
    void render_problems_toggle() {
        const std::string label = std::format("{} {}", icon("alert-fill"), problems_.size());
        const bool is_clicked = ImGui::Button(label.c_str());
        name_last_item_for_ui_driver({"bottom_bar", "warning"});
        render_item_tooltip("Show or hide the problems.");
        if (!is_clicked) {
            return;
        }
        if (is_problems_panel_open_) {
            is_problems_panel_open_ = false;
        } else {
            open_problems_panel();
        }
    }

    // The Problems panel, at the right in the place of a processor's options: what the last
    // compilation that failed says is wrong, as a list of problems, or as the compiler wrote it
    // where the user asks for that.
    void render_problems_panel(const BuildOutcome& build_outcome) {
        const float icon_size = ImGui::GetTextLineHeight();
        push_bold_font();
        ImGui::TextUnformatted("Problems");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - icon_size);
        if (render_cross_button("close", icon_size, "Close this panel.")) {
            is_problems_panel_open_ = false;
        }
        name_last_item_for_ui_driver({"problems_panel", "close"});
        ImGui::Separator();
        ImGui::Spacing();
        if (problems_.empty()) {
            ImGui::TextColored(faint_colour, "No problems.");
            return;
        }
        ImGui::PushStyleColor(ImGuiCol_Button, input_colour);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, input_hovered_colour);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, input_active_colour);
        if (ImGui::SmallButton(is_raw_output_shown_ ? "Show the list" : "Show the raw output")) {
            is_raw_output_shown_ = !is_raw_output_shown_;
        }
        ImGui::PopStyleColor(3);
        name_last_item_for_ui_driver({"problems_panel", "raw"});
        ImGui::Spacing();
        if (is_raw_output_shown_) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(build_outcome.text.c_str());
            ImGui::PopTextWrapPos();
            return;
        }
        for (std::size_t problem_index = 0; problem_index < problems_.size(); ++problem_index) {
            render_problem(problem_index, problems_.at(problem_index));
        }
    }

    // One problem of the list, in a box of its own: what is wrong, and under it where the
    // compiler found it.
    static void render_problem(std::size_t problem_index, const BuildProblem& problem) {
        ImGui::PushID(static_cast<int>(problem_index));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, block_colour);
        ImGui::PushStyleColor(ImGuiCol_Border, block_border_colour);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, block_rounding);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
        // ImGui's flags are signed ints by its API
        // NOLINTBEGIN(bugprone-signed-bitwise)
        const ImGuiChildFlags box_flags = ImGuiChildFlags_Borders |
                                          ImGuiChildFlags_AlwaysUseWindowPadding |
                                          ImGuiChildFlags_AutoResizeY;
        // NOLINTEND(bugprone-signed-bitwise)
        ImGui::BeginChild("Problem", ImVec2(0.0f, 0.0f), box_flags);
        ImGui::TextColored(failed_state_colour, "%s", icon("alert-fill"));
        ImGui::SameLine();
        push_bold_font();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(problem.title.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        std::string about = problem.body;
        if (!problem.processor_name.empty()) {
            about = about.empty() ? problem.processor_name
                                  : std::format("{}: {}", problem.processor_name, about);
        }
        if (!about.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(about.c_str());
            ImGui::PopTextWrapPos();
        }
        if (!problem.place.empty()) {
            render_faint_wrapped(problem.place.c_str());
        }
        ImGui::EndChild();
        name_last_item_for_ui_driver({"problems_panel", "problem", std::to_string(problem_index)});
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        ImGui::Spacing();
        ImGui::PopID();
    }

    // A generator on one core, a chain that fans out on a second, and a recorder on a third.
    void add_example_pipeline(bool is_one_selected) {
        const auto add = [this](std::string_view type_name,
                                int core_number) -> std::optional<ProcessorId> {
            const auto* const type = std::ranges::find_if(
                ALL_PROCESSORS, [type_name](const ProcessorDescription& processor) {
                    return type_name == processor.name;
                });
            if (type == ALL_PROCESSORS.end()) {
                return std::nullopt;
            }
            return graph_.add_processor(
                *type, {.core_number = core_number, .next_processor = std::nullopt});
        };
        const std::optional<ProcessorId> generator = add("SignalGenerator", 0);
        const std::optional<ProcessorId> detector = add("SpikeDetector", 1);
        const std::optional<ProcessorId> trigger = add("Trigger", 1);
        const std::optional<ProcessorId> parallel_output = add("PCIeParallelOutput", 1);
        const std::optional<ProcessorId> recorder = add("Recorder", 2);
        if (!generator || !detector || !trigger || !parallel_output || !recorder) {
            return;
        }
        graph_.connect({.processor = *generator, .port = "output"},
                       {.processor = *detector, .port = "input"});
        graph_.connect({.processor = *detector, .port = "output"},
                       {.processor = *trigger, .port = "input"});
        graph_.connect({.processor = *trigger, .port = "output"},
                       {.processor = *parallel_output, .port = "input"});
        graph_.connect({.processor = *trigger, .port = "output"},
                       {.processor = *recorder, .port = "inputs"});
        if (is_one_selected) {
            selected_processor_ = detector;
        }
    }

    // Escape lets a picked port go, or else closes the options panel. While a text field is
    // being typed in, Escape is that field's, which takes back what was typed.
    void leave_on_escape() {
        if (is_port_picked()) {
            forget_picked_port();
        } else if (!ImGui::IsAnyItemActive()) {
            selected_processor_.reset();
        }
    }

    // Called inside the cores' area, before its boxes are drawn.
    [[nodiscard]] std::optional<int> core_under_dragged_processor() const {
        if (!is_any_processor_dragged()) {
            return std::nullopt;
        }
        const ImVec2 mouse = ImGui::GetMousePos();
        const ImVec2 area_top_left = ImGui::GetWindowPos();
        const ImVec2 area_size = ImGui::GetWindowSize();
        const bool is_in_area =
            mouse.x >= area_top_left.x && mouse.x < area_top_left.x + area_size.x &&
            mouse.y >= area_top_left.y && mouse.y < area_top_left.y + area_size.y;
        if (!is_in_area) {
            return std::nullopt;
        }
        for (const CoreRectangle& rectangle : core_rectangles_) {
            const bool is_in_box =
                mouse.x >= rectangle.top_left.x && mouse.x < rectangle.bottom_right.x &&
                mouse.y >= rectangle.top_left.y && mouse.y < rectangle.bottom_right.y;
            if (is_in_box) {
                return rectangle.core_number;
            }
        }
        return std::nullopt;
    }

    // Called once the frame is drawn: a block is drawn from its processor in the graph, so the
    // processor whose remove icon was clicked is taken out only after every block is drawn.
    void remove_requested_processor() {
        if (!processor_to_remove_.has_value()) {
            return;
        }
        graph_.remove_processor(*processor_to_remove_);
        if (selected_processor_ == processor_to_remove_) {
            selected_processor_.reset();
        }
        processor_to_remove_.reset();
        forget_picked_port();
    }

    // Called after this frame's ports are drawn. A right click anywhere, or a click anywhere but
    // on the picked port or on one that fits it, lets the picked port go.
    void forget_picked_port_on_click_elsewhere() {
        if (!is_port_picked()) {
            return;
        }
        const bool is_clicked_elsewhere =
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !is_mouse_over_picked_or_fitting_port_;
        if (is_clicked_elsewhere || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            forget_picked_port();
        }
    }

    // One line above the cores saying which are isolated, with the others named in its tooltip.
    void render_isolation_note() const {
        const std::vector<int> isolated = cpu_cores_.isolated();
        const std::vector<int> not_isolated = cpu_cores_.not_isolated();
        if (isolated.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, warning_colour);
            ImGui::TextUnformatted("No core of this machine is isolated (i)");
            ImGui::PopStyleColor();
            render_item_tooltip(
                "The OS and its interrupts can run on every core, so a processor can be held up "
                "at any moment and its latency isn't dependable. Falcon needs cores isolated from "
                "the OS, which is set where the machine is started (isolcpus).");
        } else {
            const std::string note =
                std::format("Isolated cores: {} (i)", core_list_text(isolated));
            ImGui::TextUnformatted(note.c_str());
            const std::string explanation = std::format(
                "These cores are kept free of the OS, so a processor on them isn't held up. "
                "Not isolated: {}. The OS and its interrupts run there, so don't put a realtime "
                "processor on them. A processor that isn't realtime, such as a recorder, is fine "
                "there.",
                not_isolated.empty() ? "none" : core_list_text(not_isolated));
            render_item_tooltip(explanation.c_str());
        }
        ImGui::Spacing();
    }

    // Called inside the cores' area. While a processor is dragged, the area scrolls sideways when
    // the mouse is near its left or right edge, so a core beyond the window's width can be reached.
    static void scroll_while_dragging_at_an_edge() {
        if (!is_any_processor_dragged()) {
            return;
        }
        constexpr float edge_width = 60.0f;
        constexpr float pixels_per_second = 1200.0f;
        const ImVec2 area_position = ImGui::GetWindowPos();
        const ImVec2 area_size = ImGui::GetWindowSize();
        const ImVec2 mouse = ImGui::GetMousePos();
        if (mouse.y < area_position.y || mouse.y > area_position.y + area_size.y) {
            return;
        }
        const float step = pixels_per_second * ImGui::GetIO().DeltaTime;
        if (mouse.x > area_position.x + area_size.x - edge_width) {
            ImGui::SetScrollX(std::min(ImGui::GetScrollX() + step, ImGui::GetScrollMaxX()));
        } else if (mouse.x < area_position.x + edge_width && mouse.x >= area_position.x) {
            ImGui::SetScrollX(std::max(ImGui::GetScrollX() - step, 0.0f));
        }
    }

    // A core's box is as wide as its rows of processors need, at least one block wide, and as
    // tall as what it holds.
    void render_core(int core_number) {
        const bool is_isolated = cpu_cores_.is_isolated(core_number);
        const CoreLayout layout = layout_of_core(graph_, core_number);
        const float box_width =
            (std::max(1.0f, layout.width_in_blocks) * (block_width + padding)) + padding;

        ImGui::PushID(core_number);
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
                              is_isolated ? core_colour : not_isolated_core_colour);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, core_rounding);
        // ImGui's flags are signed ints by its API
        // NOLINTBEGIN(bugprone-signed-bitwise)
        const ImGuiChildFlags box_flags = ImGuiChildFlags_Borders |
                                          ImGuiChildFlags_AlwaysUseWindowPadding |
                                          ImGuiChildFlags_AutoResizeY;
        // NOLINTEND(bugprone-signed-bitwise)
        ImGui::BeginChild("Core", ImVec2(box_width, 0.0f), box_flags);
        const std::string title = std::format("Core {}", core_number);
        push_bold_font();
        ImGui::TextUnformatted(title.c_str());
        ImGui::PopFont();
        if (!is_isolated) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, warning_colour);
            ImGui::TextUnformatted("not isolated (i)");
            ImGui::PopStyleColor();
            render_item_tooltip(
                "The OS and its interrupts run on this core, so a processor here can be held up "
                "at any moment. Keep realtime processors on isolated cores. A processor that "
                "isn't realtime, such as a recorder, is fine here.");
        }
        ImGui::Spacing();
        ImGui::Spacing();
        const std::optional<ProcessorId> next_processor_at_drop =
            render_rows(layout, core_under_drag_ == core_number);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        core_rectangles_.push_back({
            .core_number = core_number,
            .top_left = ImGui::GetItemRectMin(),
            .bottom_right = ImGui::GetItemRectMax(),
        });
        name_last_item_for_ui_driver({"core", std::to_string(core_number)});
        accept_dropped_processor(
            {.core_number = core_number, .next_processor = next_processor_at_drop});
        ImGui::PopID();
    }

    // Called inside a core's box: its rows of blocks from top to bottom and, while a processor is
    // dragged over the box, its drop places. Returns the processor in front of which a processor
    // dropped into the box goes in the core's list, which is that of the drop place nearest the
    // mouse; none for the end of the list.
    [[nodiscard]] std::optional<ProcessorId> render_rows(const CoreLayout& layout,
                                                         bool has_drop_places) {
        const float left_edge = ImGui::GetCursorPosX();
        const ImVec2 rows_top_left_on_screen = ImGui::GetCursorScreenPos();
        bool is_top_row = true;
        for (const std::vector<PlacedProcessor>& row : layout.rows) {
            if (!is_top_row) {
                // Room for the connections' lines to bend between two rows.
                ImGui::Dummy(ImVec2(0.0f, gap_between_rows));
            }
            is_top_row = false;
            bool is_first_of_row = true;
            for (const PlacedProcessor& placed : row) {
                const GraphProcessor* processor = graph_.find_processor(placed.processor);
                if (processor == nullptr) {
                    continue;
                }
                move_to_column(left_edge, placed.column, is_first_of_row);
                render_block(*processor);
                is_first_of_row = false;
            }
        }
        if (layout.rows.empty()) {
            render_place_of_empty_core(has_drop_places);
            return std::nullopt;
        }
        if (!has_drop_places) {
            return std::nullopt;
        }
        return render_drop_bars(layout, rows_top_left_on_screen);
    }

    // Moves to where a block of this column starts, on the line of the block drawn before it
    // unless it is the first of its row.
    static void move_to_column(float left_edge, float column, bool is_first_of_row) {
        if (!is_first_of_row) {
            ImGui::SameLine();
        }
        ImGui::SetCursorPosX(left_edge + (column * (block_width + padding)));
    }

    // Called right after a core's box, which it makes a place to drop a processor dragged from the
    // panel or by its block; ImGui marks the box while one is dragged over it. The processor goes
    // to the given place of the core's list.
    void accept_dropped_processor(const CoreListPlace& place) {
        if (!ImGui::BeginDragDropTarget()) {
            return;
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(PROCESSOR_TYPE_PAYLOAD)) {
            std::size_t processor_index = 0;
            if (std::cmp_equal(payload->DataSize, sizeof(processor_index))) {
                std::memcpy(&processor_index, payload->Data, sizeof(processor_index));
                if (processor_index < ALL_PROCESSORS.size()) {
                    graph_.add_processor(ALL_PROCESSORS.at(processor_index), place);
                }
            }
        }
        if (ImGui::AcceptDragDropPayload(PROCESSOR_MOVE_PAYLOAD) != nullptr) {
            if (const std::optional<ProcessorId> moved = moved_processor_id()) {
                place_moved_processor(*moved, place);
            }
        }
        ImGui::EndDragDropTarget();
    }

    // A processor dropped into another core's box goes there alone. One dropped into its own
    // core's box takes its whole chain along: the chain's place among the core's chains is that
    // of its top in the core's list, so it is the top that is placed.
    void place_moved_processor(ProcessorId processor_id, const CoreListPlace& place) {
        const GraphProcessor* processor = graph_.find_processor(processor_id);
        if (processor == nullptr) {
            return;
        }
        const ProcessorId placed_processor_id = processor->core_number == place.core_number
                                                    ? chain_top_of(graph_, processor_id)
                                                    : processor_id;
        graph_.place_processor(placed_processor_id, place);
    }

    // The drop places of a core that holds processors: a bar in the gap left of each chain and
    // one right of the last chain, from the top row down to the bottom of the box. They are drawn
    // in gaps that are there anyway, so that nothing moves while a processor is dragged. The one
    // nearest the mouse is marked, and a processor dropped anywhere in the box goes there.
    // Returns the processor that one stands in front of; none for the end of the core's list.
    [[nodiscard]] static std::optional<ProcessorId> render_drop_bars(
        const CoreLayout& layout, ImVec2 rows_top_left_on_screen) {
        if (layout.drop_places.empty()) {
            return std::nullopt;
        }
        constexpr float half = 0.5f;
        constexpr float marked_bar_width = 4.0f;
        const ImVec2 box_top_left = ImGui::GetWindowPos();
        const ImVec2 box_size = ImGui::GetWindowSize();
        const ImVec2 box_bottom_right(box_top_left.x + box_size.x, box_top_left.y + box_size.y);
        const float bottom_on_screen = box_bottom_right.y - padding;
        const float mouse_x = ImGui::GetMousePos().x;
        const float top_on_screen = rows_top_left_on_screen.y;
        const auto bar_centre_on_screen = [&](const DropPlace& drop_place) {
            return rows_top_left_on_screen.x + (drop_place.column * (block_width + padding)) -
                   (padding * half);
        };
        const auto nearest =
            std::ranges::min_element(layout.drop_places, {}, [&](const DropPlace& drop_place) {
                return std::abs(bar_centre_on_screen(drop_place) - mouse_x);
            });

        // The bars at the two ends lie in the box's padding, which the box doesn't draw into
        // otherwise.
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->PushClipRect(box_top_left, box_bottom_right, false);
        for (auto drop_place = layout.drop_places.begin(); drop_place != layout.drop_places.end();
             ++drop_place) {
            const float bar_centre = bar_centre_on_screen(*drop_place);
            const std::string place_number =
                std::to_string(std::ranges::distance(layout.drop_places.begin(), drop_place));
            const ScreenRectangle bar{
                .top_left = ImVec2(bar_centre - (marked_bar_width * half), top_on_screen),
                .bottom_right = ImVec2(bar_centre + (marked_bar_width * half), bottom_on_screen),
            };
            name_rectangle_for_ui_driver({"drop", place_number}, bar,
                                         drop_place == nearest ? "marked" : "");
            if (drop_place == nearest) {
                draw_list->AddRectFilled(
                    ImVec2(bar_centre - (marked_bar_width * half), top_on_screen),
                    ImVec2(bar_centre + (marked_bar_width * half), bottom_on_screen),
                    ImGui::GetColorU32(drop_place_colour));
            } else {
                add_dashed_line(draw_list, ImVec2(bar_centre, top_on_screen),
                                ImVec2(bar_centre, bottom_on_screen),
                                ImGui::GetColorU32(faint_colour));
            }
        }
        draw_list->PopClipRect();
        return nearest->next_processor;
    }

    // The place a core that holds no processor keeps free for its first one, so that its box
    // doesn't grow when a processor is dragged over it. While one is, the place is the core's one
    // drop place: an outline of dashes around "Drop here".
    static void render_place_of_empty_core(bool is_drop_place) {
        constexpr float height_in_text_lines = 4.0f;
        constexpr float half = 0.5f;
        constexpr const char* label = "Drop here";
        const ImVec2 size(block_width,
                          ImGui::GetTextLineHeightWithSpacing() * height_in_text_lines);
        ImGui::Dummy(size);
        if (!is_drop_place) {
            return;
        }
        name_last_item_for_ui_driver({"drop", "0"}, "marked");
        const ImVec2 top_left = ImGui::GetItemRectMin();
        const ImVec2 bottom_right = ImGui::GetItemRectMax();
        const ImU32 colour = ImGui::GetColorU32(drop_place_colour);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        add_dashed_line(draw_list, top_left, ImVec2(bottom_right.x, top_left.y), colour);
        add_dashed_line(draw_list, ImVec2(bottom_right.x, top_left.y), bottom_right, colour);
        add_dashed_line(draw_list, bottom_right, ImVec2(top_left.x, bottom_right.y), colour);
        add_dashed_line(draw_list, ImVec2(top_left.x, bottom_right.y), top_left, colour);
        const ImVec2 label_size = ImGui::CalcTextSize(label);
        draw_list->AddText(ImVec2(top_left.x + ((size.x - label_size.x) * half),
                                  top_left.y + ((size.y - label_size.y) * half)),
                           colour, label);
    }

    static void add_dashed_line(ImDrawList* draw_list, ImVec2 line_start, ImVec2 line_end,
                                ImU32 colour) {
        constexpr float dash_length = 7.0f;
        constexpr float gap_length = 5.0f;
        constexpr float thickness = 1.5f;
        const float length = std::hypot(line_end.x - line_start.x, line_end.y - line_start.y);
        if (length <= 0.0f) {
            return;
        }
        const ImVec2 direction((line_end.x - line_start.x) / length,
                               (line_end.y - line_start.y) / length);
        const int dash_count = static_cast<int>(std::ceil(length / (dash_length + gap_length)));
        for (int dash = 0; dash < dash_count; ++dash) {
            const float dash_start = static_cast<float>(dash) * (dash_length + gap_length);
            const float dash_end = std::min(dash_start + dash_length, length);
            draw_list->AddLine(ImVec2(line_start.x + (direction.x * dash_start),
                                      line_start.y + (direction.y * dash_start)),
                               ImVec2(line_start.x + (direction.x * dash_end),
                                      line_start.y + (direction.y * dash_end)),
                               colour, thickness);
        }
    }

    // A block: its inputs on top, its name and type, its outputs at the bottom, as data flows.
    void render_block(const GraphProcessor& processor) {
        constexpr float border_thickness = 1.0f;
        constexpr float selected_border_thickness = 2.0f;
        const bool is_selected = selected_processor_ == processor.id;
        ImGui::PushID(static_cast<int>(processor.id));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, block_colour);
        const bool is_marked = has_problem(processor);
        ImVec4 border_colour = is_selected ? selected_block_colour : block_border_colour;
        if (is_marked) {
            border_colour = problem_border_colour();
        }
        ImGui::PushStyleColor(ImGuiCol_Border, border_colour);
        ImGui::PushStyleColor(ImGuiCol_Separator, block_separator_colour);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, block_rounding);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, is_selected || is_marked
                                                               ? selected_border_thickness
                                                               : border_thickness);
        // ImGui's flags are signed ints by its API
        // NOLINTBEGIN(bugprone-signed-bitwise)
        const ImGuiChildFlags block_flags = ImGuiChildFlags_Borders |
                                            ImGuiChildFlags_AlwaysUseWindowPadding |
                                            ImGuiChildFlags_AutoResizeY;
        // NOLINTEND(bugprone-signed-bitwise)
        ImGui::BeginChild("Processor", ImVec2(block_width, 0.0f), block_flags);
        if (has_port(*processor.type, PortDirection::Input)) {
            render_ports(processor, PortDirection::Input);
            ImGui::Separator();
        }
        render_block_handle(processor);
        render_block_stream(processor);
        render_block_options(processor);
        if (has_port(*processor.type, PortDirection::Output)) {
            ImGui::Separator();
            render_ports(processor, PortDirection::Output);
        }
        ImGui::EndChild();
        block_rectangles_.push_back({
            .processor = processor.id,
            .top_left = ImGui::GetItemRectMin(),
            .bottom_right = ImGui::GetItemRectMax(),
        });
        name_last_item_for_ui_driver({"block", processor.name});
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(3);
        ImGui::PopID();
    }

    // The bold face of the text around it, or that text's own face when Falcon has no bold one.
    // To be followed by ImGui::PopFont().
    static void push_bold_font() {
        constexpr int text_size = 16;
        const auto bold_font = ui_fonts.find({.size = text_size, .style = FontStyle::Bold});
        ImGui::PushFont(bold_font == ui_fonts.end() ? nullptr : bold_font->second, 0.0f);
    }

    // The block's name and type, which are also the handle it is dragged by into another core's
    // box, with the remove icon at their right. The dragged processor's name follows the mouse in
    // a tooltip of ImGui's own.
    void render_block_handle(const GraphProcessor& processor) {
        const ImVec2 handle_position = ImGui::GetCursorPos();
        const float handle_height =
            (ImGui::GetTextLineHeight() * 2.0f) + ImGui::GetStyle().ItemSpacing.y;
        const float icon_size = ImGui::GetTextLineHeight();
        const float gap_before_icon = ImGui::GetStyle().ItemSpacing.x;
        const float handle_width = ImGui::GetContentRegionAvail().x - icon_size - gap_before_icon;
        if (ImGui::InvisibleButton("handle", ImVec2(handle_width, handle_height))) {
            selected_processor_ = processor.id;
        }
        const ImVec2 handle_top_left = ImGui::GetItemRectMin();
        const ImVec2 handle_bottom_right = ImGui::GetItemRectMax();
        name_last_item_for_ui_driver({"block", processor.name, "handle"});
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        ImGui::PushStyleColor(ImGuiCol_PopupBg, TOOLTIP_BACKGROUND);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, TOOLTIP_PADDING);
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(PROCESSOR_MOVE_PAYLOAD, &processor.id, sizeof(processor.id));
            ImGui::TextUnformatted(processor.name.c_str());
            ImGui::EndDragDropSource();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        ImGui::SameLine(0.0f, gap_before_icon);
        if (render_remove_icon(icon_size)) {
            processor_to_remove_ = processor.id;
        }
        name_last_item_for_ui_driver({"block", processor.name, "remove"});

        // The name and the type are drawn over the handle, which is as high as the two, and not
        // beyond it, where the remove icon is.
        ImGui::SetCursorPos(handle_position);
        ImGui::PushClipRect(handle_top_left, handle_bottom_right, true);
        push_bold_font();
        ImGui::TextUnformatted(processor.name.c_str());
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, faint_colour);
        ImGui::TextUnformatted(processor.type->name);
        ImGui::PopStyleColor();
        ImGui::PopClipRect();
    }

    // The stream a processor starts, in short, as a line like those of its options. A click on it
    // opens the panel that edits it.
    void render_block_stream(const GraphProcessor& processor) {
        if (!processor.stream.has_value()) {
            return;
        }
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, faint_colour);
        ImGui::TextUnformatted("stream: ");
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, 0.0f);
        const std::string stream_shown = text_cut_to_width(
            std::format("{} ch, {} Hz", processor.stream->channel_count, processor.stream->rate),
            ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(stream_shown.c_str());
        ImGui::EndGroup();
        name_last_item_for_ui_driver({"block", processor.name, "stream"});
        if (ImGui::IsItemClicked()) {
            selected_processor_ = processor.id;
        }
        render_item_tooltip(
            "The stream this processor starts: how many channels it gives, and how many samples "
            "of each a second. Every processor that takes its samples, directly or through "
            "others, works with the same.");
    }

    // The block's options in short: a line for each, its name and its value as written, cut where
    // the block ends. The tooltip of a line says the rest, and a click on one opens the panel
    // that edits them.
    void render_block_options(const GraphProcessor& processor) {
        std::size_t option_index = 0;
        for (const OptionDescription& option : processor.type->options) {
            const std::string value = option_index < processor.option_values.size()
                                          ? processor.option_values.at(option_index)
                                          : std::string(option.default_value);
            ++option_index;

            const std::string value_text = option.choices.empty() ? value : shown_choice(value);
            // The name has the room that the value leaves, and half the line at least.
            constexpr float half = 0.5f;
            const float line_width = ImGui::GetContentRegionAvail().x;
            const float name_width =
                std::max(line_width * half, line_width - ImGui::CalcTextSize(value_text.c_str()).x);
            ImGui::BeginGroup();
            ImGui::PushStyleColor(ImGuiCol_Text, faint_colour);
            constexpr const char* after_name = ": ";
            const std::string name_shown =
                text_cut_to_width(option.name, name_width - ImGui::CalcTextSize(after_name).x) +
                after_name;
            ImGui::TextUnformatted(name_shown.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0f, 0.0f);
            const std::string value_shown =
                text_cut_to_width(value_text, ImGui::GetContentRegionAvail().x);
            ImGui::TextUnformatted(value_shown.c_str());
            ImGui::EndGroup();
            name_last_item_for_ui_driver({"block", processor.name, "option", option.name});
            if (ImGui::IsItemClicked()) {
                selected_processor_ = processor.id;
            }

            const std::string explanation =
                std::format("{}: {}\n\n{}, {} by default\n\n{}", option.name, value, option.type,
                            option.default_value, option.description);
            render_item_tooltip(explanation.c_str());
        }
    }

    // The text itself when it is no wider than max_width, or else its longest start that fits
    // with "..." after it.
    [[nodiscard]] static std::string text_cut_to_width(std::string_view text, float max_width) {
        constexpr std::string_view ellipsis = "...";
        const auto width_of = [](const std::string& shown) {
            return ImGui::CalcTextSize(shown.c_str()).x;
        };
        std::string cut(text);
        if (width_of(cut) <= max_width) {
            return cut;
        }
        while (!cut.empty()) {
            remove_last_character(cut);
            std::string shown = cut;
            shown += ellipsis;
            if (width_of(shown) <= max_width) {
                return shown;
            }
        }
        return std::string(ellipsis);
    }

    // Removes the last character of UTF-8 text, which can be several bytes long.
    static void remove_last_character(std::string& text) {
        constexpr unsigned continuation_mask = 0xC0U;
        constexpr unsigned continuation_bits = 0x80U;
        const auto is_last_byte_a_continuation = [&text] {
            const auto last_byte = static_cast<unsigned>(static_cast<unsigned char>(text.back()));
            return (last_byte & continuation_mask) == continuation_bits;
        };
        while (!text.empty() && is_last_byte_a_continuation()) {
            text.pop_back();
        }
        if (!text.empty()) {
            text.pop_back();
        }
    }

    // A cross at the right of a block's name, which can't be clicked while a port is picked.
    // Returns whether it was clicked.
    [[nodiscard]] bool render_remove_icon(float icon_size) const {
        ImGui::BeginDisabled(is_port_picked());
        const bool is_clicked =
            render_cross_button("remove", icon_size, "Remove this processor and its connections.");
        ImGui::EndDisabled();
        return is_clicked;
    }

    // A cross to click, with a tooltip. Returns whether it was clicked.
    [[nodiscard]] static bool render_cross_button(const char* button_id, float icon_size,
                                                  const char* tooltip) {
        constexpr float inset_in_icon_sizes = 0.25f;
        constexpr float thickness = 2.0f;
        const bool is_clicked = ImGui::InvisibleButton(button_id, ImVec2(icon_size, icon_size));
        const bool is_hovered = ImGui::IsItemHovered();
        const ImVec2 top_left = ImGui::GetItemRectMin();
        const ImVec2 bottom_right = ImGui::GetItemRectMax();
        render_item_tooltip(tooltip);
        const float inset = icon_size * inset_in_icon_sizes;
        const ImU32 colour = ImGui::GetColorU32(is_hovered ? warning_colour : faint_colour);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddLine(ImVec2(top_left.x + inset, top_left.y + inset),
                           ImVec2(bottom_right.x - inset, bottom_right.y - inset), colour,
                           thickness);
        draw_list->AddLine(ImVec2(bottom_right.x - inset, top_left.y + inset),
                           ImVec2(top_left.x + inset, bottom_right.y - inset), colour, thickness);
        return is_clicked;
    }

    // Called inside the options panel: the selected processor's name and type, then each of its
    // options with its description and a text field that holds its value as written in
    // pipeline.cpp.
    void render_options_panel() {
        GraphProcessor* processor =
            selected_processor_.has_value() ? graph_.find_processor(*selected_processor_) : nullptr;
        if (processor == nullptr) {
            return;
        }
        const float icon_size = ImGui::GetTextLineHeight();
        ImGui::TextUnformatted(processor->name.c_str());
        // At the right end of what the panel has room for, which is less when it has a scrollbar.
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - icon_size);
        if (render_cross_button("close", icon_size, "Close this panel.")) {
            selected_processor_.reset();
        }
        name_last_item_for_ui_driver({"options_panel", "close"});
        ImGui::PushStyleColor(ImGuiCol_Text, faint_colour);
        ImGui::TextUnformatted(processor->type->name);
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();

        if (processor->stream.has_value()) {
            render_stream_inputs(*processor->stream);
        }
        if (processor->type->options.empty() && !processor->stream.has_value()) {
            ImGui::PushStyleColor(ImGuiCol_Text, faint_colour);
            ImGui::TextUnformatted("This processor has no options.");
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }
        std::size_t option_index = 0;
        for (const OptionDescription& option : processor->type->options) {
            if (option_index < processor->option_values.size()) {
                ImGui::PushID(option.name);
                render_option_input(option, processor->option_values.at(option_index));
                ImGui::PopID();
            }
            ++option_index;
        }
        if (has_port(*processor->type, PortDirection::Output)) {
            render_output_names(*processor);
        }
    }

    // In the options panel, the name of what each of the processor's outputs gives, in a field
    // that takes the characters of a name only. What is typed becomes the name as soon as it can
    // be one; until then, e.g. while the field is empty, the name stays what it was. A name of
    // the user's has a button that puts the output's own back.
    void render_output_names(const GraphProcessor& processor) {
        ImGui::Separator();
        ImGui::Spacing();
        push_bold_font();
        ImGui::TextUnformatted("Outputs");
        ImGui::PopFont();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_FrameBg, input_colour);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, input_hovered_colour);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, input_active_colour);
        for (const PortDescription& port : processor.type->ports) {
            if (port.direction != PortDirection::Output) {
                continue;
            }
            const OutputReference output{.processor = processor.id, .port = port.name};
            ImGui::PushID(port.name);
            ImGui::TextUnformatted(port.name);
            render_faint_wrapped("Label for this output in recordings and logs.");
            std::string typed_name = graph_.name_of_output(output);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (render_filtered_field("##name", typed_name, IDENTIFIER_CHARACTERS) &&
                is_identifier(typed_name)) {
                graph_.rename_output(output, typed_name);
            }
            name_last_item_for_ui_driver({"options_panel", "output_name", port.name});
            if (graph_.is_output_name_shared(output)) {
                ImGui::PushStyleColor(ImGuiCol_Text, warning_colour);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(
                    "Another output has this name. Each output needs a name of its own.");
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                name_last_item_for_ui_driver({"options_panel", "output_name_shared", port.name});
            }
            if (graph_.name_of_output(output) != graph_.own_name_of_output(output)) {
                const bool is_own_name_asked_for = ImGui::SmallButton("Put its own name back");
                name_last_item_for_ui_driver({"options_panel", "own_output_name", port.name});
                if (is_own_name_asked_for) {
                    graph_.rename_output(output, graph_.own_name_of_output(output));
                }
            }
            ImGui::Spacing();
            ImGui::Spacing();
            ImGui::PopID();
        }
        ImGui::PopStyleColor(3);
    }

    // A text field that edits a string of any length: ImGui asks through the callback for a
    // larger buffer when the text outgrows the string's.
    static void render_text_field(const char* label, std::string& text) {
        const auto grow_buffer = [](ImGuiInputTextCallbackData* data) -> int {
            if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
                auto* edited_text = static_cast<std::string*>(data->UserData);
                edited_text->resize(static_cast<std::size_t>(data->BufTextLen));
                data->Buf = edited_text->data();
            }
            return 0;
        };
        ImGui::InputText(label, text.data(), text.capacity() + 1,
                         ImGuiInputTextFlags_CallbackResize, grow_buffer, &text);
    }

    // The number that a text is, or none when the text is anything but one plain number.
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

    [[nodiscard]] static bool is_decimal_option(const OptionDescription& option) {
        const std::string_view type = option.type;
        return type == "double" || type == "float";
    }

    [[nodiscard]] static bool is_whole_number_option(const OptionDescription& option) {
        return std::string_view(option.type) == "int";
    }

    // A value of a fixed list as it is shown: without the name of its list in front of it, e.g.
    // Sine for Waveform::Sine.
    [[nodiscard]] static std::string shown_choice(std::string_view choice) {
        constexpr std::string_view scope = "::";
        const std::size_t scope_position = choice.rfind(scope);
        return std::string(scope_position == std::string_view::npos
                               ? choice
                               : choice.substr(scope_position + scope.size()));
    }

    // A list to choose one of an option's values from, for an option whose type is a fixed list
    // of values. The option's value stays as it is written in pipeline.cpp.
    static void render_choice_box(const OptionDescription& option, std::string& value) {
        ImGui::PushStyleColor(ImGuiCol_PopupBg, block_colour);
        ImGui::PushStyleColor(ImGuiCol_Border, block_border_colour);
        ImGui::PushStyleColor(ImGuiCol_Header, pressed_row_colour);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, hovered_row_colour);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, pressed_row_colour);
        ImGui::PushStyleColor(ImGuiCol_Button, input_hovered_colour);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, input_active_colour);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        if (ImGui::BeginCombo("##value", shown_choice(value).c_str())) {
            for (const char* choice : option.choices) {
                const std::string choice_shown = shown_choice(choice);
                if (ImGui::Selectable(choice_shown.c_str(), value == choice)) {
                    value = choice;
                }
                name_last_item_for_ui_driver(
                    {"options_panel", "choice", option.name, choice_shown});
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(7);
    }

    // The input for an option's value. An option whose type is a fixed list of values has a list
    // to choose from, and an option that is a number has a box that takes a number only (see
    // render_number_box). Every other option has a text field, and so
    // has a number whose value isn't written as one plain number, e.g. a default that the
    // processor's source writes as a calculation.
    static void render_value_input(const OptionDescription& option, std::string& value) {
        if (!option.choices.empty()) {
            render_choice_box(option, value);
            return;
        }
        if (is_decimal_option(option)) {
            if (const std::optional<double> number = number_written_as<double>(value)) {
                render_number_box(value, *number);
                return;
            }
        }
        if (is_whole_number_option(option)) {
            if (const std::optional<int> number = number_written_as<int>(value)) {
                render_number_box(value, *number);
                return;
            }
        }
        render_text_field("##value", value);
    }

    // What a filtered field's callback needs: the text it edits and the characters it takes.
    struct FilteredField {
        std::string* text = nullptr;
        std::string_view allowed_characters;
    };

    // A text field that takes only the given characters, typed or pasted. Returns whether its
    // text was changed.
    static bool render_filtered_field(const char* label, std::string& text,
                                      std::string_view allowed_characters) {
        constexpr unsigned highest_plain_character = 127;
        FilteredField field{.text = &text, .allowed_characters = allowed_characters};
        const auto callback = [](ImGuiInputTextCallbackData* data) -> int {
            const auto* edited_field = static_cast<const FilteredField*>(data->UserData);
            if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
                edited_field->text->resize(static_cast<std::size_t>(data->BufTextLen));
                data->Buf = edited_field->text->data();
                return 0;
            }
            if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
                const bool is_allowed =
                    data->EventChar <= highest_plain_character &&
                    edited_field->allowed_characters.contains(static_cast<char>(data->EventChar));
                return is_allowed ? 0 : 1;
            }
            return 0;
        };
        // ImGui's flags are signed ints by its API
        // NOLINTNEXTLINE(bugprone-signed-bitwise)
        return ImGui::InputText(
            label, text.data(), text.capacity() + 1,
            ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_CallbackCharFilter, callback,
            &field);
    }

    // A box for one plain number, with two buttons at its right that step it by one. It takes
    // digits, a minus and, for a number with decimals, a point, and nothing else: neither a letter
    // nor a calculation can be typed. What is typed becomes the value as soon as it is a number;
    // until then, e.g. after a lone minus, the value stays what it was.
    template <typename Number>
    static void render_number_box(std::string& value, Number number) {
        const float button_size = ImGui::GetFrameHeight();
        const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
        const float field_width = ImGui::CalcItemWidth() - (2.0f * (button_size + gap));
        const std::string_view allowed_characters =
            std::is_floating_point_v<Number> ? "0123456789.-" : "0123456789-";

        ImGui::BeginGroup();
        std::string typed = value;
        ImGui::SetNextItemWidth(std::max(1.0f, field_width));
        if (render_filtered_field("##value", typed, allowed_characters)) {
            if (const std::optional<Number> typed_number = number_written_as<Number>(typed)) {
                value = std::format("{}", *typed_number);
            }
        }
        ImGui::PushStyleColor(ImGuiCol_Button, input_colour);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, input_hovered_colour);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, input_active_colour);
        ImGui::SameLine(0.0f, gap);
        if (ImGui::Button("-", ImVec2(button_size, button_size)) &&
            number > std::numeric_limits<Number>::lowest()) {
            value = std::format("{}", number - Number{1});
        }
        ImGui::SameLine(0.0f, gap);
        if (ImGui::Button("+", ImVec2(button_size, button_size)) &&
            number < std::numeric_limits<Number>::max()) {
            value = std::format("{}", number + Number{1});
        }
        ImGui::PopStyleColor(3);
        ImGui::EndGroup();
    }

    // Whether a value is the option's default. Two numbers are compared as numbers, so that 1 and
    // 1.0 are the same.
    [[nodiscard]] static bool is_default_value(const OptionDescription& option,
                                               const std::string& value) {
        if (is_decimal_option(option) || is_whole_number_option(option)) {
            const std::optional<double> number = number_written_as<double>(value);
            const std::optional<double> default_number =
                number_written_as<double>(option.default_value);
            if (number.has_value() && default_number.has_value()) {
                return *number == *default_number;
            }
        }
        return value == option.default_value;
    }

    // In the options panel, the two values of the stream a processor starts, each in a box that
    // takes a number: a channel count of 1 at least, and a rate above 0.
    static void render_stream_inputs(GraphStream& stream) {
        push_bold_font();
        ImGui::TextUnformatted("Stream");
        ImGui::PopFont();
        render_faint_wrapped(
            "What this processor gives. Every processor that takes its samples, directly or "
            "through others, works with the same channel count and rate.");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_FrameBg, input_colour);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, input_hovered_colour);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, input_active_colour);

        ImGui::PushID("channel_count");
        ImGui::TextUnformatted("channel count");
        render_faint_wrapped("How many channels each sample holds. 1 at least.");
        std::string channel_count_text = std::format("{}", stream.channel_count);
        ImGui::SetNextItemWidth(-FLT_MIN);
        render_number_box(channel_count_text, stream.channel_count);
        name_last_item_for_ui_driver({"options_panel", "stream", "channel_count"});
        if (const std::optional<int> channel_count = number_written_as<int>(channel_count_text);
            channel_count.has_value() && *channel_count >= 1) {
            stream.channel_count = *channel_count;
        }
        ImGui::PopID();
        ImGui::Spacing();

        ImGui::PushID("rate");
        ImGui::TextUnformatted("rate");
        render_faint_wrapped("How many samples of each channel a second, in Hz. Above 0.");
        std::string rate_text = std::format("{}", stream.rate);
        ImGui::SetNextItemWidth(-FLT_MIN);
        render_number_box(rate_text, stream.rate);
        name_last_item_for_ui_driver({"options_panel", "stream", "rate"});
        if (const std::optional<double> rate = number_written_as<double>(rate_text);
            rate.has_value() && *rate > 0.0) {
            stream.rate = *rate;
        }
        ImGui::PopID();

        ImGui::PopStyleColor(3);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    static void render_faint_wrapped(const char* text) {
        ImGui::PushStyleColor(ImGuiCol_Text, faint_colour);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    // One option in the options panel: its name, what it is, the input for its value, and its
    // type and default with a button that puts the default back.
    static void render_option_input(const OptionDescription& option, std::string& value) {
        ImGui::TextUnformatted(option.name);
        ImGui::PushStyleColor(ImGuiCol_Text, faint_colour);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(option.description);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        ImGui::PushStyleColor(ImGuiCol_FrameBg, input_colour);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, input_hovered_colour);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, input_active_colour);
        ImGui::SetNextItemWidth(-FLT_MIN);
        render_value_input(option, value);
        name_last_item_for_ui_driver({"options_panel", "input", option.name});
        ImGui::PopStyleColor(3);

        const std::string type_and_default =
            std::format("{}, {} by default", option.type, option.default_value);
        ImGui::PushStyleColor(ImGuiCol_Text, faint_colour);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(type_and_default.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        if (!is_default_value(option, value)) {
            const bool is_default_asked_for = ImGui::SmallButton("Put the default back");
            name_last_item_for_ui_driver({"options_panel", "default", option.name});
            if (is_default_asked_for) {
                value = option.default_value;
            }
        }
        ImGui::Spacing();
        ImGui::Spacing();
    }

    [[nodiscard]] static bool has_port(const ProcessorDescription& type, PortDirection direction) {
        return std::ranges::any_of(type.ports, [direction](const PortDescription& port) {
            return port.direction == direction;
        });
    }

    void render_ports(const GraphProcessor& processor, PortDirection direction) {
        for (const PortDescription& port : processor.type->ports) {
            if (port.direction != direction) {
                continue;
            }
            if (direction == PortDirection::Output) {
                render_output(processor, port);
            } else {
                render_input(processor, port);
            }
        }
    }

    // A click picks the output as one end of a connection, and a second click lets it go. While a
    // port is picked, the outputs that fit it are marked and every other port can't be clicked.
    void render_output(const GraphProcessor& processor, const PortDescription& port) {
        const OutputReference output{.processor = processor.id, .port = port.name};
        const OutputReference* const picked = picked_output();
        const bool is_picked = picked != nullptr && *picked == output;
        const InputReference* const waiting_input = picked_input();
        const bool is_fitting =
            waiting_input != nullptr && graph_.can_connect(output, *waiting_input);
        const bool is_picked_or_fitting = is_picked || is_fitting;
        const bool is_greyed = is_port_picked() && !is_picked_or_fitting;
        std::string rejection;
        if (is_greyed) {
            rejection = waiting_input != nullptr
                            ? rejection_message(output, *waiting_input, PortDirection::Output)
                            : "An output connects to an input, not to another output.";
        }
        ImGui::BeginDisabled(is_greyed);
        const bool is_clicked =
            render_port_row(port, is_picked_or_fitting,
                            rejection.empty() ? port_explanation(processor, port) : rejection);
        name_last_item_for_ui_driver({"port", processor.name, "output", port.name}, rejection);
        if (is_picked_or_fitting && ImGui::IsItemHovered()) {
            is_mouse_over_picked_or_fitting_port_ = true;
        }
        ImGui::EndDisabled();
        if (is_clicked) {
            if (is_fitting) {
                graph_.connect(output, *waiting_input);
                forget_picked_port();
            } else if (is_picked) {
                forget_picked_port();
            } else {
                picked_port_ = output;
            }
        }
    }

    // A click picks the input as one end of a connection, and a second click lets it go. While a
    // port is picked, the inputs that fit it are marked and every other port can't be clicked.
    void render_input(const GraphProcessor& processor, const PortDescription& port) {
        const InputReference input{.processor = processor.id, .port = port.name};
        const InputReference* const picked = picked_input();
        const bool is_picked = picked != nullptr && *picked == input;
        const OutputReference* const waiting_output = picked_output();
        const bool is_fitting =
            waiting_output != nullptr && graph_.can_connect(*waiting_output, input);
        const bool is_picked_or_fitting = is_picked || is_fitting;
        const bool is_greyed = is_port_picked() && !is_picked_or_fitting;
        std::string rejection;
        if (is_greyed) {
            rejection = waiting_output != nullptr
                            ? rejection_message(*waiting_output, input, PortDirection::Input)
                            : "An input connects to an output, not to another input.";
        }
        ImGui::BeginDisabled(is_greyed);
        const bool is_clicked =
            render_port_row(port, is_picked_or_fitting,
                            rejection.empty() ? port_explanation(processor, port) : rejection);
        name_last_item_for_ui_driver({"port", processor.name, "input", port.name}, rejection);
        if (is_picked_or_fitting && ImGui::IsItemHovered()) {
            is_mouse_over_picked_or_fitting_port_ = true;
        }
        // While a port is picked, a right click only lets it go.
        const bool is_right_clicked =
            !is_port_picked() && ImGui::IsItemClicked(ImGuiMouseButton_Right);
        ImGui::EndDisabled();
        if (is_clicked) {
            if (is_fitting) {
                graph_.connect(*waiting_output, input);
                forget_picked_port();
            } else if (is_picked) {
                forget_picked_port();
            } else {
                picked_port_ = input;
            }
        }
        if (is_right_clicked) {
            graph_.disconnect(input);
        }
    }

    // Why an output and an input can't be connected, said of the one of the two that the mouse is
    // over while the other is picked. Empty when they can.
    [[nodiscard]] std::string rejection_message(const OutputReference& output,
                                                const InputReference& input,
                                                PortDirection hovered_direction) const {
        const bool is_input_hovered = hovered_direction == PortDirection::Input;
        switch (graph_.connection_verdict(output, input)) {
            case ConnectionVerdict::Fits:
                return {};
            case ConnectionVerdict::UnknownPort:
                return "These two can't be connected.";
            case ConnectionVerdict::SameProcessor:
                return "A processor's output can't be connected to its own input.";
            case ConnectionVerdict::DifferentKind:
                return different_kind_message(output, input, is_input_hovered);
            case ConnectionVerdict::EventKindNotTaken:
                return event_kind_message(output, input, is_input_hovered);
            case ConnectionVerdict::AlreadyConnected:
                return is_input_hovered ? "This input is already connected to the picked output."
                                        : "This output is already connected to the picked input.";
            case ConnectionVerdict::InputTaken:
                return is_input_hovered
                           ? "This input takes one connection and already has one."
                           : "The picked input takes one connection and already has one.";
        }
        return {};
    }

    [[nodiscard]] std::string different_kind_message(const OutputReference& output,
                                                     const InputReference& input,
                                                     bool is_input_hovered) const {
        const PortDescription* output_port =
            graph_.find_port(output.processor, output.port, PortDirection::Output);
        const PortDescription* input_port =
            graph_.find_port(input.processor, input.port, PortDirection::Input);
        if (output_port == nullptr || input_port == nullptr) {
            return "These two can't be connected.";
        }
        if (is_input_hovered) {
            return std::format("This input takes {}, and the picked output gives {}.",
                               port_kind_name(input_port->kind), port_kind_name(output_port->kind));
        }
        return std::format("This output gives {}, and the picked input takes {}.",
                           port_kind_name(output_port->kind), port_kind_name(input_port->kind));
    }

    // Why an input doesn't take an output's events: the kinds it does take, the kind the output
    // gives, and the input's own description, which says the rule in words.
    [[nodiscard]] std::string event_kind_message(const OutputReference& output,
                                                 const InputReference& input,
                                                 bool is_input_hovered) const {
        const PortDescription* output_port =
            graph_.find_port(output.processor, output.port, PortDirection::Output);
        const PortDescription* input_port =
            graph_.find_port(input.processor, input.port, PortDirection::Input);
        if (output_port == nullptr || input_port == nullptr) {
            return "These two can't be connected.";
        }
        const std::string taken_kinds = event_kinds_text(input_port->takes_event_kinds);
        if (is_input_hovered) {
            return std::format("This input takes {}, and the picked output gives {} events.\n\n{}",
                               taken_kinds, output_port->event_kind, input_port->description);
        }
        return std::format(
            "This output gives {} events, and the picked input takes {}.\n\nThe picked input: {}",
            output_port->event_kind, taken_kinds, input_port->description);
    }

    // The kinds of event an input takes, as words for a sentence: "Spikes or LevelCrossings
    // events".
    [[nodiscard]] static std::string event_kinds_text(std::span<const char* const> kinds) {
        if (kinds.empty()) {
            return "no kind of event";
        }
        std::string text;
        for (std::size_t kind_index = 0; kind_index < kinds.size(); ++kind_index) {
            if (kind_index > 0) {
                text += kind_index + 1 == kinds.size() ? " or " : ", ";
            }
            text += kinds[kind_index];
        }
        return text + " events";
    }

    // What a port's tooltip says while the port can be clicked: what it is, its description, for
    // a port of events the kinds it gives or takes, and for a samples port the stream it carries.
    [[nodiscard]] std::string port_explanation(const GraphProcessor& processor,
                                               const PortDescription& port) const {
        std::string explanation =
            std::format("{}: {}\n\n{}", port.name, port_summary(port), port.description);
        const std::string_view event_kind = port.event_kind;
        if (!event_kind.empty()) {
            const auto* const kind = std::ranges::find_if(
                ALL_EVENT_KINDS, [event_kind](const EventKindDescription& described) {
                    return event_kind == described.name;
                });
            explanation += std::format("\n\nIt gives {} events. {}", event_kind,
                                       kind == ALL_EVENT_KINDS.end() ? "" : kind->description);
        }
        if (port.direction == PortDirection::Input && !port.takes_event_kinds.empty()) {
            explanation +=
                std::format("\n\nIt takes {}.", event_kinds_text(port.takes_event_kinds));
        }
        if (port.stream == PortStream::None) {
            return explanation;
        }
        const std::optional<GraphStream> stream =
            port.direction == PortDirection::Output
                ? graph_.stream_of_output({.processor = processor.id, .port = port.name})
                : graph_.stream_taken_by(processor.id);
        explanation += stream.has_value()
                           ? std::format("\n\nIt carries {} channels at {} Hz.",
                                         stream->channel_count, stream->rate)
                           : "\n\nIts stream isn't known yet: no processor that starts one is "
                             "connected before it.";
        return explanation;
    }

    // A dot in the colour of what the port carries, then its name, with a tooltip. The dot and the
    // name are one place to click. Returns whether it was clicked.
    static bool render_port_row(const PortDescription& port, bool is_marked,
                                const std::string& tooltip) {
        constexpr float dot_radius_in_line_heights = 0.7f;
        constexpr float half = 0.5f;
        constexpr float gap_after_dot = 8.0f;
        const float line_height = ImGui::GetTextLineHeight();
        const float dot_radius = line_height * dot_radius_in_line_heights;
        const float row_height = dot_radius * 2.0f;
        const ImVec2 row_top_left = ImGui::GetCursorScreenPos();

        ImGui::PushStyleColor(ImGuiCol_Header, marked_port_colour);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, hovered_row_colour);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, pressed_row_colour);
        ImGui::PushID(port.direction == PortDirection::Input ? "input" : "output");
        ImGui::PushID(port.name);
        const bool is_clicked = ImGui::Selectable("##port", is_marked, ImGuiSelectableFlags_None,
                                                  ImVec2(0.0f, row_height));
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopStyleColor(3);
        render_item_tooltip(tooltip.c_str());

        // Drawn after the row so that they lie over its highlight.
        constexpr float ring_thickness = 2.5f;
        const ImVec2 dot_centre(row_top_left.x + dot_radius, row_top_left.y + dot_radius);
        const float ring_radius = dot_radius - (ring_thickness * half);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddCircleFilled(dot_centre, ring_radius, ImGui::GetColorU32(block_colour));
        draw_list->AddCircle(dot_centre, ring_radius, ImGui::GetColorU32(kind_colour(port.kind)), 0,
                             ring_thickness);
        const ImVec4 name_colour =
            port.is_optional ? faint_colour : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        draw_list->AddText(ImVec2(row_top_left.x + row_height + gap_after_dot,
                                  row_top_left.y + ((row_height - line_height) * half)),
                           ImGui::GetColorU32(name_colour), port.name);
        return is_clicked;
    }

    // Where a port's line meets its block: the inputs spread along the block's top edge and the
    // outputs along its bottom edge, each in its order in the block.
    [[nodiscard]] std::optional<ImVec2> port_anchor(ProcessorId processor_id,
                                                    std::string_view port_name,
                                                    PortDirection direction) const {
        const GraphProcessor* processor = graph_.find_processor(processor_id);
        const auto rectangle =
            std::ranges::find(block_rectangles_, processor_id, &BlockRectangle::processor);
        if (processor == nullptr || rectangle == block_rectangles_.end()) {
            return std::nullopt;
        }
        int port_count = 0;
        int port_place = -1;
        for (const PortDescription& port : processor->type->ports) {
            if (port.direction != direction) {
                continue;
            }
            if (port_name == port.name) {
                port_place = port_count;
            }
            ++port_count;
        }
        if (port_place < 0) {
            return std::nullopt;
        }
        constexpr float half = 0.5f;
        const float width = rectangle->bottom_right.x - rectangle->top_left.x;
        const float along =
            (static_cast<float>(port_place) + half) / static_cast<float>(port_count);
        return ImVec2(rectangle->top_left.x + (width * along), direction == PortDirection::Input
                                                                   ? rectangle->top_left.y
                                                                   : rectangle->bottom_right.y);
    }

    // Called inside the cores' area, after its blocks: a line for each connection, leaving its
    // output downward and entering its input from above, and one between the picked port and the
    // mouse. The lines are drawn in a window of their own that lies over the cores' area and takes
    // no mouse: the blocks are windows inside that area, so the area's own drawing lies under
    // them, and the drawing in front of every window would cover a tooltip too.
    void render_connections() const {
        const ImVec2 area_top_left = ImGui::GetWindowPos();
        const ImVec2 area_size = ImGui::GetWindowSize();
        ImGui::SetNextWindowPos(area_top_left);
        ImGui::SetNextWindowSize(area_size);
        // ImGui's flags are signed ints by its API
        // NOLINTBEGIN(bugprone-signed-bitwise)
        const ImGuiWindowFlags line_window_flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove;
        // NOLINTEND(bugprone-signed-bitwise)
        ImGui::Begin("##connection_lines", nullptr, line_window_flags);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->PushClipRect(
            area_top_left, ImVec2(area_top_left.x + area_size.x, area_top_left.y + area_size.y),
            false);
        for (const GraphConnection& connection : graph_.connections) {
            const PortDescription* output_port = graph_.find_port(
                connection.output.processor, connection.output.port, PortDirection::Output);
            const auto at_output = port_anchor(connection.output.processor, connection.output.port,
                                               PortDirection::Output);
            const auto at_input = port_anchor(connection.input.processor, connection.input.port,
                                              PortDirection::Input);
            if (output_port != nullptr && at_output && at_input) {
                add_connection_line(draw_list, {.at_output = *at_output, .at_input = *at_input},
                                    ImGui::GetColorU32(kind_colour(output_port->kind)));
            }
        }
        if (const OutputReference* const output = picked_output()) {
            const PortDescription* output_port =
                graph_.find_port(output->processor, output->port, PortDirection::Output);
            const auto at_output =
                port_anchor(output->processor, output->port, PortDirection::Output);
            if (output_port != nullptr && at_output) {
                add_connection_line(draw_list,
                                    {.at_output = *at_output, .at_input = ImGui::GetMousePos()},
                                    ImGui::GetColorU32(kind_colour(output_port->kind)));
            }
        }
        if (const InputReference* const input = picked_input()) {
            const PortDescription* input_port =
                graph_.find_port(input->processor, input->port, PortDirection::Input);
            const auto at_input = port_anchor(input->processor, input->port, PortDirection::Input);
            if (input_port != nullptr && at_input) {
                add_connection_line(draw_list,
                                    {.at_output = ImGui::GetMousePos(), .at_input = *at_input},
                                    ImGui::GetColorU32(kind_colour(input_port->kind)));
            }
        }
        draw_list->PopClipRect();
        ImGui::End();
    }

    // The two ends of a connection's line on the screen: each at its port or, while that port
    // isn't clicked yet, at the mouse.
    struct LineEnds {
        ImVec2 at_output;
        ImVec2 at_input;
    };

    // A curve that leaves the output downward and reaches the input from above, so it bends into
    // an S when the input isn't below the output.
    static void add_connection_line(ImDrawList* draw_list, const LineEnds& ends, ImU32 colour) {
        constexpr float bend = 60.0f;
        constexpr float thickness = 2.0f;
        draw_list->AddBezierCubic(ends.at_output, ImVec2(ends.at_output.x, ends.at_output.y + bend),
                                  ImVec2(ends.at_input.x, ends.at_input.y - bend), ends.at_input,
                                  colour, thickness);
    }

    [[nodiscard]] static ImVec4 kind_colour(PortKind kind) {
        switch (kind) {
            case PortKind::Samples:
                return samples_colour;
            case PortKind::Events:
                return events_colour;
            case PortKind::State:
                return state_colour;
            case PortKind::Any:
                return any_kind_colour;
        }
        return any_kind_colour;
    }

    // Every core from 0 up to the highest one that holds a processor, those between that hold none
    // included, each as a box; while the pipeline is empty, up to the first core offered. While a
    // processor is dragged, from the panel or by its block, the next core after them is shown too,
    // as a place to drop it.
    [[nodiscard]] std::vector<int> shown_core_numbers() const {
        const std::vector<int>& offered = cpu_cores_.offered();
        int highest_core_number = -1;
        for (const GraphCore& core : graph_.cores) {
            if (!core.processors.empty()) {
                highest_core_number = std::max(highest_core_number, core.number);
            }
        }
        if (highest_core_number < 0) {
            highest_core_number = offered.empty() ? 0 : offered.front();
        }
        std::vector<int> core_numbers;
        for (int core_number = 0; core_number <= highest_core_number; ++core_number) {
            core_numbers.push_back(core_number);
        }
        if (is_any_processor_dragged()) {
            const auto next_free = std::ranges::find_if(offered, [&core_numbers](int core_number) {
                return !std::ranges::contains(core_numbers, core_number);
            });
            if (next_free != offered.end()) {
                core_numbers.push_back(*next_free);
            }
        }
        return core_numbers;
    }

    // Whether a processor is being dragged from the panel.
    [[nodiscard]] static bool is_new_processor_dragged() {
        const ImGuiPayload* payload = ImGui::GetDragDropPayload();
        return payload != nullptr && payload->IsDataType(PROCESSOR_TYPE_PAYLOAD);
    }

    // The processor of the graph that is being dragged by its block, if one is.
    [[nodiscard]] static std::optional<ProcessorId> moved_processor_id() {
        const ImGuiPayload* payload = ImGui::GetDragDropPayload();
        ProcessorId processor_id = 0;
        if (payload == nullptr || !payload->IsDataType(PROCESSOR_MOVE_PAYLOAD) ||
            !std::cmp_equal(payload->DataSize, sizeof(processor_id))) {
            return std::nullopt;
        }
        std::memcpy(&processor_id, payload->Data, sizeof(processor_id));
        return processor_id;
    }

    [[nodiscard]] static bool is_any_processor_dragged() {
        return is_new_processor_dragged() || moved_processor_id().has_value();
    }
};
