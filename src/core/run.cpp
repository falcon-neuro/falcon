#pragma once
#include <cxxabi.h>
#include <pmmintrin.h>
#include <pthread.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#include <xmmintrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <latch>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <typeinfo>
#include <vector>
#include "rings.cpp"
#include "run_log_lines.cpp"

// Engine, part 5 of 5: a run's cores, names and directory, its reporting, and its start and stop.

// --- Cores ---

// Set when a constructor threw on any core, or a core's thread couldn't be started.
std::atomic<bool> has_startup_failed = false;
// Set when a processor threw once its core's loop had started, which ended the run.
std::atomic<bool> has_run_failed = false;
std::latch* start_barrier = nullptr;  // created by run_on_cores()
thread_local bool has_arrived_at_start_barrier = false;
thread_local int core_number_of_this_thread = -1;  // set by run_on_cores()

// --- Processor names ---

// The names of the processors that exist, touched only while processors are built or destroyed.
std::mutex registered_names_mutex;
std::vector<std::string_view> registered_names;

// A processor's `name` member. While it exists, no other processor can take the name, so every
// line names one processor. It can't be copied, since a copy would register the name again.
class RegisteredName {
public:
    explicit RegisteredName(ProcessorName name) : name_(name) {
        const std::string_view text = name.c_str();
        const std::scoped_lock lock(registered_names_mutex);
        if (std::ranges::contains(registered_names, text)) {
            throw std::runtime_error(std::format("two processors are named {}", text));
        }
        registered_names.push_back(text);
    }

    ~RegisteredName() {
        const std::scoped_lock lock(registered_names_mutex);
        std::erase(registered_names, std::string_view(name_.c_str()));
    }

    RegisteredName(const RegisteredName&) = delete;
    RegisteredName& operator=(const RegisteredName&) = delete;
    RegisteredName(RegisteredName&&) = delete;
    RegisteredName& operator=(RegisteredName&&) = delete;

    // Implicit, so a processor passes its member straight to warn(), log() or fail().
    operator ProcessorName() const { return name_; }

private:
    ProcessorName name_;
};

template <typename ProcessorType>
concept Processor = requires(const ProcessorType& processor) {
    { processor.name } -> std::same_as<const RegisteredName&>;
};

// --- Runs ---

// The command line a program was started with, as main() gets it and hands it to run_on_cores().
struct CommandLine {
    int argument_count = 0;
    char** arguments = nullptr;
};

// What a run is started with on its command line.
struct RunArguments {
    // What stands in front of the date and time in the name of the run's directory, so that the
    // runs of one pipeline can be told apart, e.g. hippocampus_stimulation. Empty for none.
    std::string run_name;
};

inline constexpr std::string_view RUN_NAME_ARGUMENT = "--run-name";

// Reads a program's command line: `--run-name <name>`, and nothing else. A run's name is part of
// its directory's name, so it holds only letters, digits, _ and -.
[[nodiscard]] RunArguments read_run_arguments(const CommandLine& command_line) {
    RunArguments run_arguments;
    const std::span<char*> arguments(
        command_line.arguments, static_cast<std::size_t>(std::max(command_line.argument_count, 0)));
    // The first one is the program's own name.
    for (std::size_t argument_index = 1; argument_index < arguments.size(); ++argument_index) {
        const std::string_view argument = arguments[argument_index];
        const bool is_followed_by_a_value = argument_index + 1 < arguments.size();
        if (argument != RUN_NAME_ARGUMENT || !is_followed_by_a_value) {
            throw std::runtime_error(
                std::format("can't read the argument {}: the program takes {} <name>, and "
                            "nothing else",
                            argument, RUN_NAME_ARGUMENT));
        }
        ++argument_index;
        run_arguments.run_name = arguments[argument_index];
        if (run_arguments.run_name.empty()) {
            throw std::runtime_error("a run's name can't be empty");
        }
    }
    const bool is_name_for_a_directory =
        std::ranges::all_of(run_arguments.run_name, [](char character) {
            return is_letter_or_digit(character) || character == '_' || character == '-';
        });
    if (!is_name_for_a_directory) {
        throw std::runtime_error("a run's name holds only letters, digits, _ and -");
    }
    return run_arguments;
}

// Who a run's directory and files belong to. Under sudo, the engine runs as root, but they go to
// the user who ran sudo.
struct RunOwner {
    uid_t user_id = 0;
    gid_t group_id = 0;
    bool is_sudo = false;
};

[[nodiscard]] RunOwner find_run_owner() {
    const char* const sudo_user = std::getenv("SUDO_USER");
    const bool is_sudo = geteuid() == 0 && sudo_user != nullptr && sudo_user[0] != '\0';
    constexpr std::size_t entry_text_capacity = 16384;
    std::array<char, entry_text_capacity> entry_text{};
    passwd entry{};
    passwd* found_entry = nullptr;
    const int error_number =
        is_sudo ? getpwnam_r(sudo_user, &entry, entry_text.data(), entry_text.size(), &found_entry)
                : getpwuid_r(getuid(), &entry, entry_text.data(), entry_text.size(), &found_entry);
    if (error_number != 0 || found_entry == nullptr) {
        throw std::runtime_error(
            is_sudo ? std::format("can't find the user {}, who ran sudo", sudo_user)
                    : std::string("can't find the current user"));
    }
    return {
        .user_id = entry.pw_uid,
        .group_id = entry.pw_gid,
        .is_sudo = is_sudo,
    };
}

void give_to_owner(const std::filesystem::path& path, const RunOwner& owner) {
    if (owner.is_sudo && chown(path.c_str(), owner.user_id, owner.group_id) != 0) {
        throw std::runtime_error(
            std::format("can't give {} to the user who ran sudo", path.string()));
    }
}

void make_missing_directory(const std::filesystem::path& path, const RunOwner& owner) {
    if (std::filesystem::create_directory(path)) {
        give_to_owner(path, owner);
    }
}

// The folder that the running program's file lies in.
[[nodiscard]] std::filesystem::path program_directory() {
    return std::filesystem::read_symlink("/proc/self/exe").parent_path();
}

// <the program's folder>/runs/<run name>_<date>_<time>/, or <date>_<time>/ for a run without a
// name; new for every run. Beside the program, so that a pipeline's runs lie in its directory
// wherever that directory is.
[[nodiscard]] std::filesystem::path make_run_directory(const RunArguments& run_arguments,
                                                       const RunOwner& owner) {
    const std::filesystem::path runs_directory = program_directory() / "runs";
    make_missing_directory(runs_directory, owner);

    const std::time_t now = std::time(nullptr);
    std::tm local_time{};
    if (localtime_r(&now, &local_time) == nullptr) {
        throw std::runtime_error("can't read the local time for the run's directory");
    }
    constexpr int tm_first_year = 1900;
    const std::filesystem::path run_directory_path =
        runs_directory /
        std::format("{}{}{:04}-{:02}-{:02}_{:02}-{:02}-{:02}", run_arguments.run_name,
                    run_arguments.run_name.empty() ? "" : "_", local_time.tm_year + tm_first_year,
                    local_time.tm_mon + 1, local_time.tm_mday, local_time.tm_hour,
                    local_time.tm_min, local_time.tm_sec);
    // A run never writes into an earlier run's directory.
    if (!std::filesystem::create_directory(run_directory_path)) {
        throw std::runtime_error(
            std::format("the run's directory {} exists already", run_directory_path.string()));
    }
    give_to_owner(run_directory_path, owner);
    return run_directory_path;
}

// Set by run_on_cores() before any core's thread starts, so every constructor and destructor may
// use it.
std::filesystem::path run_directory_path;

// A file option is a name inside the run's directory, so it can't lead out of it.
[[nodiscard]] constexpr bool is_file_name(const Str& text) {
    const std::string_view name = text.c_str();
    return !name.empty() && name != "." && name != ".." && !name.contains('/');
}

// Where a processor writes its files; a file option is a name inside it.
[[nodiscard]] const std::filesystem::path& run_directory() {
    if (run_directory_path.empty()) {
        throw std::logic_error("run_directory() is valid only once run_on_cores() has made it");
    }
    return run_directory_path;
}

// Under sudo, processors make their files as root, so these go to the owner once the run ends.
void give_run_files_to_owner(const std::filesystem::path& directory,
                             const RunOwner& owner) noexcept {
    if (!owner.is_sudo) {
        return;
    }
    bool has_failed = false;
    try {
        std::error_code listing_error;
        for (std::filesystem::recursive_directory_iterator entry(directory, listing_error);
             !listing_error && entry != std::filesystem::recursive_directory_iterator();
             entry.increment(listing_error)) {
            if (chown(entry->path().c_str(), owner.user_id, owner.group_id) != 0) {
                has_failed = true;
            }
        }
        has_failed = has_failed || static_cast<bool>(listing_error);
    } catch (...) {
        has_failed = true;
    }
    if (has_failed) {
        std::fputs("falcon: can't give every file of the run to the user who ran sudo\n", stderr);
    }
}

// --- Reporting ---

// Main thread only.
void report_ring_drops() noexcept {
    for (RingDropCounter* drop_counter = first_ring_drop_counter; drop_counter != nullptr;
         drop_counter = drop_counter->next_ring) {
        const u64 dropped_count = drop_counter->dropped_count.load(std::memory_order_relaxed);
        if (dropped_count > drop_counter->reported_count) {
            log_as(MessageType::Warning, drop_counter->ring_name,
                   "the ring is full: dropped {} {} ({} in total)",
                   dropped_count - drop_counter->reported_count, drop_counter->dropped_kind,
                   dropped_count);
            drop_counter->reported_count = dropped_count;
        }
    }
}

void report_everything(std::span<MessageChannelReader> readers) noexcept {
    report_messages(readers);
    report_ring_drops();
    logger.flush();
}

// --- Start and stop ---

// Every core comes here once its processors are built: its first next_pass() calls this. Returns
// false if startup failed on any core, so the core returns without running its loop. The thread
// pins itself to its core only now: a thread that a library starts in a constructor (e.g. to
// receive packets) inherits its creator's CPUs, and on the realtime core it would compete with a
// loop that never yields.
bool wait_at_start_barrier() {
    // A core's function may have called this itself in front of its loop, and the barrier counts
    // each core once.
    if (has_arrived_at_start_barrier) {
        return !has_startup_failed;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(core_number_of_this_thread), &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
        throw std::runtime_error(std::format("can't pin to CPU {}", core_number_of_this_thread));
    }
    has_arrived_at_start_barrier = true;
    start_barrier->arrive_and_wait();
    is_in_loop_of_this_thread = !has_startup_failed;
    return !has_startup_failed;
}

struct CoreThread {
    int core_number;        // CPU core the thread pins itself to in wait_at_start_barrier()
    void (*entry_point)();  // declares the core's processors and buffers, then runs its loop
    // False for a core whose work can wait, e.g. one that writes files: its loop sleeps after a
    // pass that brought it nothing, where a realtime core spins.
    bool is_realtime = true;
};

// The stop signals: SIGINT (Ctrl+C) and SIGTERM.
sigset_t stop_signals() {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    return signals;
}

// Waits for a stop signal while the cores run, then clears `is_pipeline_running`, reporting the
// cores' messages and the rings' drops at every wake-up. The timeout ends the wait when
// `is_pipeline_running` was cleared without a signal (a processor threw). Says once that the
// pipeline is running, at the first wake-up after every core has passed the start barrier.
void wait_for_stop_signal(const sigset_t& signals, std::span<MessageChannelReader> readers) {
    bool is_running_reported = false;
    while (is_pipeline_running) {
        const timespec timeout{.tv_sec = 0, .tv_nsec = 100'000'000};  // MESSAGE_INTERVAL_SECONDS
        const int signal_number = sigtimedwait(&signals, nullptr, &timeout);
        if (!is_running_reported && start_barrier->try_wait() && !has_startup_failed) {
            log_engine(MessageType::Log, PIPELINE_IS_RUNNING_LINE);
            is_running_reported = true;
        }
        if (signal_number > 0) {
            log_engine(MessageType::Log, "{} received, stopping",
                       signal_number == SIGINT ? "Ctrl+C" : "SIGTERM");
            is_pipeline_running = false;
        }
        report_everything(readers);
    }
}

// Reads the command line, makes the run's directory, opens its log file, and makes one message
// channel per core. Returns false if the run can't start.
[[nodiscard]] bool start_run(const CommandLine& command_line, std::size_t core_count,
                             RunOwner& owner,
                             std::vector<std::unique_ptr<MessageChannel>>& channels) noexcept {
    try {
        start_run_clock();
        const RunArguments run_arguments = read_run_arguments(command_line);
        owner = find_run_owner();
        run_directory_path = make_run_directory(run_arguments, owner);
        const std::filesystem::path log_path = run_directory_path / "falcon.log";
        logger.open(log_path);
        give_to_owner(log_path, owner);
        for (std::size_t core_index = 0; core_index < core_count; ++core_index) {
            channels.push_back(std::make_unique<MessageChannel>());
        }
        return true;
    } catch (const std::exception& error) {
        std::fputs("falcon: the run can't start: ", stderr);
        std::fputs(error.what(), stderr);
        std::fputs("\n", stderr);
        return false;
    }
}

// A processor threw: a constructor, so the graph doesn't start, or in the loop, which ends the run.
void end_run_after_core_failure() noexcept {
    if (has_arrived_at_start_barrier) {
        has_run_failed = true;
    } else {
        has_startup_failed = true;
    }
    is_pipeline_running = false;
    if (!has_arrived_at_start_barrier) {
        start_barrier->count_down();  // release the cores waiting at the barrier
    }
}

// For an exception that fail() didn't throw, so no line reports it yet.
void handle_core_failure(int core_number, std::string_view description) noexcept {
    log_engine(MessageType::Error, "core {}: {}", core_number, description);
    end_run_after_core_failure();
}

// An exception that isn't a std::exception has no message, so its line names its type.
void handle_core_failure_without_message(int core_number, const std::type_info& type) noexcept {
    int status = 0;
    char* const readable_name = abi::__cxa_demangle(type.name(), nullptr, nullptr, &status);
    try {
        handle_core_failure(
            core_number,
            std::format("threw an exception of type {}, which isn't a std::exception, so it has "
                        "no message",
                        readable_name != nullptr ? readable_name : type.name()));
    } catch (...) {
        handle_core_failure(core_number, "threw an exception that isn't a std::exception");
    }
    std::free(readable_name);
}

// Runs one pinned thread per core until stopped. argument_count and arguments are main()'s, which
// say what the run is started with (see read_run_arguments()). Returns 1 if the graph didn't start
// or a processor ended the run, 0 otherwise. No exception leaves run_on_cores().
int run_on_cores(int argument_count, char** arguments,
                 std::initializer_list<CoreThread> core_threads) {
    // Blocked before any core's thread starts, so every thread inherits the block and no stop
    // signal ever interrupts a core's loop. Only wait_for_stop_signal() takes them.
    const sigset_t signals = stop_signals();
    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        std::fputs("falcon: can't block the stop signals\n", stderr);
        return 1;
    }

    RunOwner owner;
    std::vector<std::unique_ptr<MessageChannel>> channels;
    if (!start_run({.argument_count = argument_count, .arguments = arguments}, core_threads.size(),
                   owner, channels)) {
        return 1;
    }
    log_engine(MessageType::Log, "the run starts, writing into {}", run_directory_path.c_str());

    std::vector<MessageChannelReader> readers;
    std::latch latch(static_cast<std::ptrdiff_t>(core_threads.size()));
    start_barrier = &latch;

    std::vector<std::thread> threads;
    try {
        readers.reserve(core_threads.size());
        threads.reserve(core_threads.size());
        for (std::size_t core_index = 0; const CoreThread core_thread : core_threads) {
            MessageChannel* const channel = channels[core_index].get();
            readers.push_back({.channel = channel, .core_number = core_thread.core_number});
            threads.emplace_back([core_thread, channel] {
                try {
                    core_number_of_this_thread = core_thread.core_number;
                    is_realtime_core_of_this_thread = core_thread.is_realtime;
                    message_channel_of_this_thread = channel;
                    // Denormals, e.g. a filter's state on a flat channel, are far slower.
                    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
                    // Written out, not _MM_SET_DENORMALS_ZERO_MODE: GCC's macro of that name mixes
                    // signed and unsigned values, which the build's warnings refuse.
                    _mm_setcsr(_mm_getcsr() | _MM_DENORMALS_ZERO_ON);
                    core_thread.entry_point();
                } catch (const ProcessorFailure&) {
                    end_run_after_core_failure();
                } catch (const std::exception& error) {
                    handle_core_failure(core_thread.core_number, error.what());
                } catch (...) {
                    const std::type_info* const type = abi::__cxa_current_exception_type();
                    if (type == nullptr) {
                        // Not a C++ exception, e.g. the unwinding of a cancelled thread, which
                        // must go on.
                        throw;
                    }
                    handle_core_failure_without_message(core_thread.core_number, *type);
                }
            });
            ++core_index;
        }
    } catch (const std::exception& error) {
        // A core's thread couldn't be started. Count down once for every core that never started,
        // so the started ones pass the start barrier, see has_startup_failed and return.
        log_engine(MessageType::Error, "can't start a core's thread: {}", error.what());
        has_startup_failed = true;
        is_pipeline_running = false;
        for (std::size_t missing = threads.size(); missing < core_threads.size(); ++missing) {
            latch.count_down();
        }
    }

    wait_for_stop_signal(signals, readers);
    for (std::thread& thread : threads) {
        thread.join();
    }
    // What the loops raised after the last poll.
    report_everything(readers);
    if (has_startup_failed) {
        log_engine(MessageType::Error, "the pipeline didn't start");
    } else if (has_run_failed) {
        log_engine(MessageType::Error, "the run ended because a processor failed");
    } else {
        log_engine(MessageType::Log, "the run ended");
    }
    logger.close();
    give_run_files_to_owner(run_directory_path, owner);
    return (has_startup_failed || has_run_failed) ? 1 : 0;
}
