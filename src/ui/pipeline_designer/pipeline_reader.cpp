#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "pipeline_graph.cpp"
#include "pipeline_writer.cpp"
#include "processor_list.cpp"

// Reads a pipeline.cpp back into the graph it was written from. It takes the one form the writer
// writes, however the file is laid out, also where a person wrote it. What it can't read, it
// refuses with the line and the reason: it never reads a guess.

// The graph that was read, or why the file can't be read.
using ReadPipeline = std::expected<PipelineGraph, std::string>;

// Why a pipeline.cpp can't be read. Thrown where the reading stops, and caught by read_pipeline().
class PipelineRefusal : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] inline void refuse_at(std::size_t line, std::string_view reason) {
    throw PipelineRefusal(std::format("line {}: {}", line, reason));
}

// --- Tokens ---

enum class TokenKind : std::uint8_t {
    Word,    // a name, a keyword, or the digits of a number
    Text,    // a text between double quotes, with its quotes
    Symbol,  // any other character, each one alone
};

struct Token {
    TokenKind kind = TokenKind::Symbol;
    std::string_view text;  // as written
    std::size_t line = 0;
    std::size_t start = 0;  // where it starts in the text it was read from
};

[[nodiscard]] inline std::size_t end_of(const Token& token) {
    return token.start + token.text.size();
}

// A place in a text, with the line of the file it is on.
struct SourcePlace {
    std::size_t position = 0;
    std::size_t line = 0;
};

// Where the line that holds a position ends: at its line break, or at the text's end.
[[nodiscard]] inline std::size_t line_end_from(std::string_view source, std::size_t position) {
    const std::size_t line_break = source.find('\n', position);
    return line_break == std::string_view::npos ? source.size() : line_break;
}

// Where a text between double quotes ends, one past its closing quote.
[[nodiscard]] inline std::size_t quoted_text_end(std::string_view source, SourcePlace open_quote) {
    std::size_t position = open_quote.position + 1;
    while (position < source.size() && source.at(position) != '"' && source.at(position) != '\n') {
        // A backslash takes the character after it along, e.g. a quote that doesn't close the text.
        position += source.at(position) == '\\' ? std::size_t{2} : std::size_t{1};
    }
    if (position >= source.size() || source.at(position) != '"') {
        refuse_at(open_quote.line, "a text in double quotes isn't closed on its line");
    }
    return position + 1;
}

[[nodiscard]] inline TokenKind token_kind_starting_with(char character) {
    if (character == '"') {
        return TokenKind::Text;
    }
    return is_identifier_character(character) ? TokenKind::Word : TokenKind::Symbol;
}

[[nodiscard]] inline std::size_t token_end_from(std::string_view source, SourcePlace token_start) {
    switch (token_kind_starting_with(source.at(token_start.position))) {
        case TokenKind::Text:
            return quoted_text_end(source, token_start);
        case TokenKind::Word:
            return word_end_from(source, token_start.position);
        case TokenKind::Symbol:
            return token_start.position + 1;
    }
    return token_start.position + 1;
}

// Where a comment that starts with /* ends, one past its closing */.
[[nodiscard]] inline std::size_t block_comment_end(std::string_view source,
                                                   SourcePlace comment_start) {
    const std::size_t close_position = source.find("*/", comment_start.position + 2);
    if (close_position == std::string_view::npos) {
        refuse_at(comment_start.line, "a comment that starts with /* isn't closed");
    }
    return close_position + 2;
}

// A text's tokens, without its comments and without the lines that start with #, such as an
// include. first_line is the line of the file that the text starts on, which the tokens' lines
// count from.
[[nodiscard]] inline std::vector<Token> tokens_of(std::string_view source, std::size_t first_line) {
    std::vector<Token> tokens;
    std::size_t line = first_line;
    std::size_t position = 0;
    bool is_first_on_line = true;
    while (position < source.size()) {
        const char character = source.at(position);
        const std::string_view rest = source.substr(position);
        if (character == '\n') {
            ++line;
            ++position;
            is_first_on_line = true;
        } else if (character == ' ' || character == '\t' || character == '\r') {
            ++position;
        } else if (rest.starts_with("//") || (character == '#' && is_first_on_line)) {
            position = line_end_from(source, position);
        } else if (rest.starts_with("/*")) {
            const std::size_t comment_end =
                block_comment_end(source, {.position = position, .line = line});
            const std::string_view comment = source.substr(position, comment_end - position);
            line += static_cast<std::size_t>(std::ranges::count(comment, '\n'));
            position = comment_end;
        } else {
            const std::size_t token_end =
                token_end_from(source, {.position = position, .line = line});
            tokens.push_back({
                .kind = token_kind_starting_with(character),
                .text = source.substr(position, token_end - position),
                .line = line,
                .start = position,
            });
            position = token_end;
            is_first_on_line = false;
        }
    }
    return tokens;
}

// Tokens that are read one after the other.
struct TokenReader {
    std::span<const Token> tokens;
    std::size_t next_index = 0;
};

[[nodiscard]] inline bool is_at_end(const TokenReader& reader) {
    return reader.next_index >= reader.tokens.size();
}

// The line of the token that is read next; at the end, that of the last token.
[[nodiscard]] inline std::size_t line_of_next(const TokenReader& reader) {
    if (reader.tokens.empty()) {
        return 1;
    }
    return reader.tokens[std::min(reader.next_index, reader.tokens.size() - 1)].line;
}

// The token that is read next, without reading it.
[[nodiscard]] inline const Token& peek(const TokenReader& reader) {
    if (is_at_end(reader)) {
        refuse_at(line_of_next(reader), "the text ends where more was expected");
    }
    return reader.tokens[reader.next_index];
}

[[nodiscard]] inline bool next_is(const TokenReader& reader, std::string_view text) {
    return !is_at_end(reader) && reader.tokens[reader.next_index].text == text;
}

inline Token take(TokenReader& reader) {
    const Token token = peek(reader);
    ++reader.next_index;
    return token;
}

inline void expect(TokenReader& reader, std::string_view text) {
    const Token token = take(reader);
    if (token.text != text) {
        refuse_at(token.line, std::format("expected `{}` and found `{}`", text, token.text));
    }
}

inline void expect_all(TokenReader& reader, std::initializer_list<std::string_view> texts) {
    for (const std::string_view text : texts) {
        expect(reader, text);
    }
}

// The word that is read next. `what` says what it is for, for the refusal.
[[nodiscard]] inline std::string take_word(TokenReader& reader, std::string_view what) {
    const Token token = take(reader);
    if (token.kind != TokenKind::Word) {
        refuse_at(token.line, std::format("expected {} and found `{}`", what, token.text));
    }
    return std::string(token.text);
}

// A name with the namespaces in front of it, e.g. "ripple_neuromed_reader::RippleNeuromedInput".
[[nodiscard]] inline std::string take_qualified_name(TokenReader& reader, std::string_view what) {
    std::string name = take_word(reader, what);
    while (next_is(reader, ":")) {
        expect_all(reader, {":", ":"});
        name += "::" + take_word(reader, what);
    }
    return name;
}

[[nodiscard]] inline bool is_opening_bracket(const Token& token) {
    return token.kind == TokenKind::Symbol &&
           (token.text == "{" || token.text == "(" || token.text == "[");
}

[[nodiscard]] inline bool is_closing_bracket(const Token& token) {
    return token.kind == TokenKind::Symbol &&
           (token.text == "}" || token.text == ")" || token.text == "]");
}

// The tokens between a `{` and the `}` that closes it, without the two.
[[nodiscard]] inline std::vector<Token> take_braced_tokens(TokenReader& reader) {
    expect(reader, "{");
    std::vector<Token> inside;
    int depth = 1;
    while (true) {
        const Token token = take(reader);
        if (is_opening_bracket(token)) {
            ++depth;
        } else if (is_closing_bracket(token)) {
            --depth;
        }
        if (depth == 0) {
            return inside;
        }
        inside.push_back(token);
    }
}

// A type's template arguments, each as written, from the `<` that is read next to the `>` that
// closes it. An angle bracket inside braces or parentheses is part of a value and closes nothing.
[[nodiscard]] inline std::vector<std::string> take_template_arguments(TokenReader& reader,
                                                                      std::string_view source) {
    expect(reader, "<");
    std::vector<std::string> arguments;
    int angle_depth = 1;
    int inner_depth = 0;
    std::size_t argument_start = std::string_view::npos;
    std::size_t argument_end = 0;
    const auto end_argument = [&](std::size_t line) {
        if (argument_start == std::string_view::npos) {
            refuse_at(line, "a template argument is empty");
        }
        arguments.emplace_back(source.substr(argument_start, argument_end - argument_start));
        argument_start = std::string_view::npos;
    };
    while (true) {
        const Token token = take(reader);
        const bool is_outside_brackets = inner_depth == 0 && token.kind == TokenKind::Symbol;
        if (is_opening_bracket(token)) {
            ++inner_depth;
        } else if (is_closing_bracket(token)) {
            --inner_depth;
        } else if (is_outside_brackets && token.text == "<") {
            ++angle_depth;
        } else if (is_outside_brackets && token.text == ">") {
            --angle_depth;
            if (angle_depth == 0) {
                end_argument(token.line);
                return arguments;
            }
        } else if (is_outside_brackets && token.text == "," && angle_depth == 1) {
            end_argument(token.line);
            continue;
        }
        if (argument_start == std::string_view::npos) {
            argument_start = token.start;
        }
        argument_end = end_of(token);
    }
}

// --- What a file says, before it is held against the kinds of processor ---

// A variable that a core's function declares: a processor or a buffer.
struct ReadDeclaration {
    std::string type_name;
    std::vector<std::string> template_arguments;  // each as written
    std::string variable_name;
    std::vector<Token> initializer;  // what stands in the braces after the name, if any
    std::size_t line = 0;
};

// A port that a call gives a buffer.
struct ReadPortBuffer {
    std::string port_name;
    std::string buffer_name;
};

// A call of a processor in a core's loop.
struct ReadCall {
    std::string processor_name;
    std::vector<ReadPortBuffer> port_buffers;
    std::size_t line = 0;
};

struct ReadCore {
    std::string function_name;
    std::vector<ReadDeclaration> declarations;
    std::vector<ReadCall> calls;
    std::size_t line = 0;
};

// One core of the list that main() gives to run_on_cores().
struct ReadCoreEntry {
    std::optional<int> core_number;
    std::string function_name;
    bool is_realtime = true;
    std::size_t line = 0;
};

// A stream that the file names with `using`, for a processor to start.
struct ReadStream {
    std::string name;
    GraphStream stream;
    std::size_t line = 0;
};

struct ReadFile {
    std::vector<ReadStream> streams;
    std::vector<ReadDeclaration> rings;  // declared outside the cores' functions
    std::vector<ReadCore> cores;
    std::vector<ReadCoreEntry> entries;
    bool has_main = false;
};

[[nodiscard]] inline ReadDeclaration read_declaration(TokenReader& reader,
                                                      std::string_view source) {
    ReadDeclaration declaration;
    declaration.line = line_of_next(reader);
    declaration.type_name = take_qualified_name(reader, "a type");
    if (next_is(reader, "<")) {
        declaration.template_arguments = take_template_arguments(reader, source);
    }
    declaration.variable_name = take_word(reader, "a variable's name");
    if (next_is(reader, "{")) {
        declaration.initializer = take_braced_tokens(reader);
    }
    expect(reader, ";");
    return declaration;
}

[[nodiscard]] inline ReadCall read_call(TokenReader& reader) {
    ReadCall call;
    call.line = line_of_next(reader);
    call.processor_name = take_word(reader, "a processor's name");
    expect_all(reader, {".", "process", "(", "{"});
    while (!next_is(reader, "}")) {
        expect(reader, ".");
        std::string port_name = take_word(reader, "a port's name");
        expect(reader, "=");
        if (next_is(reader, "{")) {
            refuse_at(line_of_next(reader),
                      "the reader doesn't read a port that is given several buffers");
        }
        std::string buffer_name = take_word(reader, "a buffer's name");
        call.port_buffers.push_back(
            {.port_name = std::move(port_name), .buffer_name = std::move(buffer_name)});
        if (!next_is(reader, "}")) {
            expect(reader, ",");
        }
    }
    expect_all(reader, {"}", ")", ";"});
    return call;
}

// A core's function, from its name on: its declarations, then its loop.
[[nodiscard]] inline ReadCore read_core(TokenReader& reader, std::string_view source) {
    ReadCore core;
    core.line = line_of_next(reader);
    core.function_name = take_word(reader, "a function's name");
    expect_all(reader, {"(", ")", "{"});
    while (!next_is(reader, "while")) {
        core.declarations.push_back(read_declaration(reader, source));
    }
    expect_all(reader, {"while", "(", "next_pass", "(", ")", ")", "{"});
    while (!next_is(reader, "}")) {
        core.calls.push_back(read_call(reader));
    }
    expect_all(reader, {"}", "}"});
    return core;
}

// The number that a text of digits alone says, or none for any other text. What is read this way
// (a core's number, a channel count, a ring's slots) has few digits, so a longer text is none too.
[[nodiscard]] inline std::optional<int> whole_number_of(std::string_view text) {
    constexpr std::size_t most_digits = 6;
    const bool is_digits_alone =
        !text.empty() && text.size() <= most_digits &&
        std::ranges::all_of(text, [](char digit) { return digit >= '0' && digit <= '9'; });
    if (!is_digits_alone) {
        return std::nullopt;
    }
    int number = 0;
    for (const char digit : text) {
        number = (number * 10) + (digit - '0');
    }
    return number;
}

// Sets what one ".key = value" of a core's entry says.
inline void set_core_entry_value(ReadCoreEntry& entry, std::string_view key, const Token& value) {
    if (key == "core_number") {
        entry.core_number = whole_number_of(value.text);
        if (!entry.core_number.has_value()) {
            refuse_at(value.line, std::format("`{}` is no core's number", value.text));
        }
    } else if (key == "entry_point") {
        entry.function_name = std::string(value.text);
    } else if (key == "is_realtime" && (value.text == "true" || value.text == "false")) {
        entry.is_realtime = value.text == "true";
    } else {
        refuse_at(value.line,
                  std::format("the reader doesn't read `.{} = {}` of a core", key, value.text));
    }
}

[[nodiscard]] inline ReadCoreEntry read_core_entry(TokenReader& reader) {
    ReadCoreEntry entry;
    entry.line = line_of_next(reader);
    expect(reader, "{");
    while (!next_is(reader, "}")) {
        expect(reader, ".");
        const std::string key = take_word(reader, "a name such as core_number");
        expect(reader, "=");
        set_core_entry_value(entry, key, take(reader));
        if (!next_is(reader, "}")) {
            expect(reader, ",");
        }
    }
    expect(reader, "}");
    if (!entry.core_number.has_value() || entry.function_name.empty()) {
        refuse_at(entry.line, "a core needs its core_number and its entry_point");
    }
    return entry;
}

// main(), from its name on: the cores it gives to run_on_cores().
[[nodiscard]] inline std::vector<ReadCoreEntry> read_main(TokenReader& reader) {
    expect_all(reader, {"main", "("});
    while (!next_is(reader, ")")) {
        take(reader);
    }
    expect_all(reader, {")", "{", "return", "run_on_cores", "("});
    // What stands in front of the cores is what the run is started with: main()'s arguments, or a
    // run's name where the file's engine takes one there. A graph holds none of it.
    while (!next_is(reader, "{")) {
        take(reader);
    }
    expect(reader, "{");
    std::vector<ReadCoreEntry> entries;
    while (!next_is(reader, "}")) {
        entries.push_back(read_core_entry(reader));
        if (!next_is(reader, "}")) {
            expect(reader, ",");
        }
    }
    expect_all(reader, {"}", ")", ";", "}"});
    return entries;
}

// The types of the buffers that a core's function declares, and of the rings that the file
// declares outside the functions.
inline constexpr std::array<std::string_view, 2> BUFFER_TYPE_NAMES{"EventBuffer", "SampleBuffer"};
inline constexpr std::array<std::string_view, 2> RING_TYPE_NAMES{"EventRing", "SampleRing"};

// The number that a text says, e.g. 30000.0, or none for a text that isn't a number alone.
[[nodiscard]] inline std::optional<double> number_of(std::string_view text) {
    const std::string copy(text);
    try {
        std::size_t read_count = 0;
        const double number = std::stod(copy, &read_count);
        return read_count == copy.size() ? std::optional<double>(number) : std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// The stream that the arguments of a StreamInfo say: its channel count, its rate, and float, the
// one type a sample has.
[[nodiscard]] inline GraphStream stream_of_arguments(const std::vector<std::string>& arguments,
                                                     std::size_t line) {
    constexpr std::size_t stream_argument_count = 3;
    if (arguments.size() != stream_argument_count || arguments.back() != "float") {
        refuse_at(line, "a stream is written as StreamInfo<channels, rate, float>");
    }
    const int channel_count = whole_number_of(arguments.front()).value_or(0);
    const double rate = number_of(arguments.at(1)).value_or(0.0);
    if (channel_count < 1 || !std::isfinite(rate) || rate <= 0) {
        refuse_at(line, std::format("`{}` and `{}` must be a stream's channel count, 1 at least, "
                                    "and its rate, above 0",
                                    arguments.front(), arguments.at(1)));
    }
    return {.channel_count = channel_count, .rate = rate};
}

// A stream's line, from its name on: `using Name = StreamInfo<channels, rate, float>;`.
[[nodiscard]] inline ReadStream read_stream(TokenReader& reader, std::string_view source) {
    ReadStream stream;
    stream.line = line_of_next(reader);
    stream.name = take_word(reader, "a stream's name");
    expect(reader, "=");
    const std::string type_name = take_qualified_name(reader, "a type");
    if (type_name != "StreamInfo" || !next_is(reader, "<")) {
        refuse_at(stream.line,
                  "the reader reads `using` for a stream alone, written as `using "
                  "Name = StreamInfo<channels, rate, float>;`");
    }
    stream.stream = stream_of_arguments(take_template_arguments(reader, source), stream.line);
    expect(reader, ";");
    return stream;
}

[[nodiscard]] inline ReadFile read_file(std::span<const Token> tokens, std::string_view source) {
    ReadFile file;
    TokenReader reader{.tokens = tokens};
    while (!is_at_end(reader)) {
        const Token& first = peek(reader);
        if (first.text == "void") {
            take(reader);
            file.cores.push_back(read_core(reader, source));
        } else if (first.text == "int" && !file.has_main) {
            take(reader);
            file.entries = read_main(reader);
            file.has_main = true;
        } else if (first.text == "using") {
            take(reader);
            file.streams.push_back(read_stream(reader, source));
        } else if (std::ranges::contains(RING_TYPE_NAMES, first.text)) {
            file.rings.push_back(read_declaration(reader, source));
        } else {
            refuse_at(first.line,
                      std::format("the reader doesn't read what starts with `{}` outside a core's "
                                  "function",
                                  first.text));
        }
    }
    if (!file.has_main) {
        refuse_at(line_of_next(reader), "the file has no main() that names its cores");
    }
    return file;
}

// --- The header ---

// What the comment block at the top of a pipeline.cpp says of the pipeline.
struct PipelineHeader {
    std::string name;
    std::string description;
};

// Sets what one line of the header says, e.g. "// Name: Memory impairment". A line of another
// form, or with a key that this Falcon doesn't know, says nothing.
inline void set_header_value(PipelineHeader& header, std::string_view line) {
    if (!line.starts_with("//")) {
        return;
    }
    const std::string_view content = without_spaces_around(line.substr(2));
    const std::size_t colon_position = content.find(':');
    if (colon_position == std::string_view::npos) {
        return;
    }
    const std::string_view key = without_spaces_around(content.substr(0, colon_position));
    const std::string_view value = without_spaces_around(content.substr(colon_position + 1));
    if (key == PIPELINE_HEADER_NAME_KEY) {
        header.name = std::string(value);
    } else if (key == PIPELINE_HEADER_DESCRIPTION_KEY) {
        header.description = std::string(value);
    }
}

// The header of a pipeline.cpp. A file without the block has an empty one: a person may have
// written the file.
[[nodiscard]] inline PipelineHeader header_of(std::string_view source) {
    PipelineHeader header;
    bool is_inside_block = false;
    std::size_t block_start_line = 0;
    std::size_t line_number = 0;
    std::size_t position = 0;
    while (position < source.size()) {
        const std::size_t line_end = line_end_from(source, position);
        std::string_view line = without_spaces_around(source.substr(position, line_end - position));
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        ++line_number;
        position = line_end + 1;
        if (!is_inside_block && line == PIPELINE_HEADER_START) {
            is_inside_block = true;
            block_start_line = line_number;
        } else if (is_inside_block && line == PIPELINE_HEADER_END) {
            return header;
        } else if (is_inside_block) {
            set_header_value(header, line);
        }
    }
    if (is_inside_block) {
        refuse_at(block_start_line,
                  std::format("the header block has no line `{}`", PIPELINE_HEADER_END));
    }
    return header;
}

// --- From what a file says to a graph ---

// The value of an option that is read next, as written: up to the comma or the closing brace
// that ends it. `argument` is the text the reader's tokens were read from.
[[nodiscard]] inline std::string take_option_value(TokenReader& reader, std::string_view argument) {
    int depth = 0;
    std::size_t value_start = std::string_view::npos;
    std::size_t value_end = 0;
    while (true) {
        const Token& token = peek(reader);
        const bool is_value_end = depth == 0 && token.kind == TokenKind::Symbol &&
                                  (token.text == "," || token.text == "}");
        if (is_value_end) {
            break;
        }
        if (is_opening_bracket(token)) {
            ++depth;
        } else if (is_closing_bracket(token)) {
            --depth;
        }
        if (value_start == std::string_view::npos) {
            value_start = token.start;
        }
        value_end = end_of(token);
        take(reader);
    }
    if (value_start == std::string_view::npos) {
        refuse_at(line_of_next(reader), "an option has no value");
    }
    return std::string(argument.substr(value_start, value_end - value_start));
}

// An option's value as the graph holds it, from the value as a file has it: on one line, and a
// list with its elements between one pair of braces, however the file braces and lays it out.
[[nodiscard]] inline std::string held_value_of(const OptionDescription& option,
                                               std::string_view written_value, std::size_t line) {
    std::string value = on_one_line_plainly(written_value);
    if (!is_list_option(option)) {
        return value;
    }
    const std::optional<std::vector<std::string>> elements =
        list_elements_of(value, has_braced_elements(option));
    if (!elements.has_value()) {
        refuse_at(line, std::format("the option {} must be a list in braces, and is `{}`",
                                    option.name, value));
    }
    return held_list_of(elements.value_or(std::vector<std::string>{}));
}

// Puts the options that a processor's type is written with into its option values, e.g. from
// "{.rate = 4}" or "EventLoggerOptions{.style = LoggingStyle::EveryEvent}". An option that isn't
// named keeps the value it has.
inline void read_options_into(std::vector<std::string>& option_values, std::string_view argument,
                              const ProcessorDescription& type, std::size_t line) {
    const std::vector<Token> tokens = tokens_of(argument, line);
    TokenReader reader{.tokens = tokens};
    // The options' type may be written in front of the braces.
    while (!is_at_end(reader) && !next_is(reader, "{")) {
        take(reader);
    }
    if (is_at_end(reader)) {
        refuse_at(line, std::format("the options of a {} aren't written in braces", type.name));
    }
    expect(reader, "{");
    std::vector<bool> is_named(option_values.size(), false);
    while (!next_is(reader, "}")) {
        expect(reader, ".");
        const std::string option_name = take_word(reader, "an option's name");
        expect(reader, "=");
        const std::string written_value = take_option_value(reader, argument);
        const auto option = std::ranges::find(type.options, option_name, &OptionDescription::name);
        if (option == type.options.end()) {
            refuse_at(line, std::format("a {} has no option {}", type.name, option_name));
        }
        std::string value = held_value_of(*option, written_value, line);
        const auto option_index =
            static_cast<std::size_t>(std::ranges::distance(type.options.begin(), option));
        if (is_named.at(option_index)) {
            refuse_at(line, std::format("the option {} is named twice", option_name));
        }
        is_named.at(option_index) = true;
        option_values.at(option_index) = std::move(value);
        if (!next_is(reader, "}")) {
            expect(reader, ",");
        }
    }
}

// A processor's option values, one for each option of its type: what its declaration names, and
// the default of each option that it leaves out.
[[nodiscard]] inline std::vector<std::string> option_values_of(const ReadDeclaration& declaration,
                                                               const ProcessorDescription& type) {
    std::vector<std::string> option_values;
    for (const OptionDescription& option : type.options) {
        option_values.emplace_back(option.default_value);
    }
    if (declaration.template_arguments.size() > type.template_parameters.size()) {
        refuse_at(declaration.line,
                  std::format("{} is given {} template arguments, and a {} takes {} at most",
                              declaration.variable_name, declaration.template_arguments.size(),
                              type.name, type.template_parameters.size()));
    }
    const auto options_parameter =
        std::ranges::find(type.template_parameters, OPTIONS_PARAMETER_NAME,
                          [](const char* parameter) { return template_parameter_name(parameter); });
    const auto options_index = static_cast<std::size_t>(
        std::ranges::distance(type.template_parameters.begin(), options_parameter));
    if (options_index < declaration.template_arguments.size()) {
        read_options_into(option_values, declaration.template_arguments.at(options_index), type,
                          declaration.line);
    }
    return option_values;
}

// What is known of a ring while a file is turned into a graph.
struct ReadRing {
    int slot_count = 0;
    std::size_t line = 0;
    std::optional<OutputReference> writer;
    std::optional<int> reader_core_number;
};

// The file's rings, by their names.
using ReadRings = std::map<std::string, ReadRing, std::less<>>;

// An input that a call gives a buffer or a ring, to be connected once the writer of that is known.
struct PendingInput {
    InputReference input;
    std::string source_name;
    std::size_t line = 0;
};

// The graph that a file is turned into, with what is still needed to finish it.
struct GraphBeingRead {
    PipelineGraph graph;
    ReadRings rings;
    // The inputs that read a ring. They are connected once every function's calls are read, since
    // a ring's writer may be called in a function further down.
    std::vector<PendingInput> ring_inputs;
};

// A buffer that a core's function declares, with the rings it is given, which carry what it
// holds to other cores.
struct ReadBuffer {
    std::string name;
    std::vector<std::string> ring_names;
};

// The output that writes each buffer of a core, by the buffer's name.
using BufferWriters = std::map<std::string, OutputReference, std::less<>>;

// A core's function while its calls are read.
struct CoreBeingRead {
    int core_number = 0;
    std::vector<ReadBuffer> buffers;
    BufferWriters buffer_writers;
    std::vector<PendingInput> buffer_inputs;  // the inputs that read a buffer of this core
};

// The stream that a declaration has its processor start: the one of the file's streams that its
// type names where it takes that stream, or a stream written out there.
[[nodiscard]] inline GraphStream stream_started_by(const ReadDeclaration& declaration,
                                                   const ProcessorDescription& type,
                                                   const std::vector<ReadStream>& streams) {
    const auto stream_parameter =
        std::ranges::find(type.template_parameters, OUTPUT_STREAM_PARAMETER_NAME,
                          [](const char* parameter) { return template_parameter_name(parameter); });
    const auto stream_index = static_cast<std::size_t>(
        std::ranges::distance(type.template_parameters.begin(), stream_parameter));
    if (stream_index >= declaration.template_arguments.size()) {
        refuse_at(declaration.line, std::format("{} starts a stream, and its type names none",
                                                declaration.variable_name));
    }
    const std::string& argument = declaration.template_arguments.at(stream_index);
    const auto stream = std::ranges::find(streams, argument, &ReadStream::name);
    if (stream != streams.end()) {
        return stream->stream;
    }
    if (!argument.starts_with("StreamInfo<")) {
        refuse_at(declaration.line, std::format("`{}` names no stream of this file", argument));
    }
    return stream_of_arguments(type_arguments(argument), declaration.line);
}

// Refuses a processor's option whose value names one of the file's streams, e.g. a rate written
// as TestStream::rate: the writer names every stream anew, so the value would no longer compile.
inline void refuse_option_that_names_stream(const ReadDeclaration& declaration,
                                            const std::vector<std::string>& option_values,
                                            const std::vector<ReadStream>& streams) {
    for (const std::string& option_value : option_values) {
        for (const ReadStream& stream : streams) {
            if (holds_word(option_value, stream.name)) {
                refuse_at(declaration.line,
                          std::format("an option of {} is written as `{}`, which names the stream "
                                      "{}: its value must be written out, e.g. as a number",
                                      declaration.variable_name, option_value, stream.name));
            }
        }
    }
}

// Adds a processor that a core's function declares to the graph, at the end of its core's list.
inline void add_processor_to_graph(PipelineGraph& graph, const ReadDeclaration& declaration,
                                   const ProcessorDescription& type, int core_number,
                                   const std::vector<ReadStream>& streams) {
    if (takes_argument_for_each_connection(type)) {
        refuse_at(declaration.line,
                  std::format("the reader doesn't read a {}, whose type takes an argument for each "
                              "of its connections",
                              type.name));
    }
    const bool is_named_in_quotes =
        declaration.initializer.size() == 1 &&
        declaration.initializer.front().text == std::format("\"{}\"", declaration.variable_name);
    if (!is_named_in_quotes) {
        refuse_at(declaration.line,
                  std::format("{0} must be given its own name in braces and quotes, as "
                              "{0}{{\"{0}\"}}",
                              declaration.variable_name));
    }
    const bool is_name_taken =
        std::ranges::contains(graph.processors, declaration.variable_name, &GraphProcessor::name);
    if (is_name_taken) {
        refuse_at(declaration.line,
                  std::format("two processors are named {}", declaration.variable_name));
    }
    std::vector<std::string> option_values = option_values_of(declaration, type);
    refuse_option_that_names_stream(declaration, option_values, streams);
    const ProcessorId processor_id =
        graph.add_processor(type, {.core_number = core_number, .next_processor = std::nullopt});
    GraphProcessor* processor = graph.find_processor(processor_id);
    if (processor == nullptr) {
        refuse_at(declaration.line, "the graph didn't take the processor");
    }
    processor->name = declaration.variable_name;
    processor->option_values = std::move(option_values);
    if (type.starts_stream) {
        processor->stream = stream_started_by(declaration, type, streams);
    }
}

// A buffer as a core's function declares it. What stands in its braces are the rings it is given.
[[nodiscard]] inline ReadBuffer buffer_of(const ReadDeclaration& declaration,
                                          const ReadRings& rings) {
    ReadBuffer buffer{.name = declaration.variable_name, .ring_names = {}};
    for (const Token& token : declaration.initializer) {
        if (token.kind == TokenKind::Symbol && token.text == ",") {
            continue;
        }
        if (!rings.contains(token.text)) {
            refuse_at(token.line,
                      std::format("the buffer {} is given `{}`, which is no ring of this file",
                                  buffer.name, token.text));
        }
        buffer.ring_names.emplace_back(token.text);
    }
    return buffer;
}

// Adds what a core's function declares to the graph: its processors. Returns the buffers it
// declares.
[[nodiscard]] inline std::vector<ReadBuffer> add_declarations_to_graph(
    GraphBeingRead& being_read, const ReadFile& file, const ReadCore& core, int core_number,
    std::span<const ProcessorDescription> processor_types) {
    std::vector<ReadBuffer> buffers;
    for (const ReadDeclaration& declaration : core.declarations) {
        const auto type =
            std::ranges::find(processor_types, declaration.type_name, &ProcessorDescription::name);
        if (type != processor_types.end()) {
            add_processor_to_graph(being_read.graph, declaration, *type, core_number, file.streams);
        } else if (std::ranges::contains(BUFFER_TYPE_NAMES, declaration.type_name)) {
            buffers.push_back(buffer_of(declaration, being_read.rings));
        } else {
            refuse_at(declaration.line,
                      std::format("`{}` is no kind of processor that this Falcon knows, and no "
                                  "buffer",
                                  declaration.type_name));
        }
    }
    return buffers;
}

// Why a connection that a file holds doesn't fit, in words.
[[nodiscard]] inline std::string_view reason_of(ConnectionVerdict verdict) {
    switch (verdict) {
        case ConnectionVerdict::Fits:
            return "it fits";
        case ConnectionVerdict::UnknownPort:
            return "one of its ends is no port";
        case ConnectionVerdict::SameProcessor:
            return "both of its ends are ports of one processor";
        case ConnectionVerdict::DifferentKind:
            return "its two ends don't carry the same kind";
        case ConnectionVerdict::EventKindNotTaken:
            return "the input doesn't take the kind of event that the output gives";
        case ConnectionVerdict::AlreadyConnected:
            return "these two are connected already";
        case ConnectionVerdict::InputTaken:
            return "the input takes one connection and has one already";
    }
    return "it doesn't fit";
}

// Notes the output that writes a ring. A ring has one.
inline void set_ring_writer(ReadRings& rings, const ReadPortBuffer& port_buffer,
                            const OutputReference& output, std::size_t line) {
    const auto found = rings.find(port_buffer.buffer_name);
    if (found == rings.end()) {
        refuse_at(line, std::format("{} is no buffer that this function declares, and no ring",
                                    port_buffer.buffer_name));
    }
    ReadRing& ring = found->second;
    if (ring.writer.value_or(output) != output) {
        refuse_at(line, std::format("two outputs write the ring {}", port_buffer.buffer_name));
    }
    ring.writer = output;
}

// Takes in an output that a call gives a buffer or a ring: it writes that, and the rings that the
// buffer is given.
inline void read_output_into(GraphBeingRead& being_read, CoreBeingRead& core,
                             const OutputReference& output, const ReadPortBuffer& port_buffer,
                             std::size_t line) {
    const auto buffer = std::ranges::find(core.buffers, port_buffer.buffer_name, &ReadBuffer::name);
    if (buffer == core.buffers.end()) {
        set_ring_writer(being_read.rings, port_buffer, output, line);
        return;
    }
    if (!core.buffer_writers.emplace(buffer->name, output).second) {
        refuse_at(line, std::format("two outputs write the buffer {}", buffer->name));
    }
    for (const std::string& ring_name : buffer->ring_names) {
        set_ring_writer(being_read.rings,
                        {.port_name = port_buffer.port_name, .buffer_name = ring_name}, output,
                        line);
    }
}

// Takes in an input that a call gives a buffer or a ring, to be connected later.
inline void read_input_into(GraphBeingRead& being_read, CoreBeingRead& core,
                            const InputReference& input, const ReadPortBuffer& port_buffer,
                            std::size_t line) {
    PendingInput pending{.input = input, .source_name = port_buffer.buffer_name, .line = line};
    if (std::ranges::contains(core.buffers, port_buffer.buffer_name, &ReadBuffer::name)) {
        core.buffer_inputs.push_back(std::move(pending));
        return;
    }
    const auto found = being_read.rings.find(port_buffer.buffer_name);
    if (found == being_read.rings.end()) {
        refuse_at(line, std::format("{} is no buffer that this function declares, and no ring",
                                    port_buffer.buffer_name));
    }
    ReadRing& ring = found->second;
    if (ring.reader_core_number.value_or(core.core_number) != core.core_number) {
        refuse_at(line, std::format("the ring {} is read on two cores, and a ring has one core "
                                    "that reads it",
                                    port_buffer.buffer_name));
    }
    ring.reader_core_number = core.core_number;
    being_read.ring_inputs.push_back(std::move(pending));
}

// Takes in what one call says: what its outputs write, and the inputs still to connect.
inline void read_call_into(GraphBeingRead& being_read, CoreBeingRead& core, const ReadCall& call) {
    const auto processor = std::ranges::find_if(
        being_read.graph.processors, [&call, &core](const GraphProcessor& candidate) {
            return candidate.name == call.processor_name &&
                   candidate.core_number == core.core_number;
        });
    if (processor == being_read.graph.processors.end()) {
        refuse_at(call.line, std::format("{} is called here, and this function doesn't declare it",
                                         call.processor_name));
    }
    for (const ReadPortBuffer& port_buffer : call.port_buffers) {
        const auto port = std::ranges::find(processor->type->ports, port_buffer.port_name,
                                            &PortDescription::name);
        if (port == processor->type->ports.end()) {
            refuse_at(call.line, std::format("a {} has no port {}", processor->type->name,
                                             port_buffer.port_name));
        }
        if (port->direction == PortDirection::Output) {
            read_output_into(being_read, core,
                             {.processor = processor->id, .port = port_buffer.port_name},
                             port_buffer, call.line);
        } else {
            read_input_into(being_read, core,
                            {.processor = processor->id, .port = port_buffer.port_name},
                            port_buffer, call.line);
        }
    }
}

// Connects an output to an input that a call gives the output's buffer or ring. Returns the
// connection it made.
inline GraphConnection& connect_in_graph(PipelineGraph& graph, const OutputReference& output,
                                         const PendingInput& pending) {
    const PortDescription* output_port =
        graph.find_port(output.processor, output.port, PortDirection::Output);
    const PortDescription* input_port =
        graph.find_port(pending.input.processor, pending.input.port, PortDirection::Input);
    if (output_port == nullptr || input_port == nullptr) {
        refuse_at(pending.line, "a connection has an end that the graph doesn't hold");
    }
    if (output_port->kind == PortKind::State) {
        refuse_at(pending.line, "the reader doesn't read a connection of a state value");
    }
    if (input_port->has_slots) {
        refuse_at(pending.line, "the reader doesn't read an input that takes several connections");
    }
    const ConnectionVerdict verdict = graph.connection_verdict(output, pending.input);
    if (verdict != ConnectionVerdict::Fits || !graph.connect(output, pending.input)) {
        refuse_at(pending.line, std::format("the connection through {} doesn't fit: {}",
                                            pending.source_name, reason_of(verdict)));
    }
    return graph.connections.back();
}

// Adds a core's function to the graph: its processors, and the connections that its calls make
// through its buffers. Its inputs that read a ring are left to be connected.
inline void add_core_to_graph(GraphBeingRead& being_read, const ReadFile& file,
                              const ReadCore& core, const ReadCoreEntry& entry,
                              std::span<const ProcessorDescription> processor_types) {
    PipelineGraph& graph = being_read.graph;
    CoreBeingRead core_being_read;
    core_being_read.core_number = entry.core_number.value_or(0);
    core_being_read.buffers = add_declarations_to_graph(
        being_read, file, core, core_being_read.core_number, processor_types);
    const auto graph_core =
        std::ranges::find(graph.cores, core_being_read.core_number, &GraphCore::number);
    if (graph_core != graph.cores.end()) {
        graph_core->is_realtime = entry.is_realtime;
    }

    std::vector<std::string> called_names;
    for (const ReadCall& call : core.calls) {
        if (std::ranges::contains(called_names, call.processor_name)) {
            refuse_at(call.line, std::format("{} is called twice", call.processor_name));
        }
        called_names.push_back(call.processor_name);
        read_call_into(being_read, core_being_read, call);
    }
    for (const GraphProcessor& processor : graph.processors) {
        if (processor.core_number == core_being_read.core_number &&
            !std::ranges::contains(called_names, processor.name)) {
            refuse_at(core.line,
                      std::format("{} is declared, and the loop never calls it", processor.name));
        }
    }
    for (const PendingInput& pending : core_being_read.buffer_inputs) {
        const auto writer = core_being_read.buffer_writers.find(pending.source_name);
        if (writer == core_being_read.buffer_writers.end()) {
            refuse_at(pending.line,
                      std::format("no output writes the buffer {}", pending.source_name));
        }
        connect_in_graph(graph, writer->second, pending);
    }
}

// Connects the inputs that read a ring to the output that writes it. Such a connection gets the
// ring's number of slots where that isn't the usual one.
inline void connect_ring_inputs(GraphBeingRead& being_read) {
    PipelineGraph& graph = being_read.graph;
    for (const PendingInput& pending : being_read.ring_inputs) {
        const auto found = being_read.rings.find(pending.source_name);
        if (found == being_read.rings.end() || !found->second.writer.has_value()) {
            refuse_at(pending.line,
                      std::format("no output writes the ring {}", pending.source_name));
        }
        const ReadRing& ring = found->second;
        GraphConnection& connection =
            connect_in_graph(graph, ring.writer.value_or(OutputReference{}), pending);
        const GraphProcessor* reader = graph.find_processor(pending.input.processor);
        const auto reader_core = std::ranges::find(
            graph.cores, reader == nullptr ? 0 : reader->core_number, &GraphCore::number);
        const bool is_reader_realtime =
            reader_core == graph.cores.end() || reader_core->is_realtime;
        if (ring.slot_count != usual_ring_slot_count(is_reader_realtime)) {
            connection.ring_slot_count = ring.slot_count;
        }
    }
    for (const auto& [ring_name, ring] : being_read.rings) {
        if (ring.writer.has_value() && !ring.reader_core_number.has_value()) {
            refuse_at(ring.line,
                      std::format("the ring {} is written, and nothing reads it", ring_name));
        }
    }
}

// The file's rings as they are declared, each with its number of slots, which is the last
// argument of its type.
[[nodiscard]] inline ReadRings rings_of(const ReadFile& file) {
    ReadRings rings;
    for (const ReadDeclaration& declaration : file.rings) {
        const std::optional<int> slot_count =
            declaration.template_arguments.empty()
                ? std::nullopt
                : whole_number_of(declaration.template_arguments.back());
        if (!slot_count.has_value()) {
            refuse_at(declaration.line,
                      std::format("the ring {} must have its number of slots as the last argument "
                                  "of its type",
                                  declaration.variable_name));
        }
        const bool is_first_of_its_name = rings
                                              .emplace(declaration.variable_name,
                                                       ReadRing{
                                                           .slot_count = slot_count.value_or(0),
                                                           .line = declaration.line,
                                                           .writer = std::nullopt,
                                                           .reader_core_number = std::nullopt,
                                                       })
                                              .second;
        if (!is_first_of_its_name) {
            refuse_at(declaration.line,
                      std::format("two rings are named {}", declaration.variable_name));
        }
    }
    return rings;
}

// The entry of main()'s list that runs a core's function. A function has one, and a core's
// number is used once.
[[nodiscard]] inline const ReadCoreEntry& entry_of_core(const ReadFile& file,
                                                        const ReadCore& core) {
    const auto runs_this_function = [&core](const ReadCoreEntry& entry) {
        return entry.function_name == core.function_name;
    };
    const auto entry = std::ranges::find_if(file.entries, runs_this_function);
    if (entry == file.entries.end()) {
        refuse_at(core.line, std::format("main() names no core that runs {}", core.function_name));
    }
    if (std::ranges::count_if(file.entries, runs_this_function) > 1) {
        refuse_at(entry->line, std::format("main() names {} for two cores", core.function_name));
    }
    const auto same_number_count =
        std::ranges::count(file.entries, entry->core_number, &ReadCoreEntry::core_number);
    if (same_number_count > 1) {
        refuse_at(entry->line,
                  std::format("main() names core {} twice", entry->core_number.value_or(0)));
    }
    return *entry;
}

[[nodiscard]] inline PipelineGraph graph_of(const ReadFile& file, const PipelineHeader& header,
                                            std::span<const ProcessorDescription> processor_types) {
    GraphBeingRead being_read;
    being_read.graph.name = header.name;
    being_read.graph.description = header.description;
    being_read.rings = rings_of(file);
    for (const ReadStream& stream : file.streams) {
        if (std::ranges::count(file.streams, stream.name, &ReadStream::name) > 1) {
            refuse_at(stream.line, std::format("two streams are named {}", stream.name));
        }
    }
    for (const ReadCoreEntry& entry : file.entries) {
        if (!std::ranges::contains(file.cores, entry.function_name, &ReadCore::function_name)) {
            refuse_at(entry.line, std::format("main() names {}, which the file doesn't define",
                                              entry.function_name));
        }
    }
    for (const ReadCore& core : file.cores) {
        if (std::ranges::count(file.cores, core.function_name, &ReadCore::function_name) > 1) {
            refuse_at(core.line, std::format("two functions are named {}", core.function_name));
        }
        add_core_to_graph(being_read, file, core, entry_of_core(file, core), processor_types);
    }
    connect_ring_inputs(being_read);
    return std::move(being_read.graph);
}

// The graph that the text of a pipeline.cpp says, or why the text can't be read. processor_types
// are the kinds of processor that this Falcon knows.
[[nodiscard]] inline ReadPipeline read_pipeline(
    std::string_view source, std::span<const ProcessorDescription> processor_types) {
    try {
        const PipelineHeader header = header_of(source);
        const std::vector<Token> tokens = tokens_of(source, 1);
        return graph_of(read_file(tokens, source), header, processor_types);
    } catch (const PipelineRefusal& refusal) {
        return std::unexpected(std::string(refusal.what()));
    } catch (const std::exception& error) {
        return std::unexpected(std::format("the file couldn't be read: {}", error.what()));
    }
}
