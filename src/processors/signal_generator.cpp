#pragma once
#include <sys/prctl.h>
#include <x86intrin.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <stdexcept>
#include <thread>
#include "../core/engine.cpp"

// SignalGenerator: stands in for acquisition hardware when testing. It outputs a sine wave, a
// square wave or a train of spike-shaped pulses in µV on every channel at the output stream's rate,
// optionally on top of a slow LFP sine, so a filter's effect can be seen. As an input it runs alone
// on its core, so it may block that core's thread: it sleeps until shortly before each sample is
// due, then busy-spins to the exact time. Sleeping alone wakes too late for a 33 µs period, and
// spinning alone keeps the core at 100%. The spin adapts: it lasts the minimum plus the worst
// recent wake-up delay, so after a late wake-up the generator sleeps less for a while, and stops
// sleeping if wake-ups take a whole period.

enum class Waveform : std::uint8_t { Sine, Square, SpikeTrain };

struct SignalGeneratorOptions {
    Waveform waveform = Waveform::Sine;
    double frequency = 10;   // Hz; for SpikeTrain, spikes per second
    double amplitude = 100;  // µV, from zero to peak; for SpikeTrain, the depth of each trough
    double phase_step_per_channel = 1;  // radians, so channels can be told apart in a viewer
    double lfp_frequency = 0;           // Hz
    double lfp_amplitude = 0;           // µV, from zero to peak; 0 adds no LFP
    double minimum_busy_spin_microseconds = 10;  // before each sample is due
};

template <typename OutputStreamInfo, SignalGeneratorOptions options>
struct SignalGenerator {
    static_assert(options.frequency > 0);
    static_assert(options.frequency < OutputStreamInfo::rate / 2,
                  "the wave must be below the Nyquist frequency, or it aliases");
    static_assert(options.amplitude >= 0);
    static_assert(options.lfp_amplitude >= 0);
    static_assert(options.lfp_amplitude == 0 ||
                      (options.lfp_frequency > 0 &&
                       options.lfp_frequency < OutputStreamInfo::rate / 2),
                  "the LFP needs a frequency above 0 and below the Nyquist frequency");
    static_assert(options.minimum_busy_spin_microseconds >= 0);

    struct Connections {
        SampleOutputBuffer<OutputStreamInfo> output;
    };

    // Runs on the core's own thread, so it sets that thread's timer slack: Linux lets a normal
    // thread's sleep end up to 50 µs late, longer than a whole period at 30 kHz.
    SignalGenerator() {
        if (prctl(PR_SET_TIMERSLACK, 1UL) != 0) {
            throw std::runtime_error("SignalGenerator: can't set the thread's timer slack");
        }
    }

    void process(Connections connections) {
        using std::chrono::steady_clock;
        if (!has_started_) {
            start_time_ = steady_clock::now();
            has_started_ = true;
        }
        // A late pass sleeps and spins not at all; the samples it missed follow one per pass, like
        // a burst from hardware.
        const steady_clock::time_point due = due_time();
        const steady_clock::time_point busy_spin_start = due - busy_spin_duration();
        if (steady_clock::now() < busy_spin_start) {
            std::this_thread::sleep_until(busy_spin_start);
            record_wake_up_delay(steady_clock::now() - busy_spin_start);
        }
        while (steady_clock::now() < due) {
            _mm_pause();
        }
        worst_wake_up_delay_nanoseconds_ *= wake_up_delay_decay_per_sample_;

        // Kept within one period, so the phase doesn't lose precision as sample_number_ grows.
        const double time = static_cast<double>(sample_number_) / OutputStreamInfo::rate;
        const double cycle_fraction = std::fmod(options.frequency * time, 1.0);
        const double lfp_cycle_fraction = std::fmod(options.lfp_frequency * time, 1.0);

        for (int simd_vector_index = 0; simd_vector_index < OutputStreamInfo::simd_vectors;
             ++simd_vector_index) {
            // Padding channels stay 0.
            alignas(64) std::array<float, FLOATS_PER_SIMD_VECTOR> channel_values{};
            for (int lane_index = 0; lane_index < FLOATS_PER_SIMD_VECTOR; ++lane_index) {
                const int channel = (simd_vector_index * FLOATS_PER_SIMD_VECTOR) + lane_index;
                if (channel < OutputStreamInfo::channels) {
                    channel_values[static_cast<std::size_t>(lane_index)] =
                        static_cast<float>(channel_value(
                            {.wave = cycle_fraction, .lfp = lfp_cycle_fraction}, channel));
                }
            }
            if constexpr (HAS_AVX512) {
                connections.output.store(simd_vector_index, _mm512_load_ps(channel_values.data()));
            } else {
                connections.output.store(simd_vector_index, _mm256_load_ps(channel_values.data()));
            }
        }

        // Stamped only once every channel is computed, as a hardware sample arrives only once its
        // packet is complete, so the latency log counts none of the generation.
        connections.output.set_header({
            .sample_number = sample_number_,
            .device_timestamp = sample_number_,
            .arrival_tsc = __rdtsc(),
            .lost_sample_count = 0,
        });

        ++sample_number_;
    }

private:
    static constexpr auto minimum_busy_spin = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double, std::micro>(options.minimum_busy_spin_microseconds));

    bool has_started_ = false;
    std::chrono::steady_clock::time_point start_time_;
    u64 sample_number_ = 0;
    // Jumps up at once with a late wake-up and halves every second, so one slow wake-up makes the
    // generator sleep less for a while instead of being late again on the next sample.
    double worst_wake_up_delay_nanoseconds_ = 0;
    const double wake_up_delay_decay_per_sample_ = std::exp2(-1.0 / OutputStreamInfo::rate);

    [[nodiscard]] std::chrono::nanoseconds busy_spin_duration() const {
        return minimum_busy_spin +
               std::chrono::nanoseconds(static_cast<std::chrono::nanoseconds::rep>(
                   std::llround(worst_wake_up_delay_nanoseconds_)));
    }

    void record_wake_up_delay(std::chrono::steady_clock::duration wake_up_delay) {
        const auto wake_up_delay_nanoseconds = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(wake_up_delay).count());
        worst_wake_up_delay_nanoseconds_ =
            std::max(worst_wake_up_delay_nanoseconds_, wake_up_delay_nanoseconds);
    }

    // On a fixed schedule from the first pass, so the samples don't drift.
    [[nodiscard]] std::chrono::steady_clock::time_point due_time() const {
        return start_time_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                 std::chrono::duration<double>(static_cast<double>(sample_number_) /
                                                               OutputStreamInfo::rate));
    }

    // One spike, shaped like an extracellular spike: a sharp trough 0.3 ms after it starts, then a
    // smaller, slower positive rebound. Its whole shape fits in 2 ms.
    struct GaussianPulse {
        double center = 0;  // s after the spike starts
        double width = 0;   // s, the Gaussian's sigma
    };
    static constexpr GaussianPulse spike_trough{.center = 0.3e-3, .width = 0.1e-3};
    static constexpr GaussianPulse spike_rebound{.center = 0.7e-3, .width = 0.25e-3};
    static constexpr double spike_rebound_height = 0.3;  // of the trough's depth
    static constexpr double spike_duration = 2e-3;       // s
    static_assert(options.waveform != Waveform::SpikeTrain ||
                      options.frequency * spike_duration < 1,
                  "spikes this frequent would overlap");

    [[nodiscard]] static double gaussian(double time, GaussianPulse pulse) {
        const double distance = (time - pulse.center) / pulse.width;
        return std::exp(-0.5 * distance * distance);
    }

    [[nodiscard]] static double spike_value(double time_since_spike) {
        if (time_since_spike >= spike_duration) {
            return 0.0;
        }
        return options.amplitude *
               ((spike_rebound_height * gaussian(time_since_spike, spike_rebound)) -
                gaussian(time_since_spike, spike_trough));
    }

    // Where the current sample falls within each wave's period, from 0 to 1.
    struct CycleFractions {
        double wave = 0;
        double lfp = 0;
    };

    [[nodiscard]] static double channel_value(CycleFractions cycle_fractions, int channel) {
        const double cycle_fraction = cycle_fractions.wave;
        const double lfp_cycle_fraction = cycle_fractions.lfp;
        const double phase_offset = options.phase_step_per_channel * static_cast<double>(channel);
        const double phase = (2.0 * std::numbers::pi * cycle_fraction) + phase_offset;
        double value = 0.0;
        if constexpr (options.waveform == Waveform::Sine) {
            value = options.amplitude * std::sin(phase);
        } else if constexpr (options.waveform == Waveform::Square) {
            value = std::sin(phase) >= 0.0 ? options.amplitude : -options.amplitude;
        } else {
            static_assert(options.waveform == Waveform::SpikeTrain);
            // The phase offset shifts each channel's spikes by that fraction of the period.
            const double channel_cycle_fraction =
                std::fmod(cycle_fraction + (phase_offset / (2.0 * std::numbers::pi)), 1.0);
            value = spike_value(channel_cycle_fraction / options.frequency);
        }
        if constexpr (options.lfp_amplitude > 0) {
            value += options.lfp_amplitude *
                     std::sin((2.0 * std::numbers::pi * lfp_cycle_fraction) + phase_offset);
        }
        return value;
    }
};
