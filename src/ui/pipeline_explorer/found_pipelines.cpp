#pragma once
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
#include <vector>
#include "../pipeline_designer/opened_pipeline.cpp"
#include "../pipeline_designer/pipeline_build.cpp"
#include "../pipeline_designer/pipeline_reader.cpp"
#include "../pipeline_designer/pipeline_writer.cpp"
#include "all_processors.cpp"

// The pipelines that lie on the disk, looked for beside the UI's thread: reading a folder and its
// files may take a moment.

// The folder that Falcon keeps the pipelines in, each in a folder of its own. None when the
// environment names no home directory.
[[nodiscard]] inline std::optional<std::filesystem::path> pipelines_directory() {
    const char* const home = std::getenv("HOME");
    if (home == nullptr || std::string_view(home).empty()) {
        return std::nullopt;
    }
    return std::filesystem::path(home) / "falcon";
}

struct FoundPipeline {
    std::string folder_name;
    std::string name;  // the name its header gives it, or empty
    std::string description;
    std::string problem;      // why its pipeline.cpp can't be read; empty for one that can
    OpenedPipeline pipeline;  // what the designer is opened with, for one that can be read
    // When its pipeline.cpp was last written, in nanoseconds since 1970, which orders the list;
    // 0 if that can't be told.
    std::int64_t modified_nanoseconds = 0;
    // The same in local time, as the list shows it, e.g. "2026-11-22 19:30"; empty if it can't
    // be told.
    std::string modified_at;
};

// Sets when a found pipeline's file was last written.
inline void note_when_modified(FoundPipeline& found, const std::filesystem::path& file) {
    std::error_code error;
    const std::filesystem::file_time_type written = std::filesystem::last_write_time(file, error);
    if (error) {
        return;
    }
    const auto system_time = std::chrono::time_point_cast<std::chrono::nanoseconds>(
        std::chrono::file_clock::to_sys(written));
    found.modified_nanoseconds = system_time.time_since_epoch().count();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(system_time));
    std::tm local_time{};
    if (localtime_r(&seconds, &local_time) == nullptr) {
        return;
    }
    constexpr int tm_first_year = 1900;
    found.modified_at = std::format("{:04}-{:02}-{:02} {:02}:{:02}",
                                    local_time.tm_year + tm_first_year, local_time.tm_mon + 1,
                                    local_time.tm_mday, local_time.tm_hour, local_time.tm_min);
}

// A folder's name, also for a path that ends in a slash.
[[nodiscard]] inline std::string folder_name_of(const std::filesystem::path& directory) {
    const std::filesystem::path normal = directory.lexically_normal();
    return (normal.has_filename() ? normal.filename() : normal.parent_path().filename()).string();
}

// Whether a pipeline.cpp must start with the header block to count as a pipeline's. It must
// where folders are looked through, since the block is what tells a pipeline's folder from any
// other; a file that the user names needn't, e.g. one written by hand.
enum class HeaderBlock : std::uint8_t { Needed, Optional };

// The pipeline that a folder holds, or none for a folder that holds none.
[[nodiscard]] inline std::optional<FoundPipeline> pipeline_found_in(
    const std::filesystem::path& directory, HeaderBlock header_block) {
    const std::optional<std::string> text = text_of_file(directory / "pipeline.cpp");
    if (!text.has_value() ||
        (header_block == HeaderBlock::Needed && !text->starts_with(PIPELINE_HEADER_START))) {
        return std::nullopt;
    }
    FoundPipeline found;
    found.folder_name = folder_name_of(directory);
    found.pipeline.directory = directory;
    note_when_modified(found, directory / "pipeline.cpp");
    ReadPipeline read = read_pipeline(*text, ALL_PROCESSORS);
    if (read.has_value()) {
        found.name = read->name;
        found.description = read->description;
        found.pipeline.graph = std::move(*read);
    } else {
        found.problem = read.error();
        // Its header may be whole all the same, and then says what the pipeline is called.
        try {
            const PipelineHeader header = header_of(*text);
            found.name = header.name;
            found.description = header.description;
        } catch (const PipelineRefusal&) {
            found.name.clear();
        }
    }
    std::error_code error;
    if (std::filesystem::exists(directory / "pipeline", error)) {
        found.pipeline.compiled_text =
            text_of_file(directory / ".last_compiled_pipeline.cpp").value_or(std::string());
    }
    return found;
}

// The folder that holds the pipelines the user deleted, until they are deleted for good.
[[nodiscard]] inline std::filesystem::path trash_directory_in(
    const std::filesystem::path& pipelines_directory) {
    return pipelines_directory / ".trash";
}

// A pipeline in the trash. Its folder there is named by when it was moved there and by what it
// was called before, e.g. "2026-11-22_19-30-05_memory_impairment".
struct TrashedPipeline {
    std::string trash_name;   // its folder's name in the trash
    std::string folder_name;  // its folder's name before, which it gets back when restored
    std::string deleted_at;   // when it was moved there, e.g. "2026-11-22 19:30"; empty if unknown
    std::string name;         // the name its header gives it, or empty
    std::string description;
    int run_count = 0;  // the runs that lie in it, which go with it when it is deleted for good
};

inline constexpr std::string_view TRASH_TIME_FORM = "0000-00-00_00-00-00";

// Whether a folder's name in the trash starts with a time in the form of TRASH_TIME_FORM and an
// underscore: digits where the form has them, and its other characters as they are.
[[nodiscard]] inline bool starts_with_trash_time(std::string_view trash_name) {
    if (trash_name.size() <= TRASH_TIME_FORM.size() + 1 ||
        trash_name.at(TRASH_TIME_FORM.size()) != '_') {
        return false;
    }
    for (std::size_t position = 0; position < TRASH_TIME_FORM.size(); ++position) {
        const char character = trash_name.at(position);
        const bool is_digit = character >= '0' && character <= '9';
        const bool is_digit_expected = TRASH_TIME_FORM.at(position) == '0';
        if (is_digit_expected ? !is_digit : character != TRASH_TIME_FORM.at(position)) {
            return false;
        }
    }
    return true;
}

// How many folders a folder holds; 0 for one that isn't there.
[[nodiscard]] inline int count_of_folders_in(const std::filesystem::path& directory) {
    int count = 0;
    std::error_code error;
    std::filesystem::directory_iterator entry(directory, error);
    const std::filesystem::directory_iterator end;
    while (!error && entry != end) {
        std::error_code entry_error;
        if (entry->is_directory(entry_error)) {
            ++count;
        }
        entry.increment(error);
    }
    return count;
}

// What a folder of the trash holds. A folder that someone put there by hand, without a time in
// its name, is listed under its whole name.
[[nodiscard]] inline TrashedPipeline trashed_pipeline_in(const std::filesystem::path& directory) {
    TrashedPipeline trashed;
    trashed.trash_name = folder_name_of(directory);
    trashed.folder_name = trashed.trash_name;
    if (starts_with_trash_time(trashed.trash_name)) {
        constexpr std::size_t date_size = std::string_view("0000-00-00").size();
        constexpr std::size_t hour_position = date_size + 1;
        constexpr std::size_t minute_position = hour_position + 3;
        trashed.folder_name = trashed.trash_name.substr(TRASH_TIME_FORM.size() + 1);
        trashed.deleted_at = std::format("{} {}:{}", trashed.trash_name.substr(0, date_size),
                                         trashed.trash_name.substr(hour_position, 2),
                                         trashed.trash_name.substr(minute_position, 2));
    }
    if (const std::optional<FoundPipeline> found =
            pipeline_found_in(directory, HeaderBlock::Optional)) {
        trashed.name = found->name;
        trashed.description = found->description;
    }
    trashed.run_count = count_of_folders_in(directory / "runs");
    return trashed;
}

struct FoundPipelines {
    // The one whose pipeline.cpp was written last comes first; those written at one time are
    // in the order of their folders' names.
    std::vector<FoundPipeline> pipelines;
    // Every folder there, a pipeline's or not: a new pipeline's folder can't have one of these
    // names.
    std::vector<std::string> folder_names;
    std::vector<TrashedPipeline> trashed;  // what the trash holds, the last one deleted first
};

[[nodiscard]] inline FoundPipelines find_pipelines(const std::filesystem::path& directory) {
    FoundPipelines found;
    std::error_code error;
    std::filesystem::directory_iterator entry(directory, error);
    const std::filesystem::directory_iterator end;
    while (!error && entry != end) {
        std::error_code entry_error;
        if (entry->is_directory(entry_error)) {
            std::string folder_name = entry->path().filename().string();
            // A folder whose name starts with a dot is the app's own, never a pipeline's.
            if (!folder_name.starts_with('.')) {
                if (std::optional<FoundPipeline> pipeline =
                        pipeline_found_in(entry->path(), HeaderBlock::Needed)) {
                    found.pipelines.push_back(std::move(*pipeline));
                }
            }
            found.folder_names.push_back(std::move(folder_name));
        }
        entry.increment(error);
    }
    std::ranges::sort(found.pipelines, [](const FoundPipeline& one, const FoundPipeline& other) {
        if (one.modified_nanoseconds != other.modified_nanoseconds) {
            return one.modified_nanoseconds > other.modified_nanoseconds;
        }
        return one.folder_name < other.folder_name;
    });
    std::error_code trash_error;
    std::filesystem::directory_iterator trash_entry(trash_directory_in(directory), trash_error);
    while (!trash_error && trash_entry != end) {
        std::error_code entry_error;
        if (trash_entry->is_directory(entry_error)) {
            found.trashed.push_back(trashed_pipeline_in(trash_entry->path()));
        }
        trash_entry.increment(trash_error);
    }
    std::ranges::sort(found.trashed, std::ranges::greater(), &TrashedPipeline::trash_name);
    return found;
}

// One search at a time, beside the thread that asks for it.
class PipelineSearch {
public:
    // Starts looking for the pipelines of this folder. Does nothing while a search runs, and
    // returns whether it started one.
    [[nodiscard]] bool start(std::filesystem::path directory) {
        {
            const std::scoped_lock lock(mutex_);
            if (is_running_) {
                return false;
            }
            is_running_ = true;
        }
        thread_ = std::jthread([this, searched = std::move(directory)] {
            FoundPipelines found = find_pipelines(searched);
            const std::scoped_lock lock(mutex_);
            result_ = std::move(found);
            is_running_ = false;
        });
        return true;
    }

    // What the last search found, once, or none while there is nothing new.
    [[nodiscard]] std::optional<FoundPipelines> take_result() {
        const std::scoped_lock lock(mutex_);
        return std::exchange(result_, std::nullopt);
    }

private:
    std::mutex mutex_;
    bool is_running_ = false;
    std::optional<FoundPipelines> result_;
    // The last member, so that it ends before what its thread uses is gone.
    std::jthread thread_;
};
