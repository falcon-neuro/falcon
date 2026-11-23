#pragma once
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include "../core/engine.cpp"

// EventLogger: logs the events of its connections, for each connection apart: each event, or how
// many arrived in each second. Its lines go through log(), so at a high rate, events beyond one
// per 100 ms show only as a count. It runs on a non-realtime core.

enum class LoggingStyle : std::uint8_t { EveryEvent, CountPerSecond };

struct EventLoggerOptions {
    /// What it writes to the log: every event, with its sample number and what it holds, or once a
    /// second how many events arrived.
    LoggingStyle style = LoggingStyle::EveryEvent;
};

/// Writes the events it receives to the log: each one, or how many arrived in each second. Every
/// line names the connection it is about. At a high rate the log shows at most one line per 100 ms
/// and counts the rest.
template <EventSlots Inputs, EventLoggerOptions options = EventLoggerOptions{}>
struct EventLogger {
    static constexpr std::size_t slot_count = SLOT_NAMES<Inputs>.size();

    static_assert(slot_count > 0, "an event logger needs at least one connection");

    struct Connections {
        /// The events to log, of any kind, from any number of connections. They are logged and
        /// counted for each connection apart.
        InputSlots<Inputs> inputs;
    };

    explicit EventLogger(ProcessorName processor_name) : name(processor_name) {}

    RegisteredName name;

    void process(Connections connections) {
        std::size_t slot_index = 0;
        if constexpr (options.style == LoggingStyle::EveryEvent) {
            template for (const auto& input : connections.inputs) {
                log_every_event(slot_index, input);
                ++slot_index;
            }
        } else {
            static_assert(options.style == LoggingStyle::CountPerSecond);
            const bool is_second_over = end_second_if_over();
            template for (const auto& input : connections.inputs) {
                count_events(slot_index, input, is_second_over);
                ++slot_index;
            }
        }
    }

private:
    // What one connection has brought.
    struct SlotCounts {
        u64 received = 0;  // events received since the start
        u64 received_this_second = 0;
        u64 channels_this_second = 0;  // the channels set in the masks of those events
    };

    // An event with a channel mask holds several channels, e.g. several spikes, so a count of
    // events is not a count of spikes.
    template <typename EventType>
    static constexpr bool has_channel_mask = requires(const EventType& event) { event.channels; };

    std::array<SlotCounts, slot_count> counts_;
    bool has_started_ = false;
    std::chrono::steady_clock::time_point second_start_;

    // Whether the running second is over, which starts the next one. Asked on every pass, so a
    // second with no events still logs its 0.
    [[nodiscard]] bool end_second_if_over() {
        const auto now = std::chrono::steady_clock::now();
        if (!has_started_) {
            second_start_ = now;
            has_started_ = true;
        }
        if (now - second_start_ < std::chrono::seconds(1)) {
            return false;
        }
        second_start_ += std::chrono::seconds(1);
        return true;
    }

    template <std::derived_from<Event> EventType, int Capacity>
    void count_events(std::size_t slot_index, const EventInputBuffer<EventType, Capacity>& input,
                      bool is_second_over) {
        SlotCounts& counts = counts_[slot_index];
        const auto received_now = static_cast<u64>(input.size());
        counts.received += received_now;
        counts.received_this_second += received_now;
        if constexpr (has_channel_mask<EventType>) {
            for (const EventType& event : input) {
                counts.channels_this_second += static_cast<u64>(event.channels.count());
            }
        }
        if (!is_second_over) {
            return;
        }
        const char* const slot_name = SLOT_NAMES<Inputs>[slot_index].c_str();
        if constexpr (has_channel_mask<EventType>) {
            log(name, "{}: {} events with {} channels in the last second ({} events in total)",
                slot_name, counts.received_this_second, counts.channels_this_second,
                counts.received);
        } else {
            log(name, "{}: {} events in the last second ({} in total)", slot_name,
                counts.received_this_second, counts.received);
        }
        counts.received_this_second = 0;
        counts.channels_this_second = 0;
    }

    // Only a stream event's sample number, and fields named `channels` and `count`, are logged.
    template <std::derived_from<Event> EventType, int Capacity>
    void log_every_event(std::size_t slot_index,
                         const EventInputBuffer<EventType, Capacity>& input) {
        u64& received = counts_[slot_index].received;
        const char* const slot_name = SLOT_NAMES<Inputs>[slot_index].c_str();
        for (const EventType& event : input) {
            ++received;
            if constexpr (StreamEvent<EventType> && has_channel_mask<EventType>) {
                log(name, "{}: event {}, sample {}, channels {}", slot_name, received,
                    event.header.sample_number, event.channels);
            } else if constexpr (StreamEvent<EventType>) {
                log(name, "{}: event {}, sample {}", slot_name, received,
                    event.header.sample_number);
            } else if constexpr (requires { event.count; }) {
                log(name, "{}: event {}, count {}", slot_name, received, event.count);
            } else {
                log(name, "{}: event {}", slot_name, received);
            }
        }
    }
};
