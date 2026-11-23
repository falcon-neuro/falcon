#pragma once
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

// Reading and writing whole files, and writing one beside the UI's thread.

// A file's text, or none for a file that can't be read.
[[nodiscard]] inline std::optional<std::string> text_of_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// Writes a text as a file, in place of one that is there. Returns whether it was written.
[[nodiscard]] inline bool write_file(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    file.close();
    return !file.fail();
}

// Writes a file beside the thread that asks for it, so that a disk that is slow for a moment
// never holds the UI up. Only the latest text counts: one that still waits is replaced by a newer
// one. What waits when this object ends is written first.
class BackgroundFileWriter {
public:
    BackgroundFileWriter() : thread_([this](const std::stop_token& stop) { run(stop); }) {}

    // Has this text written as this file, in place of one that is there, and its folders made.
    void write(std::filesystem::path path, std::string text) {
        {
            const std::scoped_lock lock(mutex_);
            waiting_ = WaitingFile{.path = std::move(path), .text = std::move(text)};
        }
        wake_.notify_one();
    }

    // Whether the last file that was written couldn't be.
    [[nodiscard]] bool has_failed() const { return has_failed_.load(); }

private:
    struct WaitingFile {
        std::filesystem::path path;
        std::string text;
    };

    void run(const std::stop_token& stop) {
        while (true) {
            WaitingFile file;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, stop, [this] { return waiting_.has_value(); });
                if (!waiting_.has_value()) {
                    return;
                }
                file = std::move(*waiting_);
                waiting_.reset();
            }
            std::error_code error;
            std::filesystem::create_directories(file.path.parent_path(), error);
            has_failed_.store(!write_file(file.path, file.text));
        }
    }

    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::optional<WaitingFile> waiting_;
    std::atomic<bool> has_failed_ = false;
    // The last member, so that it ends before what its thread uses is gone.
    std::jthread thread_;
};
