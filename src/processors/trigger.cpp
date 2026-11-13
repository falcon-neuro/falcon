#pragma once
#include "../core/engine.cpp"

// Trigger: emits a TriggerEvent once every channel has had an event since its previous one. It
// never reads a clock, so a replay gives the same triggers.

// Its header is that of the sample whose event completed the set.
struct TriggerEvent : StreamEvent {};

template <typename InputStreamInfo, ChannelMaskEvent<InputStreamInfo> EventType>
struct Trigger {
    struct Connections {
        EventInputBuffer<EventType, 1> input;
        EventOutputBuffer<TriggerEvent, 1> output;
    };

    void process(Connections connections) {
        for (const EventType& event : connections.input) {
            fired_channels_ |= event.channels;
            if (fired_channels_ == every_channel) {
                connections.output.push(event.header, TriggerEvent{});
                fired_channels_ = {};
                // The output holds one trigger per pass.
                return;
            }
        }
    }

private:
    static constexpr ChannelMask<InputStreamInfo> every_channel =
        ChannelMask<InputStreamInfo>::every_channel();

    ChannelMask<InputStreamInfo> fired_channels_;
};
