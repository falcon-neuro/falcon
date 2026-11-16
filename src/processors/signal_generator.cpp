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
#include <thread>
#include "../core/engine.cpp"

// SignalGenerator: stands in for acquisition hardware when testing. It outputs a sine wave, a
// square wave, a train of spike-shaped pulses or bursts of a ripple-band oscillation in µV on every
// channel at the output stream's rate, optionally on top of a slow LFP sine, so a filter's effect
// can be seen. As an input it runs alone
// on its core, so it may block that core's thread: it sleeps until shortly before each sample is
// due, then busy-spins to the exact time. Sleeping alone wakes too late for a 33 µs period, and
// spinning alone keeps the core at 100%. The spin adapts: it lasts the minimum plus the worst
// recent wake-up delay, so after a late wake-up the generator sleeps less for a while, and stops
// sleeping if wake-ups take a whole period.

enum class Waveform : std::uint8_t { Sine, Square, SpikeTrain, RippleBursts };

struct SignalGeneratorOptions {
    /// The shape of the signal: a sine wave, a square wave, a train of spike-shaped pulses, or
    /// bursts of a ripple-band oscillation.
    Waveform waveform = Waveform::Sine;
    /// How often the shape repeats, in Hz: the wave's frequency, for a spike train the spikes per
    /// second, for ripple bursts the bursts per second. It must be above 0 and below half the
    /// stream's rate.
    double frequency = 10;
    /// The signal's size, in µV from zero to peak: for a spike train the depth of each trough, for
    /// ripple bursts the oscillation's amplitude at a burst's peak. It can't be negative.
    double amplitude = 100;
    /// How far each channel is shifted against the one before it, in radians, so that channels can
    /// be told apart in a viewer. 0 gives every channel the same signal.
    double phase_step_per_channel = 1;
    /// The frequency of a slow LFP wave added under the signal, in Hz. It counts only when that
    /// wave's amplitude is above 0, and must then be above 0 and below half the stream's rate.
    double lfp_frequency = 0;
    /// The size of that slow LFP wave, in µV from zero to peak. 0 adds none; it can't be negative.
    double lfp_amplitude = 0;
    /// For ripple bursts only: the frequency of the oscillation inside the bursts, in Hz. The
    /// oscillation never stops; its amplitude swells within each burst. It must be above 0 and
    /// below half the stream's rate.
    double ripple_frequency = 180;
    /// For ripple bursts only: how long a burst lasts from its start to its end, in milliseconds.
    /// It must be above 0, and short enough that bursts don't overlap.
    double ripple_burst_duration_milliseconds = 80;
    /// For ripple bursts only: the oscillation's amplitude between bursts, in µV from zero to peak.
    /// It can't be negative or above the amplitude at a burst's peak.
    double ripple_background_amplitude = 10;
    /// The shortest time before each sample is due at which the generator stops sleeping and waits
    /// actively, in microseconds. A larger value keeps the samples on time at the price of a busier
    /// core. It can't be negative.
    double minimum_busy_spin_microseconds = 10;
};

/// Stands in for acquisition hardware when testing: it makes a signal on every channel at the
/// stream's rate, a sine wave, a square wave, a train of spike-shaped pulses or bursts of a ripple-
/// band oscillation, optionally on top of a slow LFP wave. It keeps the samples on time itself, so
/// it needs a core of its own.
template <typename OutputStreamInfo, SignalGeneratorOptions options>
struct SignalGenerator {
    static_assert(options.frequency > 0, "the frequency must be above 0");
    static_assert(options.frequency < OutputStreamInfo::rate / 2,
                  "the wave must be below the Nyquist frequency, or it aliases");
    static_assert(options.amplitude >= 0, "the amplitude can't be negative");
    static_assert(options.lfp_amplitude >= 0, "the LFP wave's amplitude can't be negative");
    static_assert(options.lfp_amplitude == 0 ||
                      (options.lfp_frequency > 0 &&
                       options.lfp_frequency < OutputStreamInfo::rate / 2),
                  "the LFP needs a frequency above 0 and below the Nyquist frequency");
    static_assert(options.waveform != Waveform::RippleBursts ||
                      (options.ripple_frequency > 0 &&
                       options.ripple_frequency < OutputStreamInfo::rate / 2),
                  "the ripple needs a frequency above 0 and below the Nyquist frequency");
    static_assert(options.waveform != Waveform::RippleBursts ||
                      (options.ripple_burst_duration_milliseconds > 0 &&
                       options.frequency * options.ripple_burst_duration_milliseconds < 1000),
                  "bursts this frequent would overlap");
    static_assert(options.waveform != Waveform::RippleBursts ||
                      (options.ripple_background_amplitude >= 0 &&
                       options.ripple_background_amplitude <= options.amplitude),
                  "a burst swells up from the background's amplitude, so its peak can't be below "
                  "it");
    static_assert(options.minimum_busy_spin_microseconds >= 0,
                  "the time of active waiting can't be negative");

    struct Connections {
        /// The generated signal: one sample at a time at the stream's rate, the same shape on every
        /// channel, each shifted by the phase step.
        SampleOutputBuffer<OutputStreamInfo> output;
    };

    // Runs on the core's own thread, so it sets that thread's timer slack: Linux lets a normal
    // thread's sleep end up to 50 µs late, longer than a whole period at 30 kHz.
    explicit SignalGenerator(ProcessorName processor_name) : name(processor_name) {
        if (prctl(PR_SET_TIMERSLACK, 1UL) != 0) {
            fail(name, "can't set the thread's timer slack");
        }
    }

    RegisteredName name;

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
        const CycleFractions cycle_fractions{
            .wave = std::fmod(options.frequency * time, 1.0),
            .lfp = std::fmod(options.lfp_frequency * time, 1.0),
            .ripple = std::fmod(options.ripple_frequency * time, 1.0),
        };

        for (int simd_vector_index = 0; simd_vector_index < OutputStreamInfo::simd_vectors;
             ++simd_vector_index) {
            // Padding channels stay 0.
            alignas(64) std::array<float, FLOATS_PER_SIMD_VECTOR> channel_values{};
            for (int lane_index = 0; lane_index < FLOATS_PER_SIMD_VECTOR; ++lane_index) {
                const int channel = (simd_vector_index * FLOATS_PER_SIMD_VECTOR) + lane_index;
                if (channel < OutputStreamInfo::channels) {
                    channel_values[static_cast<std::size_t>(lane_index)] =
                        static_cast<float>(channel_value(cycle_fractions, channel));
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

    static constexpr double ripple_burst_duration =
        options.ripple_burst_duration_milliseconds / 1000.0;  // s

    // The oscillation's amplitude a given time after a burst started: a raised cosine from the
    // background's amplitude up to the burst's peak and back, so a burst starts and ends smoothly
    // and at a known sample.
    [[nodiscard]] static double ripple_amplitude(double time_since_burst_start) {
        if (time_since_burst_start >= ripple_burst_duration) {
            return options.ripple_background_amplitude;
        }
        const double swell = 0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * time_since_burst_start /
                                                   ripple_burst_duration));
        return options.ripple_background_amplitude +
               ((options.amplitude - options.ripple_background_amplitude) * swell);
    }

    // Where the current sample falls within each wave's period, from 0 to 1.
    struct CycleFractions {
        double wave = 0;
        double lfp = 0;
        double ripple = 0;
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
        } else if constexpr (options.waveform == Waveform::SpikeTrain) {
            // The phase offset shifts each channel's spikes by that fraction of the period.
            const double channel_cycle_fraction =
                std::fmod(cycle_fraction + (phase_offset / (2.0 * std::numbers::pi)), 1.0);
            value = spike_value(channel_cycle_fraction / options.frequency);
        } else {
            static_assert(options.waveform == Waveform::RippleBursts);
            // A burst comes on every channel at once, as a ripple does: the phase offset shifts
            // only the oscillation inside it.
            const double ripple_phase =
                (2.0 * std::numbers::pi * cycle_fractions.ripple) + phase_offset;
            value = ripple_amplitude(cycle_fraction / options.frequency) * std::sin(ripple_phase);
        }
        if constexpr (options.lfp_amplitude > 0) {
            value += options.lfp_amplitude *
                     std::sin((2.0 * std::numbers::pi * lfp_cycle_fraction) + phase_offset);
        }
        return value;
    }
};
