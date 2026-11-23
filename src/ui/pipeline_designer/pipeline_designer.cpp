#pragma once
#include <imgui.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include "../common/colours.cpp"
#include "../common/crosshair_cursor.cpp"
#include "../common/fonts.cpp"
#include "../common/icons.cpp"
#include "../common/remembered_values.cpp"
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
    PipelineDesigner(OpenedPipeline pipeline, RememberedValues& remembered)
        : remembered_(&remembered) {
        right_panel_width_ = remembered.number_or(right_panel_width_name, right_panel_width_);
        log_panel_height_ = remembered.number_or(log_panel_height_name, log_panel_height_);
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
        ImGui::SameLine();
        render_ports_toggle();
        ImGui::Spacing();
        core_under_drag_ = core_under_dragged_processor();
        processor_showing_ports_ = processor_to_show_ports();
        is_connection_being_drawn_ = is_port_picked();
        core_rectangles_.clear();
        block_rectangles_.clear();
        is_mouse_over_picked_or_fitting_port_ = false;
        processor_under_mouse_.reset();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            leave_on_escape();
        }
        hops_ = hops_of_graph();
        if (!hops_.empty() || is_connection_being_drawn_) {
            ImGui::Dummy(ImVec2(0.0f, height_of_hop_strip()));
        }
        for (const int core_number : shown_core_numbers()) {
            render_core(core_number);
            ImGui::SameLine(0.0f, padding);
        }
        forget_picked_port_on_click_elsewhere();
        render_ring_buttons();
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
    static constexpr ImVec2 block_padding{12.0f, 10.0f};
    // A port's line stands where its name ends, and two lines of one edge at least this far
    // apart.
    static constexpr float gap_after_port_name = 6.0f;
    static constexpr float gap_between_port_lines = 14.0f;
    // The circle at each end of a connection.
    static constexpr float terminal_radius = 4.0f;
    static constexpr float gap_between_rows = 40.0f;
    // A connection between two cores leaves its core by the top and reaches the other by the top.
    // Inside a box its line runs along a wall, in a lane of its own and clear of the blocks; above
    // the boxes it runs at a level of its own. Between the boxes it is a tube, the wider the more
    // slots its ring has: from the thinnest for a ring of one slot to three times that for the
    // largest ring the slider offers, so that a large ring is told from a small one at a glance.
    // A lane and a level are as wide as their tube and the room beside it.
    static constexpr float thinnest_tube = 6.0f;
    static constexpr float thickest_tube_in_thinnest = 3.0f;
    static constexpr float room_beside_tube = 6.0f;
    static constexpr float gap_from_wall_to_hop_lanes = 4.0f;
    static constexpr float gap_from_hop_lanes_to_blocks = 6.0f;
    // How far under its output, and over its input, a hop's line turns towards its lane: the
    // line of the first lane, and each further lane's by a step more, so that no two lines of
    // one core turn at one height and lie on each other.
    static constexpr float hop_turn_from_port = 4.0f;
    static constexpr float step_between_hop_turns = 3.0f;
    // A ring is a pill above its reading core's box, on its line: a ring icon and its number of
    // slots. The rings into one core stand one above the other, each in a row of its own, since
    // their lanes are closer than a pill is wide.
    static constexpr float ring_pill_height = 18.0f;
    static constexpr float gap_between_ring_pills = 4.0f;
    static constexpr float gap_from_ring_pills_to_box = 6.0f;
    // The engine takes a ring whose number of slots is a power of two, and no other. The slider
    // offers those from 1 to 16384.
    static constexpr int highest_power_of_two_of_ring_slots = 14;
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

    // Which of the ways kept for hops one hop takes, each counted from 0.
    struct HopLanes {
        int leaving = 0;   // up the right wall of the writing core's box, from the wall inward
        int arriving = 0;  // down the left wall of the reading core's box, from the wall inward
        int level = 0;     // above the boxes, from the boxes upward
    };

    // What crosses from one core to another: an output, and a core that reads it. Every
    // connection from that output into that core goes this way, through one ring; a state value
    // crosses without a ring. Each hop has a line of its own all the way, two hops of one output
    // too, so that two rings are never reached by one line.
    struct Hop {
        OutputReference output;
        int writer_core = 0;
        int reader_core = 0;
        PortKind kind = PortKind::Any;
        std::vector<InputReference> inputs;
        HopLanes lanes;
    };

    PipelineGraph graph_;
    CpuCores cpu_cores_;
    // What the app keeps between two sessions; null for the example pipeline of a picture.
    RememberedValues* remembered_ = nullptr;
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
    // What crosses between cores in this frame.
    std::vector<Hop> hops_;
    // The processor whose remove icon was clicked in this frame, if any.
    std::optional<ProcessorId> processor_to_remove_;
    // The processor whose block was clicked last, whose options the panel at the right edits.
    std::optional<ProcessorId> selected_processor_;
    // Whether, in this frame, the mouse is over the picked port or over one that fits it: the
    // ports whose row handles a click itself while a port is picked.
    bool is_mouse_over_picked_or_fitting_port_ = false;
    // The processor over whose block the mouse is in this frame, if any.
    std::optional<ProcessorId> processor_under_mouse_;
    // The processor whose block shows its ports in this frame, unless every block does.
    std::optional<ProcessorId> processor_showing_ports_;
    // Whether a port was picked when this frame began. Every block shows its ports until the
    // connection is made or let go, so that its start stays in sight and its end can be chosen.
    bool is_connection_being_drawn_ = false;
    // Whether every block shows its ports, which the user switches above the cores.
    bool are_all_ports_shown_ = false;

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
        remembered_->remember_number(right_panel_width_name, right_panel_width_);
        remembered_->remember_number(log_panel_height_name, log_panel_height_);
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

    // One line above the cores saying which are isolated, with the others named behind its info
    // icon.
    void render_isolation_note() const {
        const std::vector<int> isolated = cpu_cores_.isolated();
        const std::vector<int> not_isolated = cpu_cores_.not_isolated();
        if (isolated.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, warning_colour);
            ImGui::TextUnformatted("No core of this machine is isolated");
            ImGui::PopStyleColor();
            render_info_icon(
                "The operating system runs on every core of this machine. It can pause a "
                "processor at any moment, so Falcon's timing can't be trusted.\n\n"
                "Cores are kept free for Falcon with the isolcpus boot option, set when the "
                "machine starts.");
        } else {
            const std::string note = std::format("Isolated cores: {}", core_list_text(isolated));
            ImGui::TextUnformatted(note.c_str());
            const std::string about_other_cores =
                not_isolated.empty()
                    ? "Every core of this machine is isolated."
                    : std::format(
                          "The operating system runs on the other cores ({}). It can "
                          "pause a processor there at any moment.\n\n"
                          "Everything that must be fast belongs on an isolated core. "
                          "Slow work, like a recorder, can go on the others.",
                          core_list_text(not_isolated));
            const std::string explanation = std::format(
                "The operating system stays off these cores, so a processor on them "
                "runs without being paused.\n\n{}",
                about_other_cores);
            render_info_icon(explanation.c_str());
        }
    }

    // The button beside that line which switches between the two ways a block shows its ports.
    void render_ports_toggle() {
        ImGui::PushStyleColor(ImGuiCol_Button, input_colour);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, input_hovered_colour);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, input_active_colour);
        if (ImGui::SmallButton(are_all_ports_shown_ ? "Hide ports" : "Show ports")) {
            are_all_ports_shown_ = !are_all_ports_shown_;
        }
        ImGui::PopStyleColor(3);
        name_last_item_for_ui_driver({"ports_toggle"});
        render_item_tooltip(are_all_ports_shown_
                                ? "Show a block's ports only while the mouse is over it."
                                : "Show the ports of every block.");
    }

    [[nodiscard]] static ScreenRectangle rectangle_of_this_window() {
        const ImVec2 top_left = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        return {
            .top_left = top_left,
            .bottom_right = ImVec2(top_left.x + size.x, top_left.y + size.y),
        };
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

    // A core's box is as wide as its rows of processors and the lanes of its hops need, at least
    // one block wide, and as tall as what it holds.
    void render_core(int core_number) {
        const bool is_isolated = cpu_cores_.is_isolated(core_number);
        const CoreLayout layout = layout_of_core(graph_, core_number);
        const float arriving_lanes_width = width_of_hop_lanes(room_of_arriving_lanes(core_number));
        const float box_width = (std::max(1.0f, layout.width_in_blocks) * (block_width + padding)) +
                                padding + arriving_lanes_width +
                                width_of_hop_lanes(room_of_leaving_lanes(core_number));

        ImGui::PushID(core_number);
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
                              is_isolated ? core_colour : not_isolated_core_colour);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, core_rounding);
        // A box whose height isn't reckoned sizes itself, by what it held a frame before.
        const std::optional<float> box_height = height_of_core_box(layout);
        // ImGui's flags are signed ints by its API
        // NOLINTBEGIN(bugprone-signed-bitwise)
        ImGuiChildFlags box_flags =
            ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding;
        if (!box_height.has_value()) {
            box_flags |= ImGuiChildFlags_AutoResizeY;
        }
        const ImGuiWindowFlags box_window_flags =
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        // NOLINTEND(bugprone-signed-bitwise)
        ImGui::BeginChild("Core", ImVec2(box_width, box_height.value_or(0.0f)), box_flags,
                          box_window_flags);
        // The lanes of the lines that arrive lie along the left wall, left of all the box holds.
        if (arriving_lanes_width > 0.0f) {
            ImGui::Indent(arriving_lanes_width);
        }
        const std::string title = std::format("Core {}", core_number);
        push_bold_font();
        ImGui::TextUnformatted(title.c_str());
        ImGui::PopFont();
        if (!is_isolated) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, warning_colour);
            ImGui::TextUnformatted("not isolated");
            ImGui::PopStyleColor();
            render_info_icon(
                "The operating system also runs on this core. It can pause a processor here at "
                "any moment.\n\n"
                "Everything that must be fast belongs on an isolated core. Slow work, like a "
                "recorder, is fine here.");
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

    // The height of a core's box that holds processors, reckoned for the reason given at
    // height_of_block: a block that shows or hides its ports moves the blocks under it, and a box
    // sized a frame late would cut the lowest of them off meanwhile. It follows what render_core
    // and render_rows draw: the title, two spacings, and the rows with a gap between two.
    [[nodiscard]] std::optional<float> height_of_core_box(const CoreLayout& layout) const {
        if (layout.rows.empty()) {
            return std::nullopt;
        }
        constexpr float spacings_under_title = 3.0f;
        const float spacing = ImGui::GetStyle().ItemSpacing.y;
        float height =
            (2.0f * padding) + ImGui::GetTextLineHeight() + (spacings_under_title * spacing);
        bool is_top_row = true;
        for (const std::vector<PlacedProcessor>& row : layout.rows) {
            if (!is_top_row) {
                height += gap_between_rows + (2.0f * spacing);
            }
            is_top_row = false;
            float tallest_block = 0.0f;
            for (const PlacedProcessor& placed : row) {
                const GraphProcessor* processor = graph_.find_processor(placed.processor);
                if (processor != nullptr) {
                    tallest_block =
                        std::max(tallest_block,
                                 height_of_block(*processor, are_ports_shown_of(placed.processor)));
                }
            }
            height += tallest_block;
        }
        return height;
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

    // How a block's border is drawn, which says whether the block is selected or has a problem.
    struct BlockBorder {
        ImVec4 colour;
        float thickness = 1.0f;
    };

    [[nodiscard]] BlockBorder border_of_block(const GraphProcessor& processor) const {
        constexpr float plain_thickness = 1.0f;
        constexpr float marked_thickness = 2.0f;
        if (has_problem(processor)) {
            return {.colour = problem_border_colour(), .thickness = marked_thickness};
        }
        if (selected_processor_ == processor.id) {
            return {.colour = selected_block_colour, .thickness = marked_thickness};
        }
        return {.colour = block_border_colour, .thickness = plain_thickness};
    }

    // A block: its type and its name and, while it shows its ports, a row for each, the inputs
    // on top and the outputs at the bottom, as data flows. It shows them while the mouse is over
    // it, or while every block does. A block that grows by its rows moves the blocks under it,
    // so the gap between two blocks stays what it is.
    void render_block(const GraphProcessor& processor) {
        const BlockBorder border = border_of_block(processor);
        ImGui::PushID(static_cast<int>(processor.id));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, block_colour);
        ImGui::PushStyleColor(ImGuiCol_Border, border.colour);
        ImGui::PushStyleColor(ImGuiCol_Separator, block_separator_colour);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, block_padding);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, block_rounding);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, border.thickness);
        const bool has_rows = are_ports_shown_of(processor.id);
        // ImGui's flags are signed ints by its API
        // NOLINTBEGIN(bugprone-signed-bitwise)
        const ImGuiChildFlags block_flags =
            ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding;
        const ImGuiWindowFlags block_window_flags =
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        // NOLINTEND(bugprone-signed-bitwise)
        ImGui::BeginChild("Processor", ImVec2(block_width, height_of_block(processor, has_rows)),
                          block_flags, block_window_flags);
        // Also while something else is held by the mouse or typed in, e.g. an option's field.
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
            processor_under_mouse_ = processor.id;
        }
        const ScreenRectangle block = rectangle_of_this_window();
        if (has_rows && has_port(*processor.type, PortDirection::Input)) {
            render_ports(processor, PortDirection::Input, block);
            ImGui::Separator();
        }
        render_block_handle(processor);
        render_block_stream(processor);
        if (has_rows && has_port(*processor.type, PortDirection::Output)) {
            ImGui::Separator();
            render_ports(processor, PortDirection::Output, block);
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

    // A block's height, reckoned here and not left to ImGui, which sizes a window by what it
    // held a frame before: a block that showed or hid its rows would be cut off, or too tall, for
    // one frame. It follows what render_block draws: the type and the name, the line of the
    // stream, and for the inputs and for the outputs their rows and a separator.
    [[nodiscard]] static float height_of_block(const GraphProcessor& processor, bool has_rows) {
        const float line_height = ImGui::GetTextLineHeight();
        const float spacing = ImGui::GetStyle().ItemSpacing.y;
        float height = (2.0f * block_padding.y) + (2.0f * line_height) + spacing;
        if (processor.stream.has_value()) {
            height += spacing + line_height;
        }
        if (!has_rows) {
            return height;
        }
        for (const PortDirection direction : {PortDirection::Input, PortDirection::Output}) {
            const auto row_count =
                std::ranges::count(processor.type->ports, direction, &PortDescription::direction);
            if (row_count > 0) {
                height += (static_cast<float>(row_count) * (port_row_height() + spacing)) + spacing;
            }
        }
        return height;
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

        // The type and the name are drawn over the handle, which is as high as the two, and not
        // beyond it, where the remove icon is. The type comes first: it says what the block
        // does, where the name says which one it is.
        ImGui::SetCursorPos(handle_position);
        ImGui::PushClipRect(handle_top_left, handle_bottom_right, true);
        push_bold_font();
        ImGui::TextUnformatted(processor.type->name);
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, faint_colour);
        ImGui::TextUnformatted(processor.name.c_str());
        ImGui::PopStyleColor();
        ImGui::PopClipRect();
    }

    // The stream a processor starts, in short, as a line under the block's name. A click on it
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
        ImGui::TextUnformatted(processor->type->name);
        // At the right end of what the panel has room for, which is less when it has a scrollbar.
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - icon_size);
        if (render_cross_button("close", icon_size, "Close this panel.")) {
            selected_processor_.reset();
        }
        name_last_item_for_ui_driver({"options_panel", "close"});
        ImGui::PushStyleColor(ImGuiCol_Text, faint_colour);
        ImGui::TextUnformatted(processor->name.c_str());
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

    // One row of a block's ports: the port, its name as the row shows it, and where its line
    // stands, counted from the block's left edge.
    struct PortRowPlace {
        const PortDescription* port = nullptr;
        std::string shown_name;
        float name_width = 0.0f;
        float line_x = 0.0f;
    };

    // The rows of a processor's inputs, or of its outputs, from top to bottom. A port's line
    // leaves its row where its name ends and runs straight to the block's edge, past the rows
    // between the two, which therefore hold the shorter names: the inputs, whose lines run up,
    // stand with the shortest name on top, and the outputs, whose lines run down, with the
    // shortest name at the bottom. Two lines keep a gap, so where two names are about as long,
    // the line of the longer one, and its circle with it, stands further right than its name
    // ends. A connection meets the block's edge at its port's line, whether the block shows its
    // ports or not.
    [[nodiscard]] static std::vector<PortRowPlace> rows_of_ports(const ProcessorDescription& type,
                                                                 PortDirection direction) {
        std::vector<PortRowPlace> rows;
        for (const PortDescription& port : type.ports) {
            if (port.direction == direction) {
                PortRowPlace row;
                row.port = &port;
                rows.push_back(row);
            }
        }
        if (rows.empty()) {
            return rows;
        }
        const float room_for_lines = gap_after_port_name + (2.0f * terminal_radius) +
                                     (static_cast<float>(rows.size() - 1) * gap_between_port_lines);
        const float room_for_a_name = block_width - (2.0f * block_padding.x) - room_for_lines;
        for (PortRowPlace& row : rows) {
            row.shown_name = text_cut_to_width(row.port->name, room_for_a_name);
            row.name_width = ImGui::CalcTextSize(row.shown_name.c_str()).x;
        }
        std::ranges::stable_sort(rows, std::ranges::less{}, &PortRowPlace::name_width);
        std::optional<float> line_of_shorter_name;
        for (PortRowPlace& row : rows) {
            row.line_x = block_padding.x + row.name_width + gap_after_port_name + terminal_radius;
            if (line_of_shorter_name.has_value()) {
                row.line_x = std::max(row.line_x, *line_of_shorter_name + gap_between_port_lines);
            }
            line_of_shorter_name = row.line_x;
        }
        if (direction == PortDirection::Output) {
            std::ranges::reverse(rows);
        }
        return rows;
    }

    [[nodiscard]] bool has_connection(ProcessorId processor_id, const PortDescription& port) const {
        return std::ranges::any_of(graph_.connections, [processor_id,
                                                        &port](const GraphConnection& connection) {
            if (port.direction == PortDirection::Output) {
                return connection.output.processor == processor_id &&
                       connection.output.port == port.name;
            }
            return connection.input.processor == processor_id && connection.input.port == port.name;
        });
    }

    // Where a port's line starts, at the port's circle, and its colour.
    struct PortLineStart {
        ImVec2 at_circle;
        ImU32 colour = 0;
    };

    // What drawing a port's row gives: whether the row was clicked, and the start of its line.
    struct PortRow {
        bool is_clicked = false;
        PortLineStart line_start;
    };

    // A port's line inside its block: from the port's circle straight to the edge.
    struct PortPath {
        PortLineStart start;
        ImVec2 at_edge;
        bool is_connected = false;
    };

    // Called inside a block: the rows of its processor's inputs, or of its outputs, each with its
    // line to the block's edge.
    void render_ports(const GraphProcessor& processor, PortDirection direction,
                      const ScreenRectangle& block) {
        const bool is_input = direction == PortDirection::Input;
        std::vector<PortPath> paths;
        for (const PortRowPlace& row : rows_of_ports(*processor.type, direction)) {
            const bool is_connected = has_connection(processor.id, *row.port);
            paths.push_back({
                .start = is_input ? render_input(processor, row, is_connected)
                                  : render_output(processor, row, is_connected),
                .at_edge = ImVec2(block.top_left.x + row.line_x,
                                  is_input ? block.top_left.y : block.bottom_right.y),
                .is_connected = is_connected,
            });
        }
        // Drawn once every row is: a row's highlight would cover the lines that pass through it.
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        for (const PortPath& path : paths) {
            draw_port_path(draw_list, path);
        }
    }

    // The circle of a port without a connection: every port has a circle, which is filled once
    // the port is connected.
    static void add_empty_terminal(ImDrawList* draw_list, ImVec2 centre, ImU32 colour) {
        constexpr float ring_thickness = 1.5f;
        constexpr float half = 0.5f;
        draw_list->AddCircleFilled(centre, terminal_radius, ImGui::GetColorU32(block_colour));
        draw_list->AddCircle(centre, terminal_radius - (ring_thickness * half), colour, 0,
                             ring_thickness);
    }

    // The colour of what a port carries, fainter while the port has no connection.
    [[nodiscard]] static ImVec4 line_colour_of_port(const PortDescription& port,
                                                    bool is_connected) {
        constexpr float faintness_without_connection = 0.55f;
        ImVec4 colour = kind_colour(port.kind);
        if (!is_connected) {
            colour.w *= faintness_without_connection;
        }
        return colour;
    }

    // A port's line runs straight from its circle to the edge. That of a connected port is solid,
    // as its connections' lines are, with a filled circle; that of a port without a connection
    // is dashed, with an empty one.
    static void draw_port_path(ImDrawList* draw_list, const PortPath& path) {
        constexpr float thickness = 2.0f;
        const ImVec2 circle = path.start.at_circle;
        if (path.is_connected) {
            draw_list->AddLine(circle, path.at_edge, path.start.colour, thickness);
            draw_list->AddCircleFilled(circle, terminal_radius, path.start.colour);
            return;
        }
        add_dashed_line(draw_list, circle, path.at_edge, path.start.colour);
        add_empty_terminal(draw_list, circle, path.start.colour);
    }

    // A click picks the output as one end of a connection, and a second click lets it go. While a
    // port is picked, the outputs that fit it are marked and every other port can't be clicked.
    PortLineStart render_output(const GraphProcessor& processor, const PortRowPlace& place,
                                bool is_connected) {
        const PortDescription& port = *place.port;
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
        const PortRow row = render_port_row(place, is_picked_or_fitting, rejection, is_connected);
        name_last_item_for_ui_driver({"port", processor.name, "output", port.name}, rejection);
        if (is_picked_or_fitting && ImGui::IsItemHovered()) {
            is_mouse_over_picked_or_fitting_port_ = true;
        }
        ImGui::EndDisabled();
        if (row.is_clicked) {
            if (is_fitting) {
                graph_.connect(output, *waiting_input);
                forget_picked_port();
            } else if (is_picked) {
                forget_picked_port();
            } else {
                picked_port_ = output;
            }
        }
        return row.line_start;
    }

    // A click picks the input as one end of a connection, and a second click lets it go. While a
    // port is picked, the inputs that fit it are marked and every other port can't be clicked.
    PortLineStart render_input(const GraphProcessor& processor, const PortRowPlace& place,
                               bool is_connected) {
        const PortDescription& port = *place.port;
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
        const PortRow row = render_port_row(place, is_picked_or_fitting, rejection, is_connected);
        name_last_item_for_ui_driver({"port", processor.name, "input", port.name}, rejection);
        if (is_picked_or_fitting && ImGui::IsItemHovered()) {
            is_mouse_over_picked_or_fitting_port_ = true;
        }
        // While a port is picked, a right click only lets it go.
        const bool is_right_clicked =
            !is_port_picked() && ImGui::IsItemClicked(ImGuiMouseButton_Right);
        ImGui::EndDisabled();
        if (row.is_clicked) {
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
        return row.line_start;
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

    // A port's name. The whole row is one place to click, over which the mouse's cursor is a
    // crosshair. Its only tooltip says why the port can't be clicked, while it can't: what a
    // port is, the panel of processors explains. The port's line, which starts beside the name,
    // is drawn by render_ports once every row is.
    static PortRow render_port_row(const PortRowPlace& place, bool is_marked,
                                   const std::string& why_not_clickable, bool is_connected) {
        constexpr float half = 0.5f;
        const PortDescription& port = *place.port;
        const float line_height = ImGui::GetTextLineHeight();
        const float row_height = port_row_height();
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
        if (!why_not_clickable.empty()) {
            render_item_tooltip(why_not_clickable.c_str());
        }
        if (ImGui::IsItemHovered()) {
            crosshair_cursor_request().ask();
        }

        // Drawn after the row so that it lies over the row's highlight.
        const ImVec4 name_colour =
            port.is_optional ? faint_colour : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(row_top_left.x, row_top_left.y + ((row_height - line_height) * half)),
            ImGui::GetColorU32(name_colour), place.shown_name.c_str());
        // The colours are taken here, where a port that can't be clicked is still greyed.
        const PortLineStart line_start{
            .at_circle = ImVec2(row_top_left.x - block_padding.x + place.line_x,
                                row_top_left.y + (row_height * half)),
            .colour = ImGui::GetColorU32(line_colour_of_port(port, is_connected)),
        };
        return {.is_clicked = is_clicked, .line_start = line_start};
    }

    [[nodiscard]] static float port_row_height() {
        constexpr float row_height_in_line_heights = 1.4f;
        return ImGui::GetTextLineHeight() * row_height_in_line_heights;
    }

    // Called inside the cores' area, before its blocks are drawn. The processor whose block shows
    // its ports in this frame: the one at the mouse. None while every block shows its ports, nor
    // while a processor is dragged, when a block that grew would move the places to drop it.
    [[nodiscard]] std::optional<ProcessorId> processor_to_show_ports() const {
        if (are_all_ports_shown_ || is_any_processor_dragged()) {
            return std::nullopt;
        }
        return processor_at_mouse();
    }

    // The processor whose block the mouse was over in the frame before. The block that shows its
    // ports counts as long as the mouse is inside it, whatever lies over it there: a block that
    // grows may bring the area a scrollbar under the mouse, and a block that closed for that
    // would take the scrollbar away and open again, without end.
    [[nodiscard]] std::optional<ProcessorId> processor_at_mouse() const {
        if (processor_under_mouse_.has_value() || !processor_showing_ports_.has_value()) {
            return processor_under_mouse_;
        }
        const auto block = std::ranges::find(block_rectangles_, *processor_showing_ports_,
                                             &BlockRectangle::processor);
        const ScreenRectangle area = rectangle_of_this_window();
        const bool is_mouse_inside_block =
            block != block_rectangles_.end() &&
            ImGui::IsMouseHoveringRect(area.top_left, area.bottom_right, false) &&
            ImGui::IsMouseHoveringRect(block->top_left, block->bottom_right, false);
        return is_mouse_inside_block ? processor_showing_ports_ : std::nullopt;
    }

    // The hops of the graph, in the order of its connections.
    [[nodiscard]] std::vector<Hop> hops_of_graph() const {
        std::vector<Hop> hops;
        for (const GraphConnection& connection : graph_.connections) {
            const GraphProcessor* writer = graph_.find_processor(connection.output.processor);
            const GraphProcessor* reader = graph_.find_processor(connection.input.processor);
            const PortDescription* output_port = graph_.find_port(
                connection.output.processor, connection.output.port, PortDirection::Output);
            if (writer == nullptr || reader == nullptr || output_port == nullptr ||
                writer->core_number == reader->core_number) {
                continue;
            }
            auto hop = std::ranges::find_if(hops, [&connection, reader](const Hop& candidate) {
                return candidate.output == connection.output &&
                       candidate.reader_core == reader->core_number;
            });
            if (hop == hops.end()) {
                const HopCores cores{
                    .writer_core = writer->core_number,
                    .reader_core = reader->core_number,
                };
                hops.push_back({
                    .output = connection.output,
                    .writer_core = cores.writer_core,
                    .reader_core = cores.reader_core,
                    .kind = output_port->kind,
                    .inputs = {},
                    .lanes = lanes_of_next_hop(hops, cores),
                });
                hop = std::prev(hops.end());
            }
            hop->inputs.push_back(connection.input);
        }
        return hops;
    }

    // The two cores of a hop.
    struct HopCores {
        int writer_core = 0;
        int reader_core = 0;
    };

    // The ways that a hop takes which isn't among these hops yet: beside those taken.
    [[nodiscard]] static HopLanes lanes_of_next_hop(const std::vector<Hop>& hops,
                                                    const HopCores& cores) {
        return {
            .leaving = leaving_lane_count(hops, cores.writer_core),
            .arriving = arriving_lane_count(hops, cores.reader_core),
            .level = static_cast<int>(hops.size()),
        };
    }

    // How many lanes the lines that leave a core take: one for each hop out of it.
    [[nodiscard]] static int leaving_lane_count(const std::vector<Hop>& hops, int core_number) {
        return static_cast<int>(std::ranges::count(hops, core_number, &Hop::writer_core));
    }

    // How many lanes the lines that arrive at a core take: one for each hop into it.
    [[nodiscard]] static int arriving_lane_count(const std::vector<Hop>& hops, int core_number) {
        return static_cast<int>(std::ranges::count(hops, core_number, &Hop::reader_core));
    }

    // The power of two that a ring's number of slots is, or the one under it, among those the
    // slider offers.
    [[nodiscard]] static int power_of_two_of_ring_slots(int slot_count) {
        const int power_of_two = std::bit_width(static_cast<unsigned>(std::max(1, slot_count))) - 1;
        return std::min(power_of_two, highest_power_of_two_of_ring_slots);
    }

    // How wide a hop's tube is between the boxes: by its ring's number of slots, evenly with
    // each doubling. A hop without a ring has the thinnest.
    [[nodiscard]] float tube_width_of(const Hop& hop) const {
        if (!has_ring(hop)) {
            return thinnest_tube;
        }
        const float part_of_largest =
            static_cast<float>(power_of_two_of_ring_slots(slot_count_of_ring(hop))) /
            static_cast<float>(highest_power_of_two_of_ring_slots);
        return thinnest_tube * (1.0f + ((thickest_tube_in_thinnest - 1.0f) * part_of_largest));
    }

    // The room that a hop takes among the lanes along a wall and among the levels above the
    // boxes.
    [[nodiscard]] float room_of_hop(const Hop& hop) const {
        return tube_width_of(hop) + room_beside_tube;
    }

    // The room of the hops that leave the same core as this one in lanes nearer the wall.
    [[nodiscard]] float room_of_leaving_lanes_before(const Hop& hop) const {
        float room = 0.0f;
        for (const Hop& other : hops_) {
            if (other.writer_core == hop.writer_core && other.lanes.leaving < hop.lanes.leaving) {
                room += room_of_hop(other);
            }
        }
        return room;
    }

    // The room of the hops that arrive at the same core as this one in lanes nearer the wall.
    [[nodiscard]] float room_of_arriving_lanes_before(const Hop& hop) const {
        float room = 0.0f;
        for (const Hop& other : hops_) {
            if (other.reader_core == hop.reader_core && other.lanes.arriving < hop.lanes.arriving) {
                room += room_of_hop(other);
            }
        }
        return room;
    }

    // The room of the hops that run at levels under this one's.
    [[nodiscard]] float room_of_levels_under(const Hop& hop) const {
        float room = 0.0f;
        for (const Hop& other : hops_) {
            if (other.lanes.level < hop.lanes.level) {
                room += room_of_hop(other);
            }
        }
        return room;
    }

    [[nodiscard]] float room_of_leaving_lanes(int core_number) const {
        float room = 0.0f;
        for (const Hop& hop : hops_) {
            if (hop.writer_core == core_number) {
                room += room_of_hop(hop);
            }
        }
        return room;
    }

    [[nodiscard]] float room_of_arriving_lanes(int core_number) const {
        float room = 0.0f;
        for (const Hop& hop : hops_) {
            if (hop.reader_core == core_number) {
                room += room_of_hop(hop);
            }
        }
        return room;
    }

    // What a box keeps free along one wall for lanes that take so much room.
    [[nodiscard]] static float width_of_hop_lanes(float room_of_lanes) {
        return room_of_lanes > 0.0f ? room_of_lanes + gap_from_hop_lanes_to_blocks : 0.0f;
    }

    // The room that a hop's ring takes in the pile of rings over its reading core's box; none
    // for a hop without a ring.
    [[nodiscard]] static float room_of_ring(const Hop& hop) {
        return has_ring(hop) ? ring_pill_height + gap_between_ring_pills : 0.0f;
    }

    // How high the rings pile under a hop's own: those of the hops into the same core whose lanes
    // lie nearer the wall.
    [[nodiscard]] float height_of_rings_under(const Hop& hop) const {
        float height = 0.0f;
        for (const Hop& other : hops_) {
            if (other.reader_core == hop.reader_core && other.lanes.arriving < hop.lanes.arriving) {
                height += room_of_ring(other);
            }
        }
        return height;
    }

    // How far above the boxes the lowest level lies: over the highest pile of rings, and over
    // one ring more while a connection is drawn, for its ring.
    [[nodiscard]] float lowest_hop_level() const {
        float highest_pile = 0.0f;
        for (const Hop& hop : hops_) {
            highest_pile = std::max(highest_pile, height_of_rings_under(hop) + room_of_ring(hop));
        }
        if (is_connection_being_drawn_) {
            highest_pile += ring_pill_height + gap_between_ring_pills;
        }
        return gap_from_ring_pills_to_box + highest_pile + gap_between_ring_pills;
    }

    // The strip above the boxes in which the hops run, with the widest level to spare for the
    // hop of a connection that is being drawn.
    [[nodiscard]] float height_of_hop_strip() const {
        float room_of_levels = 0.0f;
        for (const Hop& hop : hops_) {
            room_of_levels += room_of_hop(hop);
        }
        if (is_connection_being_drawn_) {
            room_of_levels += (thinnest_tube * thickest_tube_in_thinnest) + room_beside_tube;
        }
        return lowest_hop_level() + room_of_levels;
    }

    // Where a hop's line runs outside the blocks, on the screen.
    struct HopPlace {
        float leaving_x = 0.0f;      // its lane up the writing core's box
        float arriving_x = 0.0f;     // its lane down the reading core's box
        float level_y = 0.0f;        // its run above the boxes
        float leaving_turn = 0.0f;   // how far under its output it turns towards its lane
        float arriving_turn = 0.0f;  // how far over an input it turns towards the input
        float writer_box_top = 0.0f;
        float reader_box_top = 0.0f;
        float tube_width = 0.0f;  // of its line between the two boxes
        ImVec2 at_ring;           // where its ring is, on its lane above the reading core's box
    };

    // None while one of the hop's two cores has no box.
    [[nodiscard]] std::optional<HopPlace> place_of_hop(const Hop& hop) const {
        const auto writer_box =
            std::ranges::find(core_rectangles_, hop.writer_core, &CoreRectangle::core_number);
        const auto reader_box =
            std::ranges::find(core_rectangles_, hop.reader_core, &CoreRectangle::core_number);
        if (writer_box == core_rectangles_.end() || reader_box == core_rectangles_.end()) {
            return std::nullopt;
        }
        constexpr float half = 0.5f;
        const float half_room = room_of_hop(hop) * half;
        const float arriving_x = reader_box->top_left.x + gap_from_wall_to_hop_lanes +
                                 room_of_arriving_lanes_before(hop) + half_room;
        return HopPlace{
            .leaving_x = writer_box->bottom_right.x - gap_from_wall_to_hop_lanes -
                         room_of_leaving_lanes_before(hop) - half_room,
            .arriving_x = arriving_x,
            .level_y =
                reader_box->top_left.y - lowest_hop_level() - room_of_levels_under(hop) - half_room,
            .leaving_turn = hop_turn_from_port +
                            (static_cast<float>(hop.lanes.leaving) * step_between_hop_turns),
            .arriving_turn = hop_turn_from_port +
                             (static_cast<float>(hop.lanes.arriving) * step_between_hop_turns),
            .writer_box_top = writer_box->top_left.y,
            .reader_box_top = reader_box->top_left.y,
            .tube_width = tube_width_of(hop),
            .at_ring =
                ImVec2(arriving_x, reader_box->top_left.y - gap_from_ring_pills_to_box -
                                       height_of_rings_under(hop) - (ring_pill_height * half)),
        };
    }

    [[nodiscard]] static bool has_ring(const Hop& hop) { return hop.kind != PortKind::State; }

    // The number of slots of a hop's ring: what the user set, or what such a ring has otherwise.
    [[nodiscard]] int slot_count_of_ring(const Hop& hop) const {
        const GraphProcessor* writer = graph_.find_processor(hop.output.processor);
        if (writer == nullptr) {
            return usual_ring_slot_count(is_core_realtime(hop.reader_core));
        }
        const RingPlace ring{
            .writer = *writer,
            .output_name = hop.output.port,
            .reader_core_number = hop.reader_core,
        };
        return ring_slot_count_of(graph_, ring);
    }

    [[nodiscard]] bool is_core_realtime(int core_number) const {
        const auto core = std::ranges::find(graph_.cores, core_number, &GraphCore::number);
        return core == graph_.cores.end() || core->is_realtime;
    }

    // Every connection of a ring carries the ring's number of slots, or none where that is the
    // number such a ring has anyway, as a pipeline read from its file has it.
    void set_slot_count_of_ring(const Hop& hop, int slot_count) {
        const bool is_usual =
            slot_count == usual_ring_slot_count(is_core_realtime(hop.reader_core));
        for (GraphConnection& connection : graph_.connections) {
            if (connection.output == hop.output &&
                reader_core_of(graph_, connection) == hop.reader_core) {
                connection.ring_slot_count =
                    is_usual ? std::nullopt : std::optional<int>(slot_count);
            }
        }
    }

    // Called inside the cores' area, after its boxes: each ring is a place to click, which opens a
    // box with a slider for the ring's number of slots, and with what a ring is behind an info
    // icon. The rings themselves are drawn with the connections' lines, in front of the boxes.
    void render_ring_buttons() {
        constexpr float slider_width = 220.0f;
        int hop_number = 0;
        for (const Hop& hop : hops_) {
            ++hop_number;
            const std::optional<HopPlace> place = place_of_hop(hop);
            const GraphProcessor* writer = graph_.find_processor(hop.output.processor);
            if (!has_ring(hop) || !place.has_value() || writer == nullptr) {
                continue;
            }
            const int slot_count = slot_count_of_ring(hop);
            const std::string slot_count_text = std::to_string(slot_count);
            ImGui::PushID(hop_number);
            const ScreenRectangle pill = pill_of_ring(place->at_ring, slot_count);
            ImGui::SetCursorScreenPos(pill.top_left);
            const bool is_clicked =
                ImGui::InvisibleButton("##ring", ImVec2(pill.bottom_right.x - pill.top_left.x,
                                                        pill.bottom_right.y - pill.top_left.y));
            name_last_item_for_ui_driver(
                {"ring", writer->name, hop.output.port, std::to_string(hop.reader_core)},
                slot_count_text);
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            if (is_clicked) {
                ImGui::OpenPopup("##ring_slots");
            }
            ImGui::PushStyleColor(ImGuiCol_PopupBg, TOOLTIP_BACKGROUND);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, input_colour);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, TOOLTIP_PADDING);
            if (ImGui::BeginPopup("##ring_slots")) {
                ImGui::TextUnformatted("Slots of the ring");
                render_info_icon(
                    "Data going from one core to another waits in a ring until the other core "
                    "reads it.\n\n"
                    "This is how many samples or events the ring can hold.\n\n"
                    "If the ring gets full, new data is dropped and a warning is shown in the "
                    "log. A bigger ring prevents that.");
                ImGui::SetNextItemWidth(slider_width);
                // A number that is no power of two, as a file written by hand may hold, shows as
                // the power of two under it until the slider is moved.
                int power_of_two = power_of_two_of_ring_slots(slot_count);
                const bool is_moved = ImGui::SliderInt(
                    "##slots", &power_of_two, 0, highest_power_of_two_of_ring_slots,
                    slot_count_text.c_str(), ImGuiSliderFlags_NoInput);
                name_last_item_for_ui_driver({"ring_slots"});
                if (is_moved) {
                    set_slot_count_of_ring(
                        hop, static_cast<int>(1U << static_cast<unsigned>(power_of_two)));
                }
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            ImGui::PopID();
        }
    }

    // Lays a path through the given points whose corners are rounded, each by no more than half
    // of the shorter of the two stretches that meet in it. The caller strokes it.
    static void lay_path_with_round_corners(ImDrawList* draw_list, std::span<const ImVec2> points) {
        constexpr float widest_bend = 6.0f;
        constexpr float half = 0.5f;
        draw_list->PathLineTo(points.front());
        for (const auto [before, corner, after] : std::views::adjacent<3>(points)) {
            const float length_before = std::hypot(corner.x - before.x, corner.y - before.y);
            const float length_after = std::hypot(after.x - corner.x, after.y - corner.y);
            const float bend = std::min({widest_bend, length_before * half, length_after * half});
            if (bend <= 0.0f) {
                draw_list->PathLineTo(corner);
                continue;
            }
            const float part_before = bend / length_before;
            const float part_after = bend / length_after;
            draw_list->PathLineTo(ImVec2(corner.x + ((before.x - corner.x) * part_before),
                                         corner.y + ((before.y - corner.y) * part_before)));
            draw_list->PathBezierQuadraticCurveTo(
                corner, ImVec2(corner.x + ((after.x - corner.x) * part_after),
                               corner.y + ((after.y - corner.y) * part_after)));
        }
        draw_list->PathLineTo(points.back());
    }

    // A line as a connection inside a core has it.
    static void add_line_with_round_corners(ImDrawList* draw_list, std::span<const ImVec2> points,
                                            ImU32 colour) {
        constexpr float thickness = 2.0f;
        lay_path_with_round_corners(draw_list, points);
        draw_list->PathStroke(colour, ImDrawFlags_None, thickness);
    }

    // A tube: a thick line with a light core. A hop is drawn so between two boxes, where it is
    // what no connection inside a core is.
    static void add_tube(ImDrawList* draw_list, std::span<const ImVec2> points, float width,
                         ImU32 colour) {
        constexpr float core_in_widths = 1.0f / 3.0f;
        lay_path_with_round_corners(draw_list, points);
        draw_list->PathStroke(colour, ImDrawFlags_None, width);
        lay_path_with_round_corners(draw_list, points);
        draw_list->PathStroke(ImGui::GetColorU32(block_colour), ImDrawFlags_None,
                              width * core_in_widths);
    }

    // The part of a hop's line inside the writing core's box, which looks as a connection inside
    // a core does: from under the output sideways to its lane, and up the lane to the box's top.
    static void add_hop_inside_writer(ImDrawList* draw_list, ImVec2 at_output,
                                      const HopPlace& place, ImU32 colour) {
        const float turn_y = at_output.y + place.leaving_turn;
        const std::array<ImVec2, 4> points{
            at_output,
            ImVec2(at_output.x, turn_y),
            ImVec2(place.leaving_x, turn_y),
            ImVec2(place.leaving_x, place.writer_box_top),
        };
        add_line_with_round_corners(draw_list, points, colour);
    }

    // The part of a hop's line between the two boxes: out of the writing core's box by the top,
    // above the boxes to over the reading core, and down to that core's box, through the ring.
    static void add_hop_between_boxes(ImDrawList* draw_list, const HopPlace& place, ImU32 colour) {
        const std::array<ImVec2, 4> points{
            ImVec2(place.leaving_x, place.writer_box_top),
            ImVec2(place.leaving_x, place.level_y),
            ImVec2(place.arriving_x, place.level_y),
            ImVec2(place.arriving_x, place.reader_box_top),
        };
        add_tube(draw_list, points, place.tube_width, colour);
    }

    // The part of a hop's line inside the reading core's box: down its lane, and sideways over
    // the input to the input.
    static void add_hop_inside_reader(ImDrawList* draw_list, const HopPlace& place, ImVec2 at_input,
                                      ImU32 colour) {
        const float turn_y = at_input.y - place.arriving_turn;
        const std::array<ImVec2, 4> points{
            ImVec2(place.arriving_x, place.reader_box_top),
            ImVec2(place.arriving_x, turn_y),
            ImVec2(at_input.x, turn_y),
            at_input,
        };
        add_line_with_round_corners(draw_list, points, colour);
    }

    // The pill of a ring of so many slots whose icon is at this place.
    [[nodiscard]] static ScreenRectangle pill_of_ring(ImVec2 at_ring, int slot_count) {
        constexpr float half = 0.5f;
        constexpr float room_left_of_icon = 5.0f;
        constexpr float gap_before_number = 4.0f;
        constexpr float room_right_of_number = 8.0f;
        const float half_icon = ImGui::CalcTextSize(icon("donut-chart-fill")).x * half;
        const std::string number = std::to_string(slot_count);
        return {
            .top_left = ImVec2(at_ring.x - half_icon - room_left_of_icon,
                               at_ring.y - (ring_pill_height * half)),
            .bottom_right = ImVec2(at_ring.x + half_icon + gap_before_number +
                                       ImGui::CalcTextSize(number.c_str()).x + room_right_of_number,
                                   at_ring.y + (ring_pill_height * half)),
        };
    }

    // A ring: a pill with a ring icon, which stands on the ring's line, and the ring's number of
    // slots. The pill is white and its icon grey while nothing tells how full the ring is.
    static void add_ring(ImDrawList* draw_list, ImVec2 at_ring, int slot_count) {
        constexpr float half = 0.5f;
        constexpr float gap_before_number = 4.0f;
        const char* const ring_icon = icon("donut-chart-fill");
        const float half_icon = ImGui::CalcTextSize(ring_icon).x * half;
        const float text_top = at_ring.y - (ImGui::GetTextLineHeight() * half);
        const std::string number = std::to_string(slot_count);
        const ScreenRectangle pill = pill_of_ring(at_ring, slot_count);
        draw_list->AddRectFilled(pill.top_left, pill.bottom_right, ImGui::GetColorU32(block_colour),
                                 ring_pill_height * half);
        draw_list->AddRect(pill.top_left, pill.bottom_right,
                           ImGui::GetColorU32(block_border_colour), ring_pill_height * half);
        draw_list->AddText(ImVec2(at_ring.x - half_icon, text_top),
                           ImGui::GetColorU32(idle_state_colour), ring_icon);
        draw_list->AddText(ImVec2(at_ring.x + half_icon + gap_before_number, text_top),
                           ImGui::GetColorU32(ImGuiCol_Text), number.c_str());
    }

    void add_lines_of_hop(ImDrawList* draw_list, const Hop& hop) const {
        const std::optional<HopPlace> place = place_of_hop(hop);
        const auto at_output =
            port_anchor(hop.output.processor, hop.output.port, PortDirection::Output);
        if (!place.has_value() || !at_output.has_value()) {
            return;
        }
        const ImU32 colour = ImGui::GetColorU32(kind_colour(hop.kind));
        add_hop_inside_writer(draw_list, *at_output, *place, colour);
        add_hop_between_boxes(draw_list, *place, colour);
        add_terminal_on_edge(draw_list, hop.output.processor, *at_output, colour);
        for (const InputReference& input : hop.inputs) {
            const auto at_input = port_anchor(input.processor, input.port, PortDirection::Input);
            if (at_input.has_value()) {
                add_hop_inside_reader(draw_list, *place, *at_input, colour);
                add_terminal_on_edge(draw_list, input.processor, *at_input, colour);
            }
        }
    }

    void add_ring_of_hop(ImDrawList* draw_list, const Hop& hop) const {
        const std::optional<HopPlace> place = place_of_hop(hop);
        if (place.has_value() && has_ring(hop)) {
            add_ring(draw_list, place->at_ring, slot_count_of_ring(hop));
        }
    }

    // The core whose box the mouse is over, if any.
    [[nodiscard]] std::optional<int> core_at_mouse() const {
        for (const CoreRectangle& box : core_rectangles_) {
            if (ImGui::IsMouseHoveringRect(box.top_left, box.bottom_right, false)) {
                return box.core_number;
            }
        }
        return std::nullopt;
    }

    // The port that was clicked first, while its connection is drawn.
    struct PickedPort {
        ProcessorId processor = 0;
        PortDirection direction = PortDirection::Output;
        PortKind kind = PortKind::Any;
    };

    // The hop that a connection being drawn would be, between its picked port and the core under
    // the mouse; none while the mouse is over no other core than the port's own.
    [[nodiscard]] std::optional<Hop> hop_being_drawn(const PickedPort& picked) const {
        const GraphProcessor* processor = graph_.find_processor(picked.processor);
        const std::optional<int> other_core = core_at_mouse();
        if (processor == nullptr || !other_core.has_value() ||
            *other_core == processor->core_number) {
            return std::nullopt;
        }
        const bool is_output_picked = picked.direction == PortDirection::Output;
        Hop hop;
        hop.writer_core = is_output_picked ? processor->core_number : *other_core;
        hop.reader_core = is_output_picked ? *other_core : processor->core_number;
        hop.kind = picked.kind;
        const HopCores cores{.writer_core = hop.writer_core, .reader_core = hop.reader_core};
        hop.lanes = lanes_of_next_hop(hops_, cores);
        return hop;
    }

    // Where a connection's line ends at a port: on the top edge for an input and on the bottom
    // edge for an output, at the same place of the edge whether the block shows its ports or not.
    [[nodiscard]] std::optional<ImVec2> port_anchor(ProcessorId processor_id,
                                                    std::string_view port_name,
                                                    PortDirection direction) const {
        const GraphProcessor* processor = graph_.find_processor(processor_id);
        const auto block =
            std::ranges::find(block_rectangles_, processor_id, &BlockRectangle::processor);
        if (processor == nullptr || block == block_rectangles_.end()) {
            return std::nullopt;
        }
        const std::vector<PortRowPlace> rows = rows_of_ports(*processor->type, direction);
        const auto row = std::ranges::find_if(rows, [port_name](const PortRowPlace& candidate) {
            return port_name == candidate.port->name;
        });
        if (row == rows.end()) {
            return std::nullopt;
        }
        return ImVec2(block->top_left.x + row->line_x, direction == PortDirection::Input
                                                           ? block->top_left.y
                                                           : block->bottom_right.y);
    }

    [[nodiscard]] bool are_ports_shown_of(ProcessorId processor_id) const {
        return are_all_ports_shown_ || is_connection_being_drawn_ ||
               processor_showing_ports_ == processor_id;
    }

    // On the edge of each block that shows no ports, an empty circle for every port without a
    // connection.
    void add_terminals_of_unconnected_ports(ImDrawList* draw_list) const {
        for (const BlockRectangle& block : block_rectangles_) {
            const GraphProcessor* processor = graph_.find_processor(block.processor);
            if (processor == nullptr || are_ports_shown_of(block.processor)) {
                continue;
            }
            for (const PortDescription& port : processor->type->ports) {
                const std::optional<ImVec2> at_edge =
                    port_anchor(block.processor, port.name, port.direction);
                if (at_edge.has_value() && !has_connection(block.processor, port)) {
                    add_empty_terminal(draw_list, *at_edge,
                                       ImGui::GetColorU32(line_colour_of_port(port, false)));
                }
            }
        }
    }

    // The circle at the end of a connection's line, on the edge of a block that shows no ports.
    // Where the ports show, the line goes on to the port's name and has its circle there.
    void add_terminal_on_edge(ImDrawList* draw_list, ProcessorId processor_id, ImVec2 at_edge,
                              ImU32 colour) const {
        if (!are_ports_shown_of(processor_id)) {
            draw_list->AddCircleFilled(at_edge, terminal_radius, colour);
        }
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
            add_connection_inside_core(draw_list, connection);
        }
        for (const Hop& hop : hops_) {
            add_lines_of_hop(draw_list, hop);
        }
        add_terminals_of_unconnected_ports(draw_list);
        add_connection_being_drawn(draw_list);
        // The rings last: a ring's pill lies over the lines of the lanes beside its own.
        for (const Hop& hop : hops_) {
            add_ring_of_hop(draw_list, hop);
        }
        draw_list->PopClipRect();
        ImGui::End();
    }

    // A connection between two processors of one core: a curve from the output to the input. One
    // between two cores is a hop's.
    void add_connection_inside_core(ImDrawList* draw_list,
                                    const GraphConnection& connection) const {
        const GraphProcessor* writer = graph_.find_processor(connection.output.processor);
        const GraphProcessor* reader = graph_.find_processor(connection.input.processor);
        const PortDescription* output_port = graph_.find_port(
            connection.output.processor, connection.output.port, PortDirection::Output);
        const auto at_output =
            port_anchor(connection.output.processor, connection.output.port, PortDirection::Output);
        const auto at_input =
            port_anchor(connection.input.processor, connection.input.port, PortDirection::Input);
        if (writer == nullptr || reader == nullptr || output_port == nullptr || !at_output ||
            !at_input || writer->core_number != reader->core_number) {
            return;
        }
        const ImU32 colour = ImGui::GetColorU32(kind_colour(output_port->kind));
        add_connection_line(draw_list, {.at_output = *at_output, .at_input = *at_input}, colour);
        add_terminal_on_edge(draw_list, connection.output.processor, *at_output, colour);
        add_terminal_on_edge(draw_list, connection.input.processor, *at_input, colour);
    }

    // The line from the picked port to the mouse. Over another core than the port's own it is a
    // hop already, with its ring, so that the user sees what the connection will be.
    void add_connection_being_drawn(ImDrawList* draw_list) const {
        const OutputReference* const output = picked_output();
        const InputReference* const input = picked_input();
        if (output == nullptr && input == nullptr) {
            return;
        }
        const bool is_output_picked = output != nullptr;
        const ProcessorId processor = is_output_picked ? output->processor : input->processor;
        const std::string& port_name = is_output_picked ? output->port : input->port;
        const PortDirection direction =
            is_output_picked ? PortDirection::Output : PortDirection::Input;
        const PortDescription* port = graph_.find_port(processor, port_name, direction);
        const auto at_port = port_anchor(processor, port_name, direction);
        if (port == nullptr || !at_port.has_value()) {
            return;
        }
        const ImVec2 mouse = ImGui::GetMousePos();
        const ImU32 colour = ImGui::GetColorU32(kind_colour(port->kind));
        const std::optional<Hop> hop = hop_being_drawn({
            .processor = processor,
            .direction = direction,
            .kind = port->kind,
        });
        const std::optional<HopPlace> place = hop.has_value() ? place_of_hop(*hop) : std::nullopt;
        if (!place.has_value()) {
            const LineEnds ends{
                .at_output = is_output_picked ? *at_port : mouse,
                .at_input = is_output_picked ? mouse : *at_port,
            };
            add_connection_line(draw_list, ends, colour);
        } else if (is_output_picked) {
            add_hop_inside_writer(draw_list, *at_port, *place, colour);
            add_hop_between_boxes(draw_list, *place, colour);
            const std::array<ImVec2, 3> to_mouse{
                ImVec2(place->arriving_x, place->reader_box_top),
                ImVec2(place->arriving_x, mouse.y),
                mouse,
            };
            add_line_with_round_corners(draw_list, to_mouse, colour);
        } else {
            const std::array<ImVec2, 3> from_mouse{
                mouse,
                ImVec2(place->leaving_x, mouse.y),
                ImVec2(place->leaving_x, place->writer_box_top),
            };
            add_line_with_round_corners(draw_list, from_mouse, colour);
            add_hop_between_boxes(draw_list, *place, colour);
            add_hop_inside_reader(draw_list, *place, *at_port, colour);
        }
        if (place.has_value() && has_ring(*hop)) {
            add_ring(draw_list, place->at_ring,
                     usual_ring_slot_count(is_core_realtime(hop->reader_core)));
        }
        add_terminal_on_edge(draw_list, processor, *at_port, colour);
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
        constexpr float widest_bend = 60.0f;
        constexpr float half = 0.5f;
        constexpr float thickness = 2.0f;
        // An input a short way under its output leaves the curve little height: bent as far as
        // for a long way, the curve would overshoot both ends and kink.
        const float drop = ends.at_input.y - ends.at_output.y;
        const float bend = drop > 0.0f ? std::min(widest_bend, drop * half) : widest_bend;
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

    // Every core that holds a processor, each as a box; while the pipeline is empty, the first
    // core offered. While a processor is dragged, from the panel or by its block, every core that
    // is offered is shown too, each a place to drop it.
    [[nodiscard]] std::vector<int> shown_core_numbers() const {
        const std::vector<int>& offered = cpu_cores_.offered();
        std::vector<int> core_numbers;
        for (const GraphCore& core : graph_.cores) {
            if (!core.processors.empty()) {
                core_numbers.push_back(core.number);
            }
        }
        if (core_numbers.empty()) {
            core_numbers.push_back(offered.empty() ? 0 : offered.front());
        }
        if (is_any_processor_dragged()) {
            for (const int core_number : offered) {
                if (!std::ranges::contains(core_numbers, core_number)) {
                    core_numbers.push_back(core_number);
                }
            }
        }
        std::ranges::sort(core_numbers);
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
