#pragma once
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// What is wrong with a pipeline that didn't compile, taken from what the compiler wrote, as the
// Problems panel lists it.

// A line of a file as a compiler names it: the file with its folders. Line 0 stands for no place.
struct CodePlace {
    std::string file;
    int line = 0;

    [[nodiscard]] bool operator==(const CodePlace&) const = default;
};

struct BuildProblem {
    std::string title;  // what is wrong, in a few words
    std::string body;   // the same in full, or empty where the title says it all
    std::string place;  // where the compiler found it, e.g. "recorder.cpp, line 98", or empty
    CodePlace found_at;
    // The line that asked for the code the problem was found in, e.g. the line of pipeline.cpp
    // that declares a processor whose own static_assert failed. No place where the compiler
    // names none.
    CodePlace required_from;
    // The processor it is about, by its name, which the designer then marks; empty where none is
    // known, as for all that a compiler says.
    std::string processor_name;

    [[nodiscard]] bool operator==(const BuildProblem&) const = default;
};

// The place that a compiler writes as "<file>:<line>:<column>", e.g.
// "/home/lab/falcon/src/processors/recorder.cpp:98:30" gives that file and line 98. None for
// anything else, e.g. the linker's name in front of its error.
[[nodiscard]] inline std::optional<CodePlace> place_written_as(std::string_view written) {
    const std::size_t column_colon = written.rfind(':');
    if (column_colon == std::string_view::npos || column_colon == 0) {
        return std::nullopt;
    }
    const std::size_t line_colon = written.rfind(':', column_colon - 1);
    if (line_colon == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view line_number =
        written.substr(line_colon + 1, column_colon - line_colon - 1);
    int line = 0;
    const char* const number_begin = line_number.data();
    const char* const number_end = number_begin + line_number.size();
    const std::from_chars_result parsed = std::from_chars(number_begin, number_end, line);
    if (line_number.empty() || parsed.ec != std::errc() || parsed.ptr != number_end) {
        return std::nullopt;
    }
    return CodePlace{.file = std::string(written.substr(0, line_colon)), .line = line};
}

// A place as the Problems panel words it, e.g. "recorder.cpp, line 98": the file without its
// folders.
[[nodiscard]] inline std::string text_of_place(const CodePlace& place) {
    return std::format("{}, line {}", std::filesystem::path(place.file).filename().string(),
                       place.line);
}

// The problem that one line of the compiler's text names, or none for a line that names none. An
// error is a line "<file>:<line>:<column>: error: <what>". Of a static_assert that failed, which
// a processor writes for its user, only the processor's own words are kept.
[[nodiscard]] inline std::optional<BuildProblem> problem_of_line(std::string_view line) {
    constexpr std::string_view error_mark = ": error: ";
    constexpr std::string_view fatal_error_mark = ": fatal error: ";
    constexpr std::string_view failed_assertion = "static assertion failed: ";
    std::size_t mark_position = line.find(fatal_error_mark);
    std::size_t mark_size = fatal_error_mark.size();
    if (mark_position == std::string_view::npos) {
        mark_position = line.find(error_mark);
        mark_size = error_mark.size();
    }
    if (mark_position == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view what = line.substr(mark_position + mark_size);
    if (what.starts_with(failed_assertion)) {
        what.remove_prefix(failed_assertion.size());
    }
    CodePlace found_at = place_written_as(line.substr(0, mark_position)).value_or(CodePlace{});
    return BuildProblem{
        .title = std::string(what),
        .body = {},
        .place = found_at.line == 0 ? std::string() : text_of_place(found_at),
        .found_at = std::move(found_at),
        .required_from = {},
        .processor_name = {},
    };
}

// The place that a line "<file>:<line>:<column>:   required from here" names, or none for any
// other line. A compiler writes such a line in front of an error in code that this place asked
// for.
[[nodiscard]] inline std::optional<CodePlace> place_required_from(std::string_view line) {
    constexpr std::string_view required_mark = "required from here";
    if (!line.ends_with(required_mark)) {
        return std::nullopt;
    }
    line.remove_suffix(required_mark.size());
    const std::size_t last_character = line.find_last_not_of(' ');
    if (last_character == std::string_view::npos || line.at(last_character) != ':') {
        return std::nullopt;
    }
    return place_written_as(line.substr(0, last_character));
}

// The problems that the text of a failed compilation names, each once, in the text's order. A
// text without a compiler's error is one problem as it is, e.g. the reason why a graph can't be
// written.
[[nodiscard]] inline std::vector<BuildProblem> problems_of(std::string_view build_text) {
    std::vector<BuildProblem> problems;
    // What the lines since the last error name as the place that the next error is required
    // from.
    CodePlace required_from;
    std::size_t line_start = 0;
    while (line_start < build_text.size()) {
        std::size_t line_end = build_text.find('\n', line_start);
        if (line_end == std::string_view::npos) {
            line_end = build_text.size();
        }
        const std::string_view line = build_text.substr(line_start, line_end - line_start);
        if (std::optional<CodePlace> named_place = place_required_from(line)) {
            required_from = std::move(*named_place);
        }
        std::optional<BuildProblem> problem = problem_of_line(line);
        if (problem.has_value()) {
            problem->required_from = std::exchange(required_from, CodePlace{});
            if (!std::ranges::contains(problems, *problem)) {
                problems.push_back(std::move(*problem));
            }
        }
        line_start = line_end + 1;
    }
    if (problems.empty() && !build_text.empty()) {
        problems.push_back({
            .title = std::string(build_text),
            .body = {},
            .place = {},
            .found_at = {},
            .required_from = {},
            .processor_name = {},
        });
    }
    return problems;
}

// The processor that a line of the pipeline's own file declares or calls. Empty for a place in
// another file and for a line of no processor. `processors_of_lines` holds the processor of
// each line of that file, by the line's number less one.
[[nodiscard]] inline std::string processor_at(const CodePlace& place,
                                              const std::filesystem::path& pipeline_file,
                                              const std::vector<std::string>& processors_of_lines) {
    const bool is_a_line_of_the_pipeline =
        place.line >= 1 && static_cast<std::size_t>(place.line) <= processors_of_lines.size() &&
        std::filesystem::path(place.file) == pipeline_file;
    return is_a_line_of_the_pipeline
               ? processors_of_lines.at(static_cast<std::size_t>(place.line) - 1)
               : std::string();
}

// Names the processor of every problem that has none yet and that a line of the pipeline's own
// file leads to: the line the problem was found on, or else the line it is required from.
inline void name_processors_of(std::vector<BuildProblem>& problems,
                               const std::filesystem::path& pipeline_file,
                               const std::vector<std::string>& processors_of_lines) {
    for (BuildProblem& problem : problems) {
        if (!problem.processor_name.empty()) {
            continue;
        }
        problem.processor_name = processor_at(problem.found_at, pipeline_file, processors_of_lines);
        if (problem.processor_name.empty()) {
            problem.processor_name =
                processor_at(problem.required_from, pipeline_file, processors_of_lines);
        }
    }
}
