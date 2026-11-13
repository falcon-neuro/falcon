#pragma once
#include <array>
#include <concepts>
#include <cstddef>
#include "../core/engine.cpp"

// Trigger: emits a TriggerEvent once every channel has had an event since its previous one. It
// never reads a clock, so a replay gives the same triggers.

// Its header is that of the sample whose event completed the set.
struct TriggerEvent : StreamEvent {};

// An event on one channel of a stream, such as a Spike or a LevelCrossing. The channel must be a
// u32: a negative int would cast to an index far past Trigger's set of channels.
template <typename EventType>
concept ChannelEvent =
    std::derived_from<EventType, StreamEvent> && requires(const EventType& event) {
        { event.channel } -> std::same_as<const u32&>;
    };

template <typename InputStreamInfo, ChannelEvent EventType>
struct Trigger {
    struct Connections {
        EventInputBuffer<EventType, InputStreamInfo::channels> input;
        EventOutputBuffer<TriggerEvent, 1> output;
    };

    void process(Connections connections) {
        for (const EventType& event : connections.input) {
            const auto channel = static_cast<std::size_t>(event.channel);
            if (has_channel_fired_[channel]) {
                continue;
            }
            has_channel_fired_[channel] = true;
            ++fired_channel_count_;
            if (fired_channel_count_ == InputStreamInfo::channels) {
                connections.output.push(event.header, TriggerEvent{});
                has_channel_fired_.fill(false);
                fired_channel_count_ = 0;
                // The events left in this pass belong to the trigger's sample.
                return;
            }
        }
    }

private:
    std::array<bool, static_cast<std::size_t>(InputStreamInfo::channels)> has_channel_fired_{};
    int fired_channel_count_ = 0;
};
