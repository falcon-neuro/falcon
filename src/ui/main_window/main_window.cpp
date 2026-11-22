#pragma once
#include <imgui.h>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include "bottom_panel.cpp"
#include "left_panel.cpp"
#include "main_viewport.cpp"
#include "main_window_controller.cpp"

#include "../common/remembered_numbers.cpp"
#include "../pipeline_designer/pipeline_designer.cpp"
#include "../pipeline_explorer/pipeline_explorer.cpp"

// The window shows one of two screens: the welcome screen, which lists the pipelines, or the
// designer of the pipeline that was opened from it.
class MainWindow {
private:
    MainWindowController& main_window_controller_;
    LeftPanel left_panel_;
    BottomPanel bottom_panel_;
    MainViewport main_viewport_;
    // What the app keeps between two sessions, in a file of the folder .config under the one that
    // holds the pipelines. Before the designer, which uses it.
    RememberedNumbers remembered_{remembered_numbers_file()};
    PipelineExplorer pipeline_explorer_;
    // The designer of the pipeline that is open; none while the welcome screen shows.
    std::optional<PipelineDesigner> pipeline_designer_;

    [[nodiscard]] static std::optional<std::filesystem::path> remembered_numbers_file() {
        const std::optional<std::filesystem::path> directory = pipelines_directory();
        if (!directory.has_value()) {
            return std::nullopt;
        }
        return *directory / ".config" / "ui_state.txt";
    }

    // Opens the pipeline of this directory at once, in place of the welcome screen. A directory
    // without a pipeline.cpp is a pipeline that holds nothing yet. One whose pipeline.cpp can't
    // be read isn't opened.
    void open_pipeline_in(const std::filesystem::path& directory) {
        if (std::optional<FoundPipeline> found =
                pipeline_found_in(directory, HeaderBlock::Optional)) {
            if (found->problem.empty()) {
                pipeline_designer_.emplace(std::move(found->pipeline), remembered_);
            }
            return;
        }
        std::error_code error;
        if (!std::filesystem::exists(directory / "pipeline.cpp", error)) {
            OpenedPipeline pipeline;
            pipeline.directory = directory;
            pipeline_designer_.emplace(std::move(pipeline), remembered_);
        }
    }

public:
    // Falcon starts on the welcome screen. Two environment variables start it on the designer
    // instead: FALCON_EXAMPLE_PIPELINE with a small pipeline that lies in no folder, for a
    // picture of the window (its value "selected" also opens one processor's options), and
    // FALCON_OPEN_PIPELINE with the pipeline of the directory it names, for a test.
    MainWindow(MainWindowController& main_window_controller)
        : main_window_controller_(main_window_controller),
          left_panel_([this] { main_window_controller_.set_left_sidebar_visible(false); }),
          bottom_panel_([this] { main_window_controller_.set_bottom_bar_visible(false); }),
          main_viewport_([this] { main_window_controller_.set_left_sidebar_visible(true); },
                         [this] { main_window_controller_.set_bottom_bar_visible(true); }) {
        if (const char* example = std::getenv("FALCON_EXAMPLE_PIPELINE")) {
            pipeline_designer_.emplace(PipelineDesigner::ExamplePipeline{
                .is_one_selected = std::string_view(example) == "selected",
            });
        } else if (const char* directory = std::getenv("FALCON_OPEN_PIPELINE")) {
            open_pipeline_in(directory);
        }
        if (!pipeline_designer_.has_value()) {
            pipeline_explorer_.refresh();
        }
    }

    void render() {
        if (pipeline_designer_.has_value()) {
            pipeline_designer_->render();
            if (pipeline_designer_->is_leaving_asked()) {
                pipeline_designer_.reset();
                pipeline_explorer_.refresh();
            }
            return;
        }
        pipeline_explorer_.render();
        if (std::optional<OpenedPipeline> opened = pipeline_explorer_.take_opened()) {
            pipeline_designer_.emplace(std::move(*opened), remembered_);
        }
    }
};
