#pragma once
#include <chrono>
#include <concepts>
#include <cstdint>
#include "../core/engine.cpp"

// EventLogger: logs the events it receives: each one, or how many arrived in each second. Its
// lines go through log(), so at a high rate, events beyond one per 100 ms show only as a count.
// It runs on a non-realtime core.

enum class LoggingStyle : std::uint8_t { EveryEvent, CountPerSecond };

struct EventLoggerOptions {
    LoggingStyle style = LoggingStyle::EveryEvent;
};

template <std::derived_from<Event> EventType, int Capacity,
          EventLoggerOptions options = EventLoggerOptions{}>
struct EventLogger {
    struct Connections {
        EventInputBuffer<EventType, Capacity> input;
    };

    explicit EventLogger(ProcessorName processor_name) : name(processor_name) {}

    RegisteredName name;
    u64 received = 0;  // events received since the start

    // An event with a channel mask holds several channels, e.g. several spikes, so a count of
    // events is not a count of spikes.
    static constexpr bool has_channel_mask = requires(const EventType& event) { event.channels; };

    void process(Connections connections) {
        if constexpr (options.style == LoggingStyle::EveryEvent) {
            log_every_event(connections);
        } else {
            static_assert(options.style == LoggingStyle::CountPerSecond);
            log_count_per_second(connections);
        }
    }

private:
    bool has_started_ = false;
    std::chrono::steady_clock::time_point second_start_;
    u64 received_this_second_ = 0;
    u64 channels_this_second_ = 0;  // the channels set in the masks of those events

    // Checked on every pass, so a second with no events still logs its 0.
    void log_count_per_second(Connections connections) {
        const auto received_now = static_cast<u64>(connections.input.size());
        received += received_now;
        received_this_second_ += received_now;
        if constexpr (has_channel_mask) {
            for (const EventType& event : connections.input) {
                channels_this_second_ += static_cast<u64>(event.channels.count());
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (!has_started_) {
            second_start_ = now;
            has_started_ = true;
        }
        if (now - second_start_ >= std::chrono::seconds(1)) {
            if constexpr (has_channel_mask) {
                log(name, "{} events with {} channels in the last second ({} events in total)",
                    received_this_second_, channels_this_second_, received);
            } else {
                log(name, "{} events in the last second ({} in total)", received_this_second_,
                    received);
            }
            received_this_second_ = 0;
            channels_this_second_ = 0;
            second_start_ += std::chrono::seconds(1);
        }
    }

    // Only a stream event's sample number, and fields named `channels` and `count`, are logged.
    void log_every_event(Connections connections) {
        for (const EventType& event : connections.input) {
            ++received;
            if constexpr (std::derived_from<EventType, StreamEvent> && has_channel_mask) {
                log(name, "event {}, sample {}, channels {}", received, event.header.sample_number,
                    event.channels);
            } else if constexpr (std::derived_from<EventType, StreamEvent>) {
                log(name, "event {}, sample {}", received, event.header.sample_number);
            } else if constexpr (requires { event.count; }) {
                log(name, "event {}, count {}", received, event.count);
            } else {
                log(name, "event {}", received);
            }
        }
    }
};
