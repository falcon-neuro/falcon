#pragma once
#include <unistd.h>
#include <x86intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <format>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include "common.cpp"

// The logger, and how a processor's warn(), log() and fail() reach it.

// Set once the core's loop may start: from then on, its messages go through its message channel
// instead of straight to the logger.
thread_local bool is_in_loop_of_this_thread = false;

// --- Logger ---

enum class MessageType : std::uint8_t { Log, Warning, Error };

[[nodiscard]] constexpr std::string_view word_of(MessageType type) {
    switch (type) {
        case MessageType::Log:
            return "log";
        case MessageType::Warning:
            return "warning";
        case MessageType::Error:
            return "error";
    }
    return "message";
}

// Marks around each argument in a line's text, so that a terminal can show the arguments in bold.
// The logger takes them out of what it writes anywhere else.
inline constexpr char ARGUMENT_START = '\x01';
inline constexpr char ARGUMENT_END = '\x02';

// `format` with a mark before and after each replacement field ("{}", "{:.2f}"), so that the
// formatted text carries them around each argument. "{{" and "}}" stand for braces to print.
[[nodiscard]] std::string mark_arguments(std::string_view format) {
    std::string marked;
    marked.reserve(format.size());
    std::size_t position = 0;
    while (position < format.size()) {
        const char character = format[position];
        const bool is_brace = character == '{' || character == '}';
        const bool is_doubled = position + 1 < format.size() && format[position + 1] == character;
        if (is_brace && is_doubled) {
            marked.append(2, character);
            position += 2;
        } else if (character == '{') {
            // A field ends at its matching brace: its specification can hold fields of its own,
            // e.g. "{:{}}".
            int depth = 0;
            std::size_t field_end = position;
            while (field_end < format.size()) {
                if (format[field_end] == '{') {
                    ++depth;
                } else if (format[field_end] == '}') {
                    --depth;
                    if (depth == 0) {
                        break;
                    }
                }
                ++field_end;
            }
            marked += ARGUMENT_START;
            marked += format.substr(position, field_end - position + 1);
            marked += ARGUMENT_END;
            position = field_end + 1;
        } else {
            marked += character;
            ++position;
        }
    }
    return marked;
}

// What the two marks around an argument become: nothing for plain text, a style and its end for a
// terminal.
struct ArgumentStyle {
    std::string_view start;
    std::string_view end;
};

[[nodiscard]] std::string replace_argument_marks(std::string_view text, ArgumentStyle style) {
    std::string replaced;
    replaced.reserve(text.size());
    for (const char character : text) {
        if (character == ARGUMENT_START) {
            replaced += style.start;
        } else if (character == ARGUMENT_END) {
            replaced += style.end;
        } else {
            replaced += character;
        }
    }
    return replaced;
}

// Every line of a run, from any thread, goes through here, so each reaches the console and the log
// file whole and in one format.
class Logger {
public:
    void open(const std::filesystem::path& path) {
        const std::scoped_lock lock(mutex_);
        file_ = std::fopen(path.c_str(), "w");
        if (file_ == nullptr) {
            throw std::runtime_error(std::format("can't open the log file {}", path.string()));
        }
    }

    // A line that its caller dates: one from a core's ring, which the main thread dates back to
    // when it was raised. `count` is how often it happened since its previous line: the throttle
    // shows the rest only as that count.
    void write(MessageType type, const char* source_name, std::string_view text, u64 count,
               const timespec& wall_clock) noexcept {
        try {
            const std::scoped_lock lock(mutex_);
            write_locked(type, source_name, text, count, wall_clock);
        } catch (...) {
            std::fputs("falcon: a line couldn't be logged\n", stderr);
        }
    }

    // A line dated as it is written. The time is read under the lock, so lines that several
    // threads write at once come out in the order of their times.
    void write_now(MessageType type, const char* source_name, std::string_view text) noexcept {
        try {
            const std::scoped_lock lock(mutex_);
            write_locked(type, source_name, text, 1, read_clock(CLOCK_REALTIME));
        } catch (...) {
            std::fputs("falcon: a line couldn't be logged\n", stderr);
        }
    }

    void flush() noexcept {
        try {
            const std::scoped_lock lock(mutex_);
            if (file_ != nullptr) {
                std::fflush(file_);
            }
        } catch (...) {
            std::fputs("falcon: the log file couldn't be flushed\n", stderr);
        }
    }

    void close() noexcept {
        try {
            const std::scoped_lock lock(mutex_);
            if (file_ != nullptr) {
                std::fclose(file_);
                file_ = nullptr;
            }
        } catch (...) {
            std::fputs("falcon: the log file couldn't be closed\n", stderr);
        }
    }

private:
    // ANSI escape codes, which a terminal turns into styles.
    static constexpr std::string_view plain_style = "\033[0m";
    static constexpr std::string_view dim_style = "\033[2m";
    static constexpr std::string_view bold_style = "\033[1m";

    std::mutex mutex_;
    std::FILE* file_ = nullptr;

    // Styles help on a terminal and are noise anywhere else, e.g. in a file the console is
    // redirected to. Setting NO_COLOR is the usual way to turn them off.
    [[nodiscard]] static bool is_console_styled() {
        static const bool is_styled =
            isatty(STDERR_FILENO) != 0 && std::getenv("NO_COLOR") == nullptr;
        return is_styled;
    }

    [[nodiscard]] static constexpr std::string_view style_of(MessageType type) {
        switch (type) {
            case MessageType::Log:
                return "\033[36m";  // cyan
            case MessageType::Warning:
                return "\033[1;33m";  // bold yellow
            case MessageType::Error:
                return "\033[1;31m";  // bold red
        }
        return plain_style;
    }

    // The log file always gets the plain line.
    void write_locked(MessageType type, const char* source_name, std::string_view text, u64 count,
                      const timespec& wall_clock) {
        std::tm local_time{};
        const std::time_t seconds = wall_clock.tv_sec;
        localtime_r(&seconds, &local_time);
        constexpr int tm_first_year = 1900;
        constexpr long nanoseconds_per_microsecond = 1000;
        const std::string time_text = std::format(
            "{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:06}", local_time.tm_year + tm_first_year,
            local_time.tm_mon + 1, local_time.tm_mday, local_time.tm_hour, local_time.tm_min,
            local_time.tm_sec, wall_clock.tv_nsec / nanoseconds_per_microsecond);
        const std::string count_text = count > 1 ? std::format(" (×{})", count) : std::string();
        const std::string line =
            std::format("{} {} {}: {}{}\n", time_text, word_of(type), source_name,
                        replace_argument_marks(text, {}), count_text);
        if (is_console_styled()) {
            const std::string styled_count =
                count_text.empty() ? std::string()
                                   : std::format("{}{}{}", dim_style, count_text, plain_style);
            const std::string styled_line =
                std::format("{}{}{} {}{}{} {}: {}{}\n", dim_style, time_text, plain_style,
                            style_of(type), word_of(type), plain_style, source_name,
                            replace_argument_marks(text, {.start = bold_style, .end = plain_style}),
                            styled_count);
            std::fwrite(styled_line.data(), 1, styled_line.size(), stderr);
        } else {
            std::fwrite(line.data(), 1, line.size(), stderr);
        }
        if (file_ != nullptr) {
            std::fwrite(line.data(), 1, line.size(), file_);
        }
    }
};

inline Logger logger;

// A line written at once, from outside any loop, under the name of what it is about: a ring, or
// the engine itself.
template <typename... Arguments>
void log_as(MessageType type, const char* source_name, std::format_string<Arguments...> format,
            Arguments&&... arguments) noexcept {
    try {
        logger.write_now(
            type, source_name,
            std::vformat(mark_arguments(format.get()), std::make_format_args(arguments...)));
    } catch (...) {
        std::fputs("falcon: a line couldn't be logged\n", stderr);
    }
}

// A line of the engine's own.
template <typename... Arguments>
void log_engine(MessageType type, std::format_string<Arguments...> format,
                Arguments&&... arguments) noexcept {
    log_as(type, "engine", format, std::forward<Arguments>(arguments)...);
}

// --- Messages ---

// Whether a type is a channel mask, which a message may carry. The engine sets it for its
// ChannelMask.
template <typename Type>
inline constexpr bool IS_CHANNEL_MASK = false;

// The throttle lets one record of a kind into the ring per interval.
inline constexpr double MESSAGE_INTERVAL_SECONDS = 0.1;  // how often the main thread reads them

[[nodiscard]] u64 message_interval_ticks() noexcept {
    return static_cast<u64>(tsc_ticks_per_second * MESSAGE_INTERVAL_SECONDS);
}

template <typename Argument>
inline constexpr bool IS_MESSAGE_TEXT =
    std::same_as<Argument, std::string_view> || std::same_as<Argument, const char*>;

// In the loop, a message's arguments are copied into a record and formatted later by the main
// thread, by which time a pointer could dangle. So an argument is a plain value, or text, whose
// characters are copied.
template <typename Argument>
concept MessageArgument =
    IS_MESSAGE_TEXT<Argument> ||
    (std::is_trivially_copyable_v<Argument> &&
     (std::is_arithmetic_v<Argument> || std::is_enum_v<Argument> || IS_CHANNEL_MASK<Argument>));

// What std::format sees of an argument: an enum's number, text as a string_view.
template <MessageArgument Argument>
[[nodiscard]] constexpr auto formatted(const Argument& argument) {
    if constexpr (std::is_enum_v<Argument>) {
        return std::to_underlying(argument);
    } else if constexpr (std::same_as<Argument, const char*>) {
        return argument == nullptr ? std::string_view("(null)") : std::string_view(argument);
    } else {
        return argument;
    }
}

template <MessageArgument Argument>
using FormattedAs = decltype(formatted(std::declval<const Argument&>()));

// What the main thread rebuilds of an argument from a record: it owns its copy of text.
template <MessageArgument Argument>
using StoredAs = std::conditional_t<IS_MESSAGE_TEXT<Argument>, std::string, Argument>;

template <typename Value>
[[nodiscard]] auto formatted_value(const Value& value) {
    if constexpr (std::same_as<Value, std::string>) {
        return std::string_view(value);
    } else {
        return formatted(value);
    }
}

template <typename... Values>
[[nodiscard]] std::string format_values(std::string_view format, const Values&... values) {
    auto formatted_values = std::make_tuple(formatted_value(values)...);
    const std::string marked_format = mark_arguments(format);
    return std::apply(
        [&marked_format](auto&... formatted_values_of_tuple) {
            return std::vformat(marked_format, std::make_format_args(formatted_values_of_tuple...));
        },
        formatted_values);
}

inline constexpr std::size_t MESSAGE_ARGUMENTS_CAPACITY = 256;  // bytes
using MessageArgumentBytes = std::array<std::byte, MESSAGE_ARGUMENTS_CAPACITY>;
using MessageTextLength = u32;

// Text takes its length here; its characters share what the other arguments leave.
template <MessageArgument Argument>
inline constexpr std::size_t MESSAGE_ARGUMENT_FIXED_SIZE =
    IS_MESSAGE_TEXT<Argument> ? sizeof(MessageTextLength) : sizeof(Argument);

template <MessageArgument... Arguments>
inline constexpr std::size_t MESSAGE_ARGUMENTS_FIXED_SIZE =
    (std::size_t{0} + ... + MESSAGE_ARGUMENT_FIXED_SIZE<Arguments>);

struct MessageArgumentCursor {
    std::size_t offset = 0;
    std::size_t text_room = 0;  // bytes the text arguments still may take
};

// Text that doesn't fit is cut and ends in "...".
template <MessageArgument Argument>
void store_message_argument(MessageArgumentBytes& bytes, MessageArgumentCursor& cursor,
                            const Argument& argument) noexcept {
    if constexpr (IS_MESSAGE_TEXT<Argument>) {
        const std::string_view text = formatted(argument);
        const std::size_t kept_size = std::min(text.size(), cursor.text_room);
        const auto length = static_cast<MessageTextLength>(kept_size);
        std::memcpy(bytes.data() + cursor.offset, &length, sizeof(length));
        cursor.offset += sizeof(length);
        if (kept_size > 0) {
            std::memcpy(bytes.data() + cursor.offset, text.data(), kept_size);
        }
        constexpr std::string_view cut_mark = "...";
        if (kept_size < text.size() && kept_size >= cut_mark.size()) {
            std::memcpy(bytes.data() + cursor.offset + kept_size - cut_mark.size(), cut_mark.data(),
                        cut_mark.size());
        }
        cursor.offset += kept_size;
        cursor.text_room -= kept_size;
    } else {
        std::memcpy(bytes.data() + cursor.offset, &argument, sizeof(Argument));
        cursor.offset += sizeof(Argument);
    }
}

template <MessageArgument... Arguments>
void store_message_arguments(MessageArgumentBytes& bytes, const Arguments&... arguments) noexcept {
    MessageArgumentCursor cursor{
        .offset = 0,
        .text_room = MESSAGE_ARGUMENTS_CAPACITY - MESSAGE_ARGUMENTS_FIXED_SIZE<Arguments...>,
    };
    (store_message_argument(bytes, cursor, arguments), ...);
}

template <MessageArgument Argument>
void load_message_argument(const MessageArgumentBytes& bytes, std::size_t& offset,
                           StoredAs<Argument>& value) {
    if constexpr (IS_MESSAGE_TEXT<Argument>) {
        MessageTextLength length = 0;
        std::memcpy(&length, bytes.data() + offset, sizeof(length));
        offset += sizeof(length);
        value.assign(reinterpret_cast<const char*>(bytes.data() + offset), length);
        offset += length;
    } else {
        std::memcpy(&value, bytes.data() + offset, sizeof(Argument));
        offset += sizeof(Argument);
    }
}

// Formats what store_message_arguments() copied into a record.
template <MessageArgument... Arguments>
[[nodiscard]] std::string format_stored_message(std::string_view format,
                                                const MessageArgumentBytes& bytes) {
    std::tuple<StoredAs<Arguments>...> values{};
    [[maybe_unused]] std::size_t offset = 0;
    // A default capture, since a message without arguments uses neither.
    std::apply(
        [&](StoredAs<Arguments>&... value) {
            (load_message_argument<Arguments>(bytes, offset, value), ...);
        },
        values);
    return std::apply(
        [format](const StoredAs<Arguments>&... value) { return format_values(format, value...); },
        values);
}

using MessageFormatter = std::string (*)(std::string_view format,
                                         const MessageArgumentBytes& bytes);

// One kind of message on one core: a processor's name with a format string. It counts every
// occurrence, including those the throttle keeps out of the ring.
struct MessageKind {
    const char* processor_name = nullptr;
    std::string_view format;
    MessageType type = MessageType::Log;
    bool has_sent_record = false;  // core only
    u64 last_record_tsc = 0;       // core only
    std::atomic<u64> occurrence_count = 0;
};

struct MessageRecord {
    const MessageKind* kind = nullptr;
    MessageFormatter formatter = nullptr;
    u64 tsc = 0;
    MessageArgumentBytes arguments{};
};

// One per core: the ring through which its loop's messages reach the main thread, and its kinds.
// The core writes it, the main thread reads it.
struct MessageChannel {
    static constexpr std::size_t record_count = 64;
    static constexpr std::size_t kind_capacity = 64;

    // Each position apart from the others, so the two threads share only the lines they must.
    alignas(BYTES_BETWEEN_CORES) std::atomic<u64> write_position = 0;
    alignas(BYTES_BETWEEN_CORES) std::atomic<u64> read_position = 0;
    alignas(BYTES_BETWEEN_CORES) std::atomic<std::size_t> kind_count = 0;
    std::atomic<u64> dropped_record_count = 0;      // the ring was full
    std::atomic<u64> dropped_occurrence_count = 0;  // the core had more kinds than it holds
    std::array<MessageRecord, record_count> records{};
    std::array<MessageKind, kind_capacity> kinds{};
};

thread_local MessageChannel* message_channel_of_this_thread = nullptr;  // set by run_on_cores()

// nullptr if the core already has as many kinds as it holds.
[[nodiscard]] MessageKind* find_message_kind(MessageChannel& channel, MessageType type,
                                             const char* processor_name,
                                             std::string_view format) noexcept {
    // Only this core adds kinds, so it reads its own latest count.
    const std::size_t kind_count = channel.kind_count.load(std::memory_order_relaxed);
    for (std::size_t kind_index = 0; kind_index < kind_count; ++kind_index) {
        MessageKind& kind = channel.kinds[kind_index];
        if (kind.processor_name == processor_name && kind.format.data() == format.data() &&
            kind.type == type) {
            return &kind;
        }
    }
    if (kind_count == MessageChannel::kind_capacity) {
        return nullptr;
    }
    MessageKind& kind = channel.kinds[kind_count];
    kind.processor_name = processor_name;
    kind.format = format;
    kind.type = type;
    channel.kind_count.store(kind_count + 1, std::memory_order_release);
    return &kind;
}

// Core only. A full ring drops the record and counts it.
template <MessageArgument... Arguments>
void publish_message_record(MessageChannel& channel, MessageKind& kind, u64 tsc,
                            const Arguments&... arguments) noexcept {
    const u64 write_position = channel.write_position.load(std::memory_order_relaxed);
    if (write_position - channel.read_position.load(std::memory_order_acquire) ==
        MessageChannel::record_count) {
        channel.dropped_record_count.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    kind.has_sent_record = true;
    kind.last_record_tsc = tsc;
    MessageRecord& record = channel.records[write_position % MessageChannel::record_count];
    record.kind = &kind;
    record.formatter = &format_stored_message<Arguments...>;
    record.tsc = tsc;
    store_message_arguments(record.arguments, arguments...);
    channel.write_position.store(write_position + 1, std::memory_order_release);
}

template <MessageArgument... Arguments>
void send_through_channel(MessageChannel& channel, MessageType type, ProcessorName name,
                          std::string_view format, const Arguments&... arguments) noexcept {
    MessageKind* const kind = find_message_kind(channel, type, name.c_str(), format);
    if (kind == nullptr) {
        channel.dropped_occurrence_count.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const u64 now = __rdtsc();
    const bool is_throttled =
        kind->has_sent_record && now - kind->last_record_tsc < message_interval_ticks();
    if (!is_throttled) {
        publish_message_record(channel, *kind, now, arguments...);
    }
    // Counted only once its record is in the ring: the main thread reads the counts before the
    // records, so it has read the record of every occurrence it counts.
    // Only this core writes the count, so a load and a store need no locked instruction.
    kind->occurrence_count.store(kind->occurrence_count.load(std::memory_order_relaxed) + 1,
                                 std::memory_order_release);
}

template <MessageArgument... Arguments>
void send_message(MessageType type, ProcessorName name, std::string_view format,
                  const Arguments&... arguments) noexcept {
    static_assert(MESSAGE_ARGUMENTS_FIXED_SIZE<Arguments...> <= MESSAGE_ARGUMENTS_CAPACITY,
                  "a message's arguments take at most 256 bytes");
    MessageChannel* const channel = message_channel_of_this_thread;
    // While an exception unwinds the core's function, its processors' destructors are teardown
    // already, though is_pipeline_running is cleared only once they are done.
    const bool is_in_loop = channel != nullptr && is_in_loop_of_this_thread &&
                            is_pipeline_running.load(std::memory_order_relaxed) &&
                            std::uncaught_exceptions() == 0;
    if (is_in_loop) {
        send_through_channel(*channel, type, name, format, arguments...);
        return;
    }
    // Outside a loop (setup, teardown, or a thread that runs none), waiting is allowed.
    try {
        logger.write_now(type, name.c_str(), format_values(format, arguments...));
    } catch (...) {
        std::fputs("falcon: a line couldn't be logged\n", stderr);
    }
}

// Cold and never inlined, so a call in process() costs it only a branch and doesn't keep clang
// from inlining it.
template <MessageArgument... Arguments>
[[gnu::cold, gnu::noinline]] void warn(ProcessorName name,
                                       std::format_string<FormattedAs<Arguments>...> format,
                                       Arguments... arguments) noexcept {
    send_message(MessageType::Warning, name, format.get(), arguments...);
}

template <MessageArgument... Arguments>
[[gnu::cold, gnu::noinline]] void log(ProcessorName name,
                                      std::format_string<FormattedAs<Arguments>...> format,
                                      Arguments... arguments) noexcept {
    send_message(MessageType::Log, name, format.get(), arguments...);
}

// What fail() throws. Its error line is logged already, so whoever catches it doesn't log it
// again.
class ProcessorFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Set by fail() before it throws. While the exception unwinds a process() call, its outputs are
// destroyed like after a normal return, and must not hand a half-written sample to another core.
thread_local bool is_failing_of_this_thread = false;

// Ends the run: the core's thread catches the exception, every loop stops and every destructor
// runs. Cold and never inlined, as warn() is. It logs its line itself, before it throws: the
// core's destructors run before the catch, and one that hangs would otherwise hide the reason.
template <MessageArgument... Arguments>
[[noreturn, gnu::cold, gnu::noinline]] void fail(
    ProcessorName name, std::format_string<FormattedAs<Arguments>...> format,
    Arguments... arguments) {
    is_failing_of_this_thread = true;
    const std::string text = format_values(format.get(), arguments...);
    logger.write_now(MessageType::Error, name.c_str(), text);
    throw ProcessorFailure(std::format("{}: {}", name.c_str(), replace_argument_marks(text, {})));
}

// The main thread's side of a core's message channel: what it has reported so far.
struct ReportedKind {
    u64 reported_count = 0;
    std::string text;  // its latest record's, shown again when only its count changes
    // `text` is a record's that no line has shown yet. A later record doesn't replace it, so the
    // kind's next line shows its first occurrence since its previous line.
    bool has_unreported_record = false;
    u64 unreported_record_tsc = 0;
};

struct MessageChannelReader {
    MessageChannel* channel = nullptr;
    int core_number = 0;
    std::array<ReportedKind, MessageChannel::kind_capacity> kinds{};
    u64 reported_dropped_record_count = 0;
    u64 reported_dropped_occurrence_count = 0;
};

// A core's occurrence counts at one moment.
struct PolledCounts {
    std::size_t kind_count = 0;
    std::array<u64, MessageChannel::kind_capacity> occurrence_counts{};
};

[[nodiscard]] PolledCounts read_message_counts(const MessageChannel& channel) noexcept {
    PolledCounts counts;
    counts.kind_count = channel.kind_count.load(std::memory_order_acquire);
    for (std::size_t kind_index = 0; kind_index < counts.kind_count; ++kind_index) {
        counts.occurrence_counts[kind_index] =
            channel.kinds[kind_index].occurrence_count.load(std::memory_order_acquire);
    }
    return counts;
}

void read_message_records(MessageChannelReader& reader) {
    MessageChannel& channel = *reader.channel;
    const u64 write_position = channel.write_position.load(std::memory_order_acquire);
    for (u64 read_position = channel.read_position.load(std::memory_order_relaxed);
         read_position != write_position; ++read_position) {
        const MessageRecord& record = channel.records[read_position % MessageChannel::record_count];
        const auto kind_index = static_cast<std::size_t>(record.kind - channel.kinds.data());
        ReportedKind& reported_kind = reader.kinds[kind_index];
        if (!reported_kind.has_unreported_record) {
            try {
                reported_kind.text = record.formatter(record.kind->format, record.arguments);
            } catch (...) {
                reported_kind.text = "(its text couldn't be formatted)";
            }
            reported_kind.has_unreported_record = true;
            reported_kind.unreported_record_tsc = record.tsc;
        }
        channel.read_position.store(read_position + 1, std::memory_order_release);
    }
}

// A line of one poll, kept until every core's are collected, so that they can be written in time
// order.
struct PolledLine {
    timespec wall_clock{};
    MessageType type = MessageType::Log;
    const char* source_name = "";
    std::string_view text;  // its kind's, which stays as it is until the next poll
    u64 count = 0;
};

void collect_message_lines(MessageChannelReader& reader, const PolledCounts& counts,
                           const ClockReading& now, std::vector<PolledLine>& polled_lines) {
    const MessageChannel& channel = *reader.channel;
    for (std::size_t kind_index = 0; kind_index < counts.kind_count; ++kind_index) {
        const MessageKind& kind = channel.kinds[kind_index];
        ReportedKind& reported_kind = reader.kinds[kind_index];
        const u64 occurrence_count = counts.occurrence_counts[kind_index];
        if (occurrence_count == reported_kind.reported_count) {
            continue;
        }
        const timespec wall_clock = reported_kind.has_unreported_record
                                        ? wall_clock_of(reported_kind.unreported_record_tsc, now)
                                        : now.wall_clock;
        const std::string_view text =
            reported_kind.text.empty()
                ? std::string_view("(its text was lost: the core's message ring was full)")
                : std::string_view(reported_kind.text);
        polled_lines.push_back({
            .wall_clock = wall_clock,
            .type = kind.type,
            .source_name = kind.processor_name,
            .text = text,
            .count = occurrence_count - reported_kind.reported_count,
        });
        reported_kind.reported_count = occurrence_count;
        reported_kind.has_unreported_record = false;
    }
}

void report_message_drops(MessageChannelReader& reader) {
    const MessageChannel& channel = *reader.channel;
    const u64 dropped_record_count = channel.dropped_record_count.load(std::memory_order_relaxed);
    if (dropped_record_count > reader.reported_dropped_record_count) {
        log_engine(MessageType::Warning,
                   "core {}'s message ring was full: {} messages from its loop were dropped",
                   reader.core_number, dropped_record_count - reader.reported_dropped_record_count);
        reader.reported_dropped_record_count = dropped_record_count;
    }
    const u64 dropped_occurrence_count =
        channel.dropped_occurrence_count.load(std::memory_order_relaxed);
    if (dropped_occurrence_count > reader.reported_dropped_occurrence_count) {
        log_engine(MessageType::Warning,
                   "core {} raised more than {} kinds of messages: {} occurrences weren't shown",
                   reader.core_number, MessageChannel::kind_capacity,
                   dropped_occurrence_count - reader.reported_dropped_occurrence_count);
        reader.reported_dropped_occurrence_count = dropped_occurrence_count;
    }
}

// Main thread only: at every poll, and once more after the loops end. Every core's lines are
// collected before any is written, since they are written in time order and each core's come kind
// by kind.
void report_messages(std::span<MessageChannelReader> readers) noexcept {
    try {
        std::vector<PolledLine> polled_lines;
        for (MessageChannelReader& reader : readers) {
            // The counts before the records: a core counts an occurrence only once its record is
            // in the ring, so the record of every occurrence counted here is read below.
            const PolledCounts counts = read_message_counts(*reader.channel);
            read_message_records(reader);
            // Read after the records, so no record is newer than it.
            const ClockReading now = read_clocks();
            collect_message_lines(reader, counts, now, polled_lines);
        }
        std::ranges::stable_sort(
            polled_lines, [](const PolledLine& first, const PolledLine& second) {
                return std::tie(first.wall_clock.tv_sec, first.wall_clock.tv_nsec) <
                       std::tie(second.wall_clock.tv_sec, second.wall_clock.tv_nsec);
            });
        for (const PolledLine& line : polled_lines) {
            logger.write(line.type, line.source_name, line.text, line.count, line.wall_clock);
        }
        for (MessageChannelReader& reader : readers) {
            report_message_drops(reader);
        }
    } catch (...) {
        std::fputs("falcon: the cores' messages couldn't be reported\n", stderr);
    }
}
