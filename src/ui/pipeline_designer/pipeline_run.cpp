#pragma once
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include "core/run_log_lines.cpp"
#include "pipeline_build.cpp"

// Runs a pipeline's executable beside the UI's thread: starts it, reads what it writes line by
// line, tells from those lines how far the run is, and stops it.

enum class RunState : std::uint8_t {
    None,         // no run was started, or the last one ended well
    Starting,     // the program is started and builds its processors
    Running,      // the program said that its pipeline is running
    Stopping,     // the program was told to stop
    StartFailed,  // the program ended before its pipeline ran
    Failed,       // the program ended badly after its pipeline ran
};

[[nodiscard]] constexpr bool is_run_on(RunState state) {
    return state == RunState::Starting || state == RunState::Running || state == RunState::Stopping;
}

// One run at a time.
class PipelineRun {
public:
    PipelineRun() = default;
    PipelineRun(const PipelineRun&) = delete;
    PipelineRun& operator=(const PipelineRun&) = delete;
    PipelineRun(PipelineRun&&) = delete;
    PipelineRun& operator=(PipelineRun&&) = delete;

    // A run that is still on is stopped, and waited for: the program belongs to this object.
    ~PipelineRun() { stop(); }

    // Starts this executable. Does nothing while a run is on.
    void start(const std::filesystem::path& executable) {
        {
            const std::scoped_lock lock(mutex_);
            if (is_run_on(state_)) {
                return;
            }
            state_ = RunState::Starting;
            new_lines_.clear();
        }
        const StartedProgram program = start_program({executable.string()});
        if (!program.start_error.empty()) {
            const std::scoped_lock lock(mutex_);
            new_lines_.push_back(program.start_error);
            state_ = RunState::StartFailed;
            return;
        }
        {
            const std::scoped_lock lock(mutex_);
            process_id_ = program.process_id;
        }
        thread_ = std::jthread([this, program] { follow(program); });
    }

    // Tells the program to stop, as Ctrl+C does in a terminal. Does nothing while no run is on.
    void stop() {
        const std::scoped_lock lock(mutex_);
        if (process_id_ != 0 && (state_ == RunState::Starting || state_ == RunState::Running)) {
            kill(process_id_, SIGINT);
            state_ = RunState::Stopping;
        }
    }

    // Forgets how the last run ended. Does nothing while a run is on.
    void forget_last_run() {
        const std::scoped_lock lock(mutex_);
        if (!is_run_on(state_)) {
            state_ = RunState::None;
        }
    }

    [[nodiscard]] RunState state() const {
        const std::scoped_lock lock(mutex_);
        return state_;
    }

    // The lines that the program wrote since the last call, in their order.
    [[nodiscard]] std::vector<std::string> take_new_lines() {
        const std::scoped_lock lock(mutex_);
        return std::exchange(new_lines_, {});
    }

private:
    // The engine's line that says its pipeline is running, as it ends: after the line's time.
    [[nodiscard]] static std::string end_of_running_line() {
        return std::format(" log engine: {}", PIPELINE_IS_RUNNING_LINE);
    }

    void take_line(std::string line, const std::string& running_line_end) {
        const std::scoped_lock lock(mutex_);
        if (state_ == RunState::Starting && line.ends_with(running_line_end)) {
            state_ = RunState::Running;
        }
        new_lines_.push_back(std::move(line));
    }

    // Reads what the program writes until it ends, then notes how it ended.
    void follow(const StartedProgram& program) {
        const std::string running_line_end = end_of_running_line();
        std::string unfinished_line;
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
            unfinished_line.append(chunk.data(), static_cast<std::size_t>(read_count));
            std::size_t line_end = unfinished_line.find('\n');
            while (line_end != std::string::npos) {
                take_line(unfinished_line.substr(0, line_end), running_line_end);
                unfinished_line.erase(0, line_end + 1);
                line_end = unfinished_line.find('\n');
            }
        }
        close(program.output_end);
        if (!unfinished_line.empty()) {
            take_line(std::move(unfinished_line), running_line_end);
        }
        // Waited for without being let go yet: until it is let go its number is no other
        // program's, so a stop asked for meanwhile can't reach a stranger.
        siginfo_t ending{};
        const bool is_ending_known =
            waitid(P_PID, static_cast<id_t>(program.process_id), &ending, WEXITED | WNOWAIT) == 0;
        const bool has_ended_well =
            is_ending_known && ending.si_code == CLD_EXITED && ending.si_status == 0;
        // A program told to stop before it had set itself up to stop in order dies of the signal.
        const bool has_died_of_the_stop =
            is_ending_known && ending.si_code == CLD_KILLED && ending.si_status == SIGINT;
        {
            const std::scoped_lock lock(mutex_);
            process_id_ = 0;
            if (state_ == RunState::Starting) {
                state_ = RunState::StartFailed;
            } else if (state_ == RunState::Stopping && has_died_of_the_stop) {
                state_ = RunState::None;
            } else {
                state_ = has_ended_well ? RunState::None : RunState::Failed;
            }
        }
        int status = 0;
        waitpid(program.process_id, &status, 0);
    }

    mutable std::mutex mutex_;
    RunState state_ = RunState::None;
    pid_t process_id_ = 0;  // 0 while no program runs
    std::vector<std::string> new_lines_;
    // The last member, so that it ends before what its thread uses is gone.
    std::jthread thread_;
};
