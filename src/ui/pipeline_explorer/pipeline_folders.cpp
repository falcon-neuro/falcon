#pragma once
#include <ctime>
#include <filesystem>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include "found_pipelines.cpp"

// What is done to a pipeline's whole folder: moved into the trash, brought back from it, and
// deleted for good. Each returns why it couldn't be done, or an empty text. Done beside the UI's
// thread (see PipelineFolderWork): deleting a pipeline's recordings may take long.

// Whether a text is the name of one folder and nothing more: no path into another folder, and
// neither "." nor "..". What is moved or deleted is named by such a name alone, so that nothing
// outside the folder it is looked for in can be meant.
[[nodiscard]] inline bool is_one_folder_name(std::string_view text) {
    return !text.empty() && text != "." && text != ".." && !text.contains('/');
}

// The local time now in the form of TRASH_TIME_FORM, or none if it can't be read.
[[nodiscard]] inline std::optional<std::string> trash_time_now() {
    const std::time_t now = std::time(nullptr);
    std::tm local_time{};
    if (localtime_r(&now, &local_time) == nullptr) {
        return std::nullopt;
    }
    constexpr int tm_first_year = 1900;
    return std::format("{:04}-{:02}-{:02}_{:02}-{:02}-{:02}", local_time.tm_year + tm_first_year,
                       local_time.tm_mon + 1, local_time.tm_mday, local_time.tm_hour,
                       local_time.tm_min, local_time.tm_sec);
}

// Moves a pipeline's folder into the trash, under a name that says when and what it was called.
[[nodiscard]] inline std::string move_pipeline_to_trash(
    const std::filesystem::path& pipelines_directory, const std::string& folder_name) {
    if (!is_one_folder_name(folder_name) || folder_name.starts_with('.')) {
        return std::format("{} is no pipeline's folder.", folder_name);
    }
    const std::optional<std::string> time = trash_time_now();
    if (!time.has_value()) {
        return "The time couldn't be read, which names a pipeline in the trash.";
    }
    const std::filesystem::path trash = trash_directory_in(pipelines_directory);
    const std::filesystem::path in_trash = trash / std::format("{}_{}", *time, folder_name);
    std::error_code error;
    std::filesystem::create_directories(trash, error);
    if (error) {
        return std::format("The trash folder couldn't be made: {}", error.message());
    }
    if (std::filesystem::exists(in_trash, error)) {
        return std::format("The trash holds {} already.", in_trash.filename().string());
    }
    std::filesystem::rename(pipelines_directory / folder_name, in_trash, error);
    return error
               ? std::format("{} couldn't be moved to the trash: {}", folder_name, error.message())
               : std::string();
}

// Brings a pipeline back from the trash, under the name its folder had. Refused where a folder
// of that name is there again.
[[nodiscard]] inline std::string restore_pipeline(const std::filesystem::path& pipelines_directory,
                                                  const std::string& trash_name,
                                                  const std::string& folder_name) {
    if (!is_one_folder_name(trash_name) || !is_one_folder_name(folder_name) ||
        folder_name.starts_with('.')) {
        return std::format("{} can't be restored as {}.", trash_name, folder_name);
    }
    const std::filesystem::path restored = pipelines_directory / folder_name;
    std::error_code error;
    if (std::filesystem::exists(restored, error)) {
        return std::format("A folder named {} is there already.", folder_name);
    }
    std::filesystem::rename(trash_directory_in(pipelines_directory) / trash_name, restored, error);
    return error ? std::format("{} couldn't be restored: {}", folder_name, error.message())
                 : std::string();
}

// Deletes a pipeline of the trash with all it holds, its runs too. This can't be undone.
[[nodiscard]] inline std::string delete_pipeline_for_good(
    const std::filesystem::path& pipelines_directory, const std::string& trash_name) {
    if (!is_one_folder_name(trash_name)) {
        return std::format("{} is no folder of the trash.", trash_name);
    }
    const std::filesystem::path deleted = trash_directory_in(pipelines_directory) / trash_name;
    std::error_code error;
    // A link in the trash is removed as a link: what it points to isn't the trash's to delete.
    if (!std::filesystem::is_symlink(deleted, error) &&
        !std::filesystem::is_directory(deleted, error)) {
        return std::format("The trash holds no folder {}.", trash_name);
    }
    std::filesystem::remove_all(deleted, error);
    return error ? std::format("{} couldn't be deleted: {}", trash_name, error.message())
                 : std::string();
}

// One piece of work on a folder at a time, beside the thread that asks for it.
class PipelineFolderWork {
public:
    // Starts this work, which returns why it failed or an empty text. Does nothing while another
    // runs.
    void start(std::function<std::string()> work) {
        {
            const std::scoped_lock lock(mutex_);
            if (is_running_) {
                return;
            }
            is_running_ = true;
        }
        thread_ = std::jthread([this, run = std::move(work)] {
            std::string problem = run();
            const std::scoped_lock lock(mutex_);
            outcome_ = std::move(problem);
            is_running_ = false;
        });
    }

    [[nodiscard]] bool is_running() const {
        const std::scoped_lock lock(mutex_);
        return is_running_;
    }

    // How the last work ended, once: why it failed, or an empty text. None while nothing has
    // ended since the last call.
    [[nodiscard]] std::optional<std::string> take_outcome() {
        const std::scoped_lock lock(mutex_);
        return std::exchange(outcome_, std::nullopt);
    }

private:
    mutable std::mutex mutex_;
    bool is_running_ = false;
    std::optional<std::string> outcome_;
    // The last member, so that it ends before what its thread uses is gone.
    std::jthread thread_;
};
