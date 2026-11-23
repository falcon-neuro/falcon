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

// What the app remembers between two sessions: values by name, e.g. a panel's width or the
// pipeline that is open, kept in one file as lines "name: value". The file is read once, when
// this object is made, and written beside the thread that changes a value.
class RememberedValues {
public:
    // Without a file nothing is read and nothing is kept: the values then last as long as this
    // object.
    explicit RememberedValues(std::optional<std::filesystem::path> file) : file_(std::move(file)) {
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

    // The number of this name, or the fallback where none is remembered or what is remembered
    // under the name is no number.
    [[nodiscard]] float number_or(std::string_view name, float fallback) const {
        const auto found = values_.find(name);
        if (found == values_.end()) {
            return fallback;
        }
        const std::string& value_text = found->second;
        float value = 0.0f;
        const char* const value_begin = value_text.data();
        const char* const value_end = value_begin + value_text.size();
        const std::from_chars_result parsed = std::from_chars(value_begin, value_end, value);
        return parsed.ec == std::errc() && parsed.ptr == value_end ? value : fallback;
    }

    // The text of this name, or none where none is remembered.
    [[nodiscard]] std::optional<std::string> text_of(std::string_view name) const {
        const auto found = values_.find(name);
        if (found == values_.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    // Notes the number of this name, and has the file written if that changed it.
    void remember_number(std::string_view name, float value) {
        remember_text(name, std::format("{}", value));
    }

    // Notes the text of this name, and has the file written if that changed it. A text that a
    // line of the file can't hold isn't noted: an empty one, one of several lines, or one that
    // starts with a space, which reading the line back would drop.
    void remember_text(std::string_view name, std::string_view text) {
        if (text.empty() || text.contains('\n') || text.starts_with(' ')) {
            return;
        }
        const auto found = values_.find(name);
        if (found != values_.end() && found->second == text) {
            return;
        }
        values_.insert_or_assign(std::string(name), std::string(text));
        write();
    }

    // Forgets the value of this name, and has the file written if there was one.
    void forget(std::string_view name) {
        const auto found = values_.find(name);
        if (found == values_.end()) {
            return;
        }
        values_.erase(found);
        write();
    }

private:
    // A line that isn't "name: value" is skipped: a file that a person changed, or one of a
    // later Falcon, must not stop this one.
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
        values_.insert_or_assign(std::string(line.substr(0, colon_position)),
                                 std::string(value_text));
    }

    void write() {
        if (!file_.has_value()) {
            return;
        }
        std::string text;
        for (const auto& [name, value] : values_) {
            text += std::format("{}: {}\n", name, value);
        }
        writer_.write(*file_, std::move(text));
    }

    std::optional<std::filesystem::path> file_;
    std::map<std::string, std::string, std::less<>> values_;
    BackgroundFileWriter writer_;
};
