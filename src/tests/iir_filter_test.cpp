// Runs IirFilter under several designs on one test input and writes the input, each design's
// output, its coefficients and the filter they claim to be, for
// scripts/python/compare_iir_filter.py. The files go to the repository's build/iir_filter_test/,
// or to the directory given as the first argument.
#include "../processors/iir_filter.cpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <numbers>
#include <print>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "../core/engine.cpp"
#include "iir_filter_test_designs.cpp"

// 37 channels, padded to 48: on either instruction set every lane of every vector half carries a
// signal of its own (3 AVX512 or 6 AVX2 vectors), and the last vector is partly padding.
using TestStream = StreamInfo<37, 30000.0, float>;

constexpr std::size_t CHANNEL_COUNT = static_cast<std::size_t>(TestStream::channels);
constexpr std::size_t SAMPLE_COUNT = 30000;  // 1 s
// The input's lost-sample count grows at each of these samples, so IirFilter restarts there: twice
// far apart, then on two samples in a row.
constexpr std::array<std::size_t, 3> RESTART_SAMPLES{12000, 20000, 20001};

// Falcon 1's bessel_spike_30000.filter: 4th-order Bessel band-pass, 300-6000 Hz at 30 kHz.
constexpr IirFilterOptions<2> BESSEL_SPIKE_BAND{
    .design_rate = 30000.0,
    .gain = 0.175014382674721,
    .sections =
        {
            {
                {
                    .b0 = 1.0,
                    .b1 = 2.0,
                    .b2 = 1.0,
                    .a1 = -0.4116456379943422,
                    .a2 = 0.1208387236075018,
                },
                {
                    .b0 = 1.0,
                    .b1 = -2.0,
                    .b2 = 1.0,
                    .a1 = -1.888701220483925,
                    .a2 = 0.8929613419279413,
                },
            },
        },
};
// The filter BESSEL_SPIKE_BAND claims to be, in the terms of SciPy's iirfilter, so the comparison
// can redesign it independently. The band-pass has twice the prototype's order.
constexpr std::string_view BESSEL_SPIKE_BAND_DESIGN =
    R"json({"family": "bessel", "band_type": "bandpass", "prototype_order": 2, )json"
    R"json("cutoffs": [300.0, 6000.0], "passband_ripple": null, "stopband_attenuation": null})json";

u64 lost_sample_count_at(std::size_t sample_index) {
    return static_cast<u64>(std::ranges::count_if(
        RESTART_SAMPLES,
        [sample_index](std::size_t restart_sample) { return restart_sample <= sample_index; }));
}

// Every channel carries a different mix, so an output that lands on the wrong channel can't match:
// a DC offset from -45 to +45 mV (checking the start and the restarts in steady state), a chirp
// sweeping 1 Hz to 10 kHz with its own amplitude and phase, white noise, an impulse every 0.1 s at
// its own moment, and a square wave at its own frequency, whose edges a band-pass lets through.
// The noise differs on every run; the comparison reads the input from the file.
std::vector<float> make_test_input() {
    std::vector<float> input_values(SAMPLE_COUNT * CHANNEL_COUNT);
    std::mt19937 random_generator{std::random_device{}()};
    std::normal_distribution<double> noise{0.0, 1.0};
    constexpr double chirp_start_frequency = 1.0;
    constexpr double chirp_end_frequency = 10000.0;
    constexpr double duration = static_cast<double>(SAMPLE_COUNT) / TestStream::rate;
    for (std::size_t sample_index = 0; sample_index < SAMPLE_COUNT; ++sample_index) {
        const double time = static_cast<double>(sample_index) / TestStream::rate;
        const double chirp_phase =
            2.0 * std::numbers::pi *
            ((chirp_start_frequency * time) +
             ((chirp_end_frequency - chirp_start_frequency) * time * time / (2.0 * duration)));
        for (std::size_t channel = 0; channel < CHANNEL_COUNT; ++channel) {
            const auto channel_number = static_cast<double>(channel);
            const double dc_offset = (channel_number - 18.0) * 2500.0;
            const double chirp =
                (100.0 + (20.0 * channel_number)) * std::sin(chirp_phase + (0.7 * channel_number));
            const double white_noise = (20.0 + channel_number) * noise(random_generator);
            const double impulse = (sample_index + (97 * channel)) % 3000 == 0 ? 1000.0 : 0.0;
            const double square_wave =
                std::sin(2.0 * std::numbers::pi * (3.0 + channel_number) * time) >= 0.0 ? 100.0
                                                                                        : -100.0;
            input_values[(sample_index * CHANNEL_COUNT) + channel] =
                static_cast<float>(dc_offset + chirp + white_noise + impulse + square_wave);
        }
    }
    return input_values;
}

void write_input_sample(SampleBuffer<TestStream>& buffer, std::span<const float> input_values,
                        std::size_t sample_index) {
    SampleOutputBuffer<TestStream> writer{buffer};
    writer.set_header({
        .sample_number = sample_index,
        .device_timestamp = sample_index,
        .arrival_tsc = 0,
        .lost_sample_count = lost_sample_count_at(sample_index),
    });
    for (int simd_vector_index = 0; simd_vector_index < TestStream::simd_vectors;
         ++simd_vector_index) {
        alignas(64) std::array<float, FLOATS_PER_SIMD_VECTOR> channel_values{};
        for (int lane_index = 0; lane_index < FLOATS_PER_SIMD_VECTOR; ++lane_index) {
            const int channel = (simd_vector_index * FLOATS_PER_SIMD_VECTOR) + lane_index;
            if (channel < TestStream::channels) {
                const std::size_t value_index =
                    (sample_index * CHANNEL_COUNT) + static_cast<std::size_t>(channel);
                channel_values[static_cast<std::size_t>(lane_index)] = input_values[value_index];
            }
        }
#ifdef __AVX512F__
        writer.store(simd_vector_index, _mm512_load_ps(channel_values.data()));
#else
        writer.store(simd_vector_index, _mm256_load_ps(channel_values.data()));
#endif
    }
}

void read_output_sample(const SampleBuffer<TestStream>& buffer, std::span<float> output_values,
                        std::size_t sample_index) {
    const SampleInputBuffer<TestStream> reader{buffer};
    if (reader.is_empty()) {
        throw std::runtime_error(
            std::format("IirFilter gave no output for sample {}", sample_index));
    }
    if (reader.header.sample_number != sample_index ||
        reader.header.lost_sample_count != lost_sample_count_at(sample_index)) {
        throw std::runtime_error(
            std::format("IirFilter's output header for sample {} isn't its input's", sample_index));
    }
    for (int simd_vector_index = 0; simd_vector_index < TestStream::simd_vectors;
         ++simd_vector_index) {
        alignas(64) std::array<float, FLOATS_PER_SIMD_VECTOR> channel_values{};
#ifdef __AVX512F__
        _mm512_store_ps(channel_values.data(), reader.load(simd_vector_index));
#else
        _mm256_store_ps(channel_values.data(), reader.load(simd_vector_index));
#endif
        for (int lane_index = 0; lane_index < FLOATS_PER_SIMD_VECTOR; ++lane_index) {
            const int channel = (simd_vector_index * FLOATS_PER_SIMD_VECTOR) + lane_index;
            if (channel < TestStream::channels) {
                output_values[(sample_index * CHANNEL_COUNT) + static_cast<std::size_t>(channel)] =
                    channel_values[static_cast<std::size_t>(lane_index)];
            }
        }
    }
}

// Little-endian float32, sample after sample, each sample's channels in order.
void write_floats(const std::filesystem::path& path, std::span<const float> values) {
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char*>(values.data()),
               static_cast<std::streamsize>(values.size_bytes()));
    if (!file) {
        throw std::runtime_error("can't write " + path.string());
    }
}

// std::format writes each double in its shortest form that reads back exactly.
template <int SectionCount>
std::string describe_design(const IirFilterOptions<SectionCount>& options, std::string_view name,
                            std::string_view design) {
    std::string sections;
    for (std::size_t section_index = 0; section_index < options.sections.size(); ++section_index) {
        const BiquadSection& section = options.sections[section_index];
        sections += std::format("{}[{}, {}, {}, {}, {}]", section_index == 0 ? "" : ", ",
                                section.b0, section.b1, section.b2, section.a1, section.a2);
    }
    return std::format(
        "{{\"name\": \"{}\", \"design\": {}, \"design_rate\": {}, \"gain\": {}, "
        "\"sections\": [{}]}}",
        name, design, options.design_rate, options.gain, sections);
}

// The input every design filters, and the descriptions of the designs run so far.
struct TestRun {
    std::filesystem::path directory;
    std::vector<float> input_values;
    std::vector<std::string> design_descriptions;
};

// Filters the test input with these coefficients and writes the output as <name>.f32. `design`
// is a JSON object naming the filter the coefficients claim to be, for the comparison to redesign.
template <IirFilterOptions options>
void run_design(TestRun& test_run, std::string_view name, std::string_view design) {
    std::vector<float> output_values(test_run.input_values.size());
    IirFilter<TestStream, options> iir_filter;
    SampleBuffer<TestStream> input_buffer;
    SampleBuffer<TestStream> output_buffer;
    for (std::size_t sample_index = 0; sample_index < SAMPLE_COUNT; ++sample_index) {
        write_input_sample(input_buffer, test_run.input_values, sample_index);
        iir_filter.process({.input = input_buffer, .output = output_buffer});
        read_output_sample(output_buffer, output_values, sample_index);
    }
    write_floats(test_run.directory / std::format("{}.f32", name), output_values);
    test_run.design_descriptions.push_back(describe_design(options, name, design));
}

std::string describe_test(const TestRun& test_run) {
    std::string restart_samples;
    for (std::size_t restart_index = 0; restart_index < RESTART_SAMPLES.size(); ++restart_index) {
        restart_samples +=
            std::format("{}{}", restart_index == 0 ? "" : ", ", RESTART_SAMPLES[restart_index]);
    }
    std::string designs;
    for (std::size_t design_index = 0; design_index < test_run.design_descriptions.size();
         ++design_index) {
        designs += std::format("{}\n    {}", design_index == 0 ? "" : ",",
                               test_run.design_descriptions[design_index]);
    }
    return std::format(
        "{{\"channel_count\": {}, \"sample_count\": {}, \"sampling_rate\": {}, "
        "\"restart_samples\": [{}], \"designs\": [{}\n]}}\n",
        CHANNEL_COUNT, SAMPLE_COUNT, TestStream::rate, restart_samples, designs);
}

void run_test(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    TestRun test_run{
        .directory = directory,
        .input_values = make_test_input(),
        .design_descriptions = {},
    };
    write_floats(directory / "input.f32", test_run.input_values);

    run_design<BESSEL_SPIKE_BAND>(test_run, "bessel_spike_band", BESSEL_SPIKE_BAND_DESIGN);
    for_each_generated_design(
        [&test_run]<IirFilterOptions options>(std::string_view name, std::string_view design) {
            run_design<options>(test_run, name, design);
        });

    std::ofstream description_file{directory / "test.json"};
    description_file << describe_test(test_run);
    if (!description_file) {
        throw std::runtime_error("can't write " + (directory / "test.json").string());
    }
    std::println("IirFilter test: {} designs written to {}", test_run.design_descriptions.size(),
                 std::filesystem::absolute(directory).string());
}

int main(int argument_count, char** arguments) {
    try {
        // __FILE__ is this source file's path as given to the compiler, absolute in a CMake build,
        // so the default doesn't depend on the folder the program is started from.
        const std::filesystem::path repository_root =
            std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path();
        const std::filesystem::path directory = argument_count > 1
                                                    ? std::filesystem::path{arguments[1]}
                                                    : repository_root / "build" / "iir_filter_test";
        run_test(directory);
        return 0;
    } catch (const std::exception& error) {
        std::fputs("IirFilter test: ", stderr);
        std::fputs(error.what(), stderr);
        std::fputs("\n", stderr);
        return 1;
    }
}
