#pragma once
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <system_error>
#include "../core/engine.cpp"

// SampleRecorder: writes every sample of a stream to a file in the run's directory, as raw
// little-endian records with no file header: each sample's number as a u64, then its channels'
// values in order as float32. The numbers match a sample with the events it caused, and a jump in
// them shows where samples were lost. Writing to a file can block, so it runs on a core that isn't
// realtime, behind a ring long enough to hold what arrives while it writes.

struct SampleRecorderOptions {
    Str file_name = "recording.bin";  // in the run's directory
};

template <typename InputStreamInfo, SampleRecorderOptions options = SampleRecorderOptions{}>
struct SampleRecorder {
    static_assert(is_file_name(options.file_name),
                  "file_name is a name inside the run's directory, without a /");

    struct Connections {
        SampleInputBuffer<InputStreamInfo> input;
    };

    explicit SampleRecorder(ProcessorName processor_name) : name(processor_name) {
        path_ = (run_directory() / options.file_name.c_str()).string();
        file_ = create_file(name, path_);
        log(name, "recording {} channels at {} Hz to {}", InputStreamInfo::channels,
            InputStreamInfo::rate, path_.c_str());
    }

    // A destructor must not throw, so a failure to write the last samples is warned.
    ~SampleRecorder() {
        if (std::fclose(file_.release()) != 0) {
            warn(name, "can't finish writing {}, which may lack its last samples", path_.c_str());
            return;
        }
        log(name, "recorded {} samples to {}", recorded_sample_count_, path_.c_str());
    }

    SampleRecorder(const SampleRecorder&) = delete;
    SampleRecorder& operator=(const SampleRecorder&) = delete;
    SampleRecorder(SampleRecorder&&) = delete;
    SampleRecorder& operator=(SampleRecorder&&) = delete;

    RegisteredName name;

    void process(Connections connections) {
        if (connections.input.is_empty()) {
            return;
        }

        alignas(64) std::array<float, InputStreamInfo::padded_channels> values{};
        for (int simd_vector_index = 0; simd_vector_index < InputStreamInfo::simd_vectors;
             ++simd_vector_index) {
            float* const vector_values = &values[static_cast<std::size_t>(simd_vector_index) *
                                                 static_cast<std::size_t>(FLOATS_PER_SIMD_VECTOR)];
            // auto, not SimdVector: a type that depends on the template keeps the other
            // instruction set's branch from being compiled.
            const auto loaded_values = connections.input.load(simd_vector_index);
            if constexpr (HAS_AVX512) {
                _mm512_store_ps(vector_values, loaded_values);
            } else {
                _mm256_store_ps(vector_values, loaded_values);
            }
        }

        // Two writes, not one struct, whose padding after an odd number of channels would end up
        // in the file. Only the real channels: padding channels are not part of the stream.
        const u64 sample_number = connections.input.header.sample_number;
        const auto channel_count = static_cast<std::size_t>(InputStreamInfo::channels);
        const bool is_written =
            std::fwrite(&sample_number, sizeof(sample_number), 1, file_.get()) == 1 &&
            std::fwrite(values.data(), sizeof(float), channel_count, file_.get()) == channel_count;
        if (!is_written) {
            fail(name, "can't write to {}: {}", path_.c_str(),
                 std::system_category().message(errno).c_str());
        }
        ++recorded_sample_count_;
    }

private:
    struct FileCloser {
        void operator()(std::FILE* file) const noexcept { static_cast<void>(std::fclose(file)); }
    };

    std::string path_;
    std::unique_ptr<std::FILE, FileCloser> file_;
    u64 recorded_sample_count_ = 0;

    // "x" makes fopen fail if the file exists, so a second recorder with the same file name can't
    // overwrite the first's recording.
    [[nodiscard]] static std::unique_ptr<std::FILE, FileCloser> create_file(
        ProcessorName processor_name, const std::string& path) {
        std::FILE* const file = std::fopen(path.c_str(), "wbx");
        if (file == nullptr) {
            fail(processor_name, "can't create {}: {}", path.c_str(),
                 std::system_category().message(errno).c_str());
        }
        return std::unique_ptr<std::FILE, FileCloser>(file);
    }
};
