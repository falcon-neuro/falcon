#pragma once
#include <chrono>
#include <concepts>
#include <cstdint>
#include <format>
#include <print>
#include <string>
#include "../core/engine.cpp"

// EventLogger: prints the events it receives to the console: each one, or how many arrived in each
// second. Printing is a system call, so it runs on a non-realtime core.

enum class LoggingStyle : std::uint8_t { EveryEvent, CountPerSecond };

struct EventLoggerOptions {
    Str label = "EventLogger";  // starts every line, so several loggers can be told apart
    LoggingStyle style = LoggingStyle::EveryEvent;
};

template <std::derived_from<Event> EventType, int Capacity,
          EventLoggerOptions options = EventLoggerOptions{}>
struct EventLogger {
    struct Connections {
        EventInputBuffer<EventType, Capacity> input;
    };

    u64 received = 0;  // events received since the start

    // An event with a channel mask holds several channels, e.g. several spikes, so a count of
    // events is not a count of spikes.
    static constexpr bool has_channel_mask = requires(const EventType& event) { event.channels; };

    void process(Connections connections) {
        if constexpr (options.style == LoggingStyle::EveryEvent) {
            print_every_event(connections);
        } else {
            static_assert(options.style == LoggingStyle::CountPerSecond);
            print_count_per_second(connections);
        }
    }

private:
    bool has_started_ = false;
    std::chrono::steady_clock::time_point second_start_;
    u64 received_this_second_ = 0;
    u64 channels_this_second_ = 0;  // the channels set in the masks of those events

    // Checked on every pass, so a second with no events still prints its 0.
    void print_count_per_second(Connections connections) {
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
                std::println(
                    "{}: {} events with {} channels in the last second ({} events in total)",
                    options.label.c_str(), received_this_second_, channels_this_second_, received);
            } else {
                std::println("{}: {} events in the last second ({} in total)",
                             options.label.c_str(), received_this_second_, received);
            }
            received_this_second_ = 0;
            channels_this_second_ = 0;
            second_start_ += std::chrono::seconds(1);
        }
    }

    void print_every_event(Connections connections) {
        for (const EventType& event : connections.input) {
            ++received;
            std::string line = std::format("{}: event {}", options.label.c_str(), received);
            // Only a stream event's sample number and fields named `channel`, `channels` and
            // `count` are printed.
            if constexpr (std::derived_from<EventType, StreamEvent>) {
                line += std::format(", sample {}", event.header.sample_number);
            }
            if constexpr (requires { event.channel; }) {
                line += std::format(", channel {}", event.channel);
            }
            if constexpr (has_channel_mask) {
                line += ", channels";
                for (const u32 channel : event.channels) {
                    line += std::format(" {}", channel);
                }
            }
            if constexpr (requires { event.count; }) {
                line += std::format(", count {}", event.count);
            }
            std::println("{}", line);
        }
    }
};
