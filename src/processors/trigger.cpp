#pragma once
#include "../core/engine.cpp"

// Trigger: emits a TriggerEvent once every channel has had an event since its previous one. It
// never reads a clock, so a replay gives the same triggers.

// Its header is that of the sample whose event completed the set.
struct TriggerEvent : StreamEvent {};

/// Decides when to act: it emits a trigger event once every channel of a stream has had an event
/// since the previous trigger. It is built for measuring latency with a test signal on every
/// channel; on real data one quiet channel keeps it from firing.
template <typename InputStreamInfo, ChannelMaskEvent<InputStreamInfo> EventType>
struct Trigger {
    struct Connections {
        /// Events that each name the channels something happened on, e.g. spikes or level
        /// crossings.
        EventInputBuffer<EventType, 1> input;
        /// One trigger event each time every channel of the stream has been named at least once
        /// since the previous trigger, on the sample whose event named the last missing channel.
        /// A channel that never has an event keeps the trigger from ever firing.
        EventOutputBuffer<TriggerEvent, 1> output;
    };

    explicit Trigger(ProcessorName processor_name) : name(processor_name) {}

    RegisteredName name;

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
