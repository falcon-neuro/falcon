#pragma once
#include <algorithm>
#include <array>
#include <cerrno>
#include <concepts>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include "../core/engine.cpp"

// Recorder: writes what all its connections bring, samples and events alike, into one file in the
// run's directory. The file starts with one line of JSON that names each connection and says how
// its records are laid out. Chunks follow, each a u32 for the connection, a u32 for the number of
// records, then that many records of that connection. A sample's record is its number as a u64
// and its channels' values in order as float32. An event's record is its header: the sample
// number, device timestamp, arrival TSC and lost-sample count, each a u64. All little-endian.
// Writing to a file can block, so it runs on a core that isn't realtime, behind rings.

// How one connection of a Recorder is written.
struct RecorderSlotFormat {
    bool is_sample_stream = false;
    int channels = 0;  // of a sample stream
    double rate = 0;   // Hz, of a sample stream
    std::size_t record_bytes = 0;
    std::size_t chunk_records = 0;  // how many records are gathered before they are written
};

// Defined for each kind of connection a Recorder can write.
template <typename Kind>
struct RecorderSlotFormatOf;

template <typename StreamInfoType>
struct RecorderSlotFormatOf<SampleBuffer<StreamInfoType>> {
    // 1024 samples are about 30 ms at 30 kHz: few chunks for a reader to walk through, and little
    // lost if the run is killed.
    static constexpr RecorderSlotFormat value{
        .is_sample_stream = true,
        .channels = StreamInfoType::channels,
        .rate = StreamInfoType::rate,
        .record_bytes =
            sizeof(u64) + (static_cast<std::size_t>(StreamInfoType::channels) * sizeof(float)),
        .chunk_records = 1024,
    };
};

template <std::derived_from<StreamEvent> EventType, int Capacity>
struct RecorderSlotFormatOf<EventBuffer<EventType, Capacity>> {
    static_assert(sizeof(EventType) == sizeof(StreamEvent),
                  "only an event's header is written, so an event with fields of its own would "
                  "lose them");
    static_assert(sizeof(SampleHeader) == 4 * sizeof(u64),
                  "an event's record is the header's four u64 fields, with no padding");

    // Events are rare, so a pass's events are written at once, never held back for a full chunk.
    static constexpr RecorderSlotFormat value{
        .is_sample_stream = false,
        .record_bytes = sizeof(SampleHeader),
        .chunk_records = static_cast<std::size_t>(Capacity),
    };
};

template <typename Kind>
concept RecordableKind = requires { RecorderSlotFormatOf<Kind>::value; };

// A slot's name goes into the file's JSON as it is, so it holds nothing JSON would escape.
[[nodiscard]] constexpr bool is_recorder_slot_name(const Str& text) {
    const std::string_view name = text.c_str();
    return !name.empty() && std::ranges::all_of(name, [](char character) {
        return is_letter_or_digit(character) || character == '_' || character == '-';
    });
}

template <int SlotCount>
struct RecorderOptions {
    /// The name of the file, in the run's directory. A name only, without a folder.
    Str file_name = "recording.bin";
    /// What the file calls each connection, in the order they are connected: one name each, of
    /// letters, digits, _ and - only.
    std::array<Str, SlotCount> slot_names{};
};

// `options` comes first and its type is checked below: a template's list of kinds has to come
// last, and the options' type depends on how many kinds there are.
/// Writes everything its connections bring, samples and events alike, into one file in the run's
/// directory, to look at after the run. Writing to a file can stall, so it belongs on a core that
/// isn't realtime.
template <auto options, RecordableKind... InputKinds>
struct Recorder {
    static constexpr std::size_t slot_count = sizeof...(InputKinds);

    static_assert(slot_count > 0, "a recorder needs at least one connection");
    static_assert(std::same_as<decltype(options), RecorderOptions<static_cast<int>(slot_count)>>,
                  "options is a RecorderOptions<N>, with a name for each of the N connections");
    static_assert(is_file_name(options.file_name),
                  "file_name is a name inside the run's directory, without a /");
    static_assert(std::ranges::all_of(options.slot_names, is_recorder_slot_name),
                  "a slot's name holds only letters, digits, _ and -");

    struct Connections {
        /// Whatever is to be recorded: any number of connections, each bringing samples or events.
        /// Of an event, only its sample number and its times are written.
        InputSlots<InputKinds...> inputs;
    };

    explicit Recorder(ProcessorName processor_name) : name(processor_name) {
        for (std::size_t slot_index = 0; slot_index < slot_count; ++slot_index) {
            const RecorderSlotFormat& format = slot_formats[slot_index];
            slots_[slot_index].staged_records.resize(format.chunk_records * format.record_bytes);
        }
        path_ = (run_directory() / options.file_name.c_str()).string();
        file_ = create_file(name, path_);
        const std::string description = file_description();
        if (std::fwrite(description.data(), 1, description.size(), file_.get()) !=
            description.size()) {
            fail(name, "can't write to {}: {}", path_.c_str(),
                 std::system_category().message(errno).c_str());
        }
        log(name, "recording {} connections to {}", slot_count, path_.c_str());
    }

    // A destructor must not throw, so a failure to write the last records is warned.
    ~Recorder() {
        bool is_complete = true;
        for (std::size_t slot_index = 0; slot_index < slot_count; ++slot_index) {
            is_complete = write_chunk(slot_index) && is_complete;
        }
        is_complete = std::fclose(file_.release()) == 0 && is_complete;
        if (!is_complete) {
            warn(name, "can't finish writing {}, which may lack its last records", path_.c_str());
            return;
        }
        for (std::size_t slot_index = 0; slot_index < slot_count; ++slot_index) {
            log(name, "recorded {} records of {} to {}", slots_[slot_index].recorded_count,
                options.slot_names[slot_index].c_str(), path_.c_str());
        }
    }

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    Recorder(Recorder&&) = delete;
    Recorder& operator=(Recorder&&) = delete;

    RegisteredName name;

    void process(Connections connections) {
        std::size_t slot_index = 0;
        template for (const auto& input : connections.inputs) {
            stage(slot_index, input);
            ++slot_index;
        }
    }

private:
    struct FileCloser {
        void operator()(std::FILE* file) const noexcept { static_cast<void>(std::fclose(file)); }
    };

    // The records of one connection that wait to be written as its next chunk.
    struct Slot {
        std::vector<std::byte> staged_records;
        std::size_t staged_count = 0;
        u64 recorded_count = 0;
    };

    static constexpr std::array<RecorderSlotFormat, slot_count> slot_formats{
        RecorderSlotFormatOf<InputKinds>::value...};

    std::array<Slot, slot_count> slots_;
    std::string path_;
    std::unique_ptr<std::FILE, FileCloser> file_;

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

    // The file's first line.
    [[nodiscard]] static std::string file_description() {
        std::string description = R"({"format": "falcon_recording", "version": 1, "slots": [)";
        for (std::size_t slot_index = 0; slot_index < slot_count; ++slot_index) {
            const RecorderSlotFormat& format = slot_formats[slot_index];
            const char* const slot_name = options.slot_names[slot_index].c_str();
            if (slot_index > 0) {
                description += ", ";
            }
            if (format.is_sample_stream) {
                description +=
                    std::format(R"({{"name": "{}", "kind": "samples", "channels": {}, "rate": {}, )"
                                R"("record_bytes": {}}})",
                                slot_name, format.channels, format.rate, format.record_bytes);
            } else {
                description +=
                    std::format(R"({{"name": "{}", "kind": "events", "record_bytes": {}}})",
                                slot_name, format.record_bytes);
            }
        }
        description += "]}\n";
        return description;
    }

    template <typename StreamInfoType>
    void stage(std::size_t slot_index, const SampleInputBuffer<StreamInfoType>& input) {
        if (input.is_empty()) {
            return;
        }
        alignas(64) std::array<float, StreamInfoType::padded_channels> values{};
        for (int simd_vector_index = 0; simd_vector_index < StreamInfoType::simd_vectors;
             ++simd_vector_index) {
            float* const vector_values = &values[static_cast<std::size_t>(simd_vector_index) *
                                                 static_cast<std::size_t>(FLOATS_PER_SIMD_VECTOR)];
            // auto, not SimdVector: a type that depends on the template keeps the other
            // instruction set's branch from being compiled.
            const auto loaded_values = input.load(simd_vector_index);
            if constexpr (HAS_AVX512) {
                _mm512_store_ps(vector_values, loaded_values);
            } else {
                _mm256_store_ps(vector_values, loaded_values);
            }
        }

        std::byte* const record = next_record(slot_index);
        const u64 sample_number = input.header.sample_number;
        std::memcpy(record, &sample_number, sizeof(sample_number));
        // Only the real channels: padding channels are not part of the stream.
        std::memcpy(record + sizeof(sample_number), values.data(),
                    static_cast<std::size_t>(StreamInfoType::channels) * sizeof(float));
        finish_record(slot_index);
    }

    template <std::derived_from<StreamEvent> EventType, int Capacity>
    void stage(std::size_t slot_index, const EventInputBuffer<EventType, Capacity>& input) {
        if (input.is_empty()) {
            return;
        }
        for (const EventType& event : input) {
            std::memcpy(next_record(slot_index), &event.header, sizeof(SampleHeader));
            finish_record(slot_index);
        }
        write_chunk_or_fail(slot_index);
    }

    [[nodiscard]] std::byte* next_record(std::size_t slot_index) {
        Slot& slot = slots_[slot_index];
        return &slot.staged_records[slot.staged_count * slot_formats[slot_index].record_bytes];
    }

    void finish_record(std::size_t slot_index) {
        Slot& slot = slots_[slot_index];
        ++slot.staged_count;
        ++slot.recorded_count;
        if (slot.staged_count == slot_formats[slot_index].chunk_records) {
            write_chunk_or_fail(slot_index);
        }
    }

    // Writes a connection's waiting records as one chunk, if it has any. Returns whether the
    // write succeeded.
    [[nodiscard]] bool write_chunk(std::size_t slot_index) {
        Slot& slot = slots_[slot_index];
        if (slot.staged_count == 0) {
            return true;
        }
        const std::array<u32, 2> chunk_header{
            static_cast<u32>(slot_index),
            static_cast<u32>(slot.staged_count),
        };
        const std::size_t byte_count = slot.staged_count * slot_formats[slot_index].record_bytes;
        slot.staged_count = 0;
        return std::fwrite(chunk_header.data(), sizeof(u32), chunk_header.size(), file_.get()) ==
                   chunk_header.size() &&
               std::fwrite(slot.staged_records.data(), 1, byte_count, file_.get()) == byte_count;
    }

    void write_chunk_or_fail(std::size_t slot_index) {
        if (!write_chunk(slot_index)) {
            fail(name, "can't write to {}: {}", path_.c_str(),
                 std::system_category().message(errno).c_str());
        }
    }
};
