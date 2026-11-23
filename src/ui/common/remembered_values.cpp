#pragma once
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include "files.cpp"

// What the app remembers between two sessions: numbers by name, e.g. a panel's width, kept in one
// file as lines "name: value". The file is read once, when this object is made, and written
// beside the thread that changes a number.
class RememberedNumbers {
public:
    // Without a file nothing is read and nothing is kept: the numbers then last as long as this
    // object.
    explicit RememberedNumbers(std::optional<std::filesystem::path> file) : file_(std::move(file)) {
        if (!file_.has_value()) {
            return;
        }
        const std::string text = text_of_file(*file_).value_or(std::string());
        std::size_t line_start = 0;
        while (line_start < text.size()) {
            std::size_t line_end = text.find('\n', line_start);
            if (line_end == std::string::npos) {
                line_end = text.size();
            }
            read_line(std::string_view(text).substr(line_start, line_end - line_start));
            line_start = line_end + 1;
        }
    }

    // The number of this name, or the fallback where none is remembered.
    [[nodiscard]] float value_or(std::string_view name, float fallback) const {
        const auto found = numbers_.find(name);
        return found == numbers_.end() ? fallback : found->second;
    }

    // Notes the number of this name, and has the file written if that changed it.
    void remember(std::string_view name, float value) {
        const auto found = numbers_.find(name);
        if (found != numbers_.end() && found->second == value) {
            return;
        }
        numbers_.insert_or_assign(std::string(name), value);
        if (!file_.has_value()) {
            return;
        }
        std::string text;
        for (const auto& [number_name, number] : numbers_) {
            text += std::format("{}: {}\n", number_name, number);
        }
        writer_.write(*file_, std::move(text));
    }

private:
    // A line that isn't "name: value" with a number is skipped: a file that a person changed, or
    // one of a later Falcon, must not stop this one.
    void read_line(std::string_view line) {
        const std::size_t colon_position = line.find(':');
        if (colon_position == std::string_view::npos) {
            return;
        }
        std::string_view value_text = line.substr(colon_position + 1);
        const std::size_t value_start = value_text.find_first_not_of(' ');
        if (value_start == std::string_view::npos) {
            return;
        }
        value_text.remove_prefix(value_start);
        float value = 0.0f;
        const char* const value_begin = value_text.data();
        const char* const value_end = value_begin + value_text.size();
        const std::from_chars_result parsed = std::from_chars(value_begin, value_end, value);
        if (parsed.ec == std::errc() && parsed.ptr == value_end) {
            numbers_.insert_or_assign(std::string(line.substr(0, colon_position)), value);
        }
    }

    std::optional<std::filesystem::path> file_;
    std::map<std::string, float, std::less<>> numbers_;
    BackgroundFileWriter writer_;
};
