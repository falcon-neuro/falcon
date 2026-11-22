#pragma once
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include "../common/files.cpp"
#include "build_problems.cpp"

// Compiles a pipeline.cpp into its pipeline's directory beside the UI's thread, so that the window
// stays usable meanwhile. The repository's own build does the compiling: the file is copied to
// the one place that the build's target falcon_pipeline compiles, and the executable is copied
// back into the pipeline's directory.

// The one file that the build's target falcon_pipeline compiles, which is the file that a
// compiler's error about a pipeline names.
[[nodiscard]] inline std::filesystem::path compiled_pipeline_source() {
    return std::filesystem::path(FALCON_BUILD_DIRECTORY) / "pipeline" / "pipeline.cpp";
}

enum class BuildState : std::uint8_t { None, Running, Succeeded, Failed };

// How the last compilation went, with what the compiler wrote. Where nothing was compiled, e.g.
// for a graph that can't be written, `problem` says why and `text` says the same as one text.
struct BuildOutcome {
    BuildState state = BuildState::None;
    std::string text;
    std::optional<BuildProblem> problem;
};

// A program that was started, with all that it writes, its errors too, coming through one pipe.
struct StartedProgram {
    pid_t process_id = 0;
    int output_end = -1;      // the pipe's end to read from, which whoever reads it closes
    std::string start_error;  // why the program couldn't be started; empty for one that was
};

// Starts a program directly, with the arguments as they are and no shell in between. A name
// without a folder is looked for where programs lie (PATH).
[[nodiscard]] inline StartedProgram start_program(std::vector<std::string> arguments) {
    std::array<int, 2> output_pipe{};
    if (pipe(output_pipe.data()) != 0) {
        return {
            .process_id = 0,
            .output_end = -1,
            .start_error = "no pipe could be made for the program's output",
        };
    }
    const int read_end = output_pipe[0];
    const int write_end = output_pipe[1];
    posix_spawn_file_actions_t file_actions;
    posix_spawn_file_actions_init(&file_actions);
    // The program reads nothing: what this process is sent, e.g. by a test's driver, isn't its.
    posix_spawn_file_actions_addopen(&file_actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&file_actions, write_end, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&file_actions, write_end, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&file_actions, read_end);
    posix_spawn_file_actions_addclose(&file_actions, write_end);
    std::vector<char*> argument_pointers;
    argument_pointers.reserve(arguments.size() + 1);
    for (std::string& argument : arguments) {
        argument_pointers.push_back(argument.data());
    }
    argument_pointers.push_back(nullptr);
    pid_t process_id = 0;
    const int spawn_error = posix_spawnp(&process_id, argument_pointers.front(), &file_actions,
                                         nullptr, argument_pointers.data(), environ);
    posix_spawn_file_actions_destroy(&file_actions);
    close(write_end);
    if (spawn_error != 0) {
        close(read_end);
        return {
            .process_id = 0,
            .output_end = -1,
            .start_error = std::format("{} couldn't be started: {}", arguments.front(),
                                       std::system_category().message(spawn_error)),
        };
    }
    return {.process_id = process_id, .output_end = read_end, .start_error = {}};
}

struct ProgramResult {
    bool has_succeeded = false;
    std::string output;  // what the program wrote, its errors too
};

// Runs a program to its end and gathers what it writes.
[[nodiscard]] inline ProgramResult run_program(std::vector<std::string> arguments) {
    const StartedProgram program = start_program(std::move(arguments));
    if (!program.start_error.empty()) {
        return {.has_succeeded = false, .output = program.start_error};
    }
    ProgramResult result;
    constexpr std::size_t chunk_size = 4096;
    std::array<char, chunk_size> chunk{};
    while (true) {
        const ssize_t read_count = read(program.output_end, chunk.data(), chunk.size());
        if (read_count < 0 && errno == EINTR) {
            continue;
        }
        if (read_count <= 0) {
            break;
        }
        result.output.append(chunk.data(), static_cast<std::size_t>(read_count));
    }
    close(program.output_end);
    int status = 0;
    result.has_succeeded = waitpid(program.process_id, &status, 0) == program.process_id &&
                           WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return result;
}

// Compiles a pipeline.cpp into its pipeline's directory: the text as pipeline.cpp, its executable
// as `pipeline`, and beside them the text that the executable was built from, which tells later
// whether the executable is still the graph's.
[[nodiscard]] inline BuildOutcome compile_pipeline(
    const std::string& pipeline_text, const std::filesystem::path& pipeline_directory) {
    const std::filesystem::path build_directory = FALCON_BUILD_DIRECTORY;
    const std::filesystem::path compiled_source = compiled_pipeline_source();
    std::error_code error;
    std::filesystem::create_directories(pipeline_directory, error);
    std::filesystem::create_directories(compiled_source.parent_path(), error);
    if (!write_file(pipeline_directory / "pipeline.cpp", pipeline_text) ||
        !write_file(compiled_source, pipeline_text)) {
        return {
            .state = BuildState::Failed,
            .text =
                std::format("pipeline.cpp couldn't be written into {} and {}",
                            pipeline_directory.string(), compiled_source.parent_path().string()),
            .problem = std::nullopt,
        };
    }
    ProgramResult build =
        run_program({"cmake", "--build", build_directory.string(), "--target", "falcon_pipeline"});
    if (!build.has_succeeded) {
        return {
            .state = BuildState::Failed,
            .text = std::move(build.output),
            .problem = std::nullopt,
        };
    }
    std::filesystem::copy_file(build_directory / "falcon_pipeline", pipeline_directory / "pipeline",
                               std::filesystem::copy_options::overwrite_existing, error);
    if (error || !write_file(pipeline_directory / ".last_compiled_pipeline.cpp", pipeline_text)) {
        return {
            .state = BuildState::Failed,
            .text = std::format("The pipeline was compiled, but its executable couldn't be put "
                                "into {}: {}",
                                pipeline_directory.string(), error.message()),
            .problem = std::nullopt,
        };
    }
    return {
        .state = BuildState::Succeeded,
        .text = std::move(build.output),
        .problem = std::nullopt,
    };
}

// One compilation at a time, beside the thread that asks for it.
class PipelineBuild {
public:
    // Starts compiling this pipeline.cpp into this directory. Does nothing while a compilation
    // runs.
    void start(std::string pipeline_text, std::filesystem::path pipeline_directory) {
        {
            const std::scoped_lock lock(mutex_);
            if (outcome_.state == BuildState::Running) {
                return;
            }
            outcome_ = {.state = BuildState::Running, .text = {}, .problem = std::nullopt};
        }
        thread_ = std::jthread(
            [this, text = std::move(pipeline_text), directory = std::move(pipeline_directory)] {
                BuildOutcome outcome = compile_pipeline(text, directory);
                const std::scoped_lock lock(mutex_);
                outcome_ = std::move(outcome);
            });
    }

    // Notes why nothing was compiled, e.g. for a graph that can't be written. Does nothing while
    // a compilation runs.
    void fail(BuildProblem problem) {
        const std::scoped_lock lock(mutex_);
        if (outcome_.state != BuildState::Running) {
            outcome_ = {
                .state = BuildState::Failed,
                .text = std::format("{}\n{}", problem.title, problem.body),
                .problem = std::move(problem),
            };
        }
    }

    [[nodiscard]] BuildOutcome outcome() const {
        const std::scoped_lock lock(mutex_);
        return outcome_;
    }

private:
    mutable std::mutex mutex_;
    BuildOutcome outcome_;
    // The last member, so that it ends before what its thread uses is gone.
    std::jthread thread_;
};
