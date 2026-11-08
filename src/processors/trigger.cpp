#pragma once
#include <array>
#include <cstddef>
#include "../core/engine.cpp"
#include "spike_detector.cpp"

// Trigger: emits a TriggerEvent once every channel has spiked since its previous one. It never
// reads a clock, so a replay gives the same triggers.

// Its header is that of the sample whose spike completed the set.
struct TriggerEvent : StreamEventRecord {};

template <typename InputStreamInfo>
struct Trigger {
    struct Connections {
        EventInputBuffer<Spike, InputStreamInfo::channels> input;
        EventOutputBuffer<TriggerEvent, 1> output;
    };

    void process(Connections connections) {
        for (const Spike& spike : connections.input) {
            const auto channel = static_cast<std::size_t>(spike.channel);
            if (has_channel_spiked_[channel]) {
                continue;
            }
            has_channel_spiked_[channel] = true;
            ++spiked_channel_count_;
            if (spiked_channel_count_ == InputStreamInfo::channels) {
                connections.output.push(spike.header, TriggerEvent{});
                has_channel_spiked_.fill(false);
                spiked_channel_count_ = 0;
                // The spikes left in this pass belong to the trigger's sample.
                return;
            }
        }
    }

private:
    std::array<bool, static_cast<std::size_t>(InputStreamInfo::channels)> has_channel_spiked_{};
    int spiked_channel_count_ = 0;
};
