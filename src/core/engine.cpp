#pragma once
#include <cxxabi.h>
#include <immintrin.h>
#include <pmmintrin.h>
#include <pthread.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#include <x86intrin.h>
#include <xmmintrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <concepts>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <iterator>
#include <latch>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

// Falcon engine. Its design is described in src/core/spec.md.

using u32 = std::uint32_t;
using u64 = std::uint64_t;

// --- Instruction set ---

#ifndef __AVX2__
#error "Falcon needs AVX2: compile with -mavx2 or -march=native"
#endif

#ifndef __FMA__
#error "Falcon needs FMA: compile with -mfma or -march=native"
#endif

#ifdef __AVX512F__
inline constexpr bool HAS_AVX512 = true;
#else
inline constexpr bool HAS_AVX512 = false;
#endif

inline constexpr int FLOATS_PER_SIMD_VECTOR = HAS_AVX512 ? 16 : 8;
using SimdVector = std::conditional_t<HAS_AVX512, __m512, __m256>;

// --- String options ---

// A string a template argument can hold: std::string and pointers to string literals can't be one.
struct Str {
    static constexpr std::size_t capacity = 256;  // bytes, the terminating zero included
    std::array<char, capacity> characters{};

    constexpr Str() = default;

    // consteval, so a string longer than 255 characters is a compile error, never cut.
    consteval Str(const char* text) {
        std::size_t length = 0;
        while (text[length] != '\0') {
            if (length + 1 >= capacity) {
                throw std::length_error("a string option holds at most 255 characters");
            }
            characters[length] = text[length];
            ++length;
        }
    }

    [[nodiscard]] constexpr const char* c_str() const { return characters.data(); }
};

// --- Streams and samples ---

template <int ChannelCount, double SampleRate, typename SampleType>
struct StreamInfo {
    static_assert(ChannelCount > 0);
    static_assert(SampleRate > 0);
    static_assert(std::same_as<SampleType, float>, "only float32 samples are implemented");

    static constexpr int channels = ChannelCount;
    static constexpr double rate = SampleRate;  // Hz
    using ChannelValue = SampleType;
    // 16 floats are one AVX512 vector, so SIMD loops have no leftover channels.
    static constexpr int padded_channels = (ChannelCount + 15) / 16 * 16;
    static constexpr int simd_vectors = padded_channels / FLOATS_PER_SIMD_VECTOR;
};

// Set by the input processor. Other processors pass it on and never make one up.
struct SampleHeader {
    u64 sample_number = 0;     // counted at the input's rate, derived from the device's clock
    u64 device_timestamp = 0;  // in the device's own unit; processors never calculate with it
    u64 arrival_tsc = 0;       // CPU timestamp counter when the sample arrived
    // Samples lost upstream since the start: the input adds those the device skipped, each ring
    // those it dropped. It only grows, so any later header carries every earlier loss; a processor
    // sees one when the count differs from its previous sample's.
    u64 lost_sample_count = 0;
};

template <typename StreamInfoType>
struct SampleWithHeader {
    SampleHeader header;
    alignas(64)
        std::array<typename StreamInfoType::ChannelValue, StreamInfoType::padded_channels> values;
};

// 0 or 1 sample. `sample_` points at the buffer's own storage, or at a ring slot, so nothing is
// copied across rings. Only In, Out and rings reach inside, so a processor that holds a buffer
// directly instead of through In or Out can't use it.
template <typename StreamInfoType>
struct SampleBuffer {
    SampleBuffer() = default;
    // A copy's `sample_` would still point at the original's storage.
    SampleBuffer(const SampleBuffer&) = delete;
    SampleBuffer& operator=(const SampleBuffer&) = delete;

private:
    template <typename Kind>
    friend struct In;
    template <typename Kind>
    friend struct Out;
    template <typename RingStreamInfoType, int slot_count>
    friend struct SampleRing;

    SampleWithHeader<StreamInfoType> storage_{};
    SampleWithHeader<StreamInfoType>* sample_ = &storage_;
    int count_ = 0;
};

// --- Events ---

// Base of every event. Empty: an event from a timer has no sample to point to.
struct Event {};

// Base of every event that comes from a sample; `header` is that sample's header.
struct StreamEvent : Event {
    SampleHeader header;
};

// Up to Capacity events produced on this pass, held inside the buffer. Only In and Out reach
// inside, and it can't be copied, so a processor that holds a buffer directly instead of through In
// or Out can't use it or push into a copy.
template <std::derived_from<Event> EventType, int Capacity>
struct EventBuffer {
    EventBuffer() = default;
    EventBuffer(const EventBuffer&) = delete;
    EventBuffer& operator=(const EventBuffer&) = delete;

private:
    template <typename Kind>
    friend struct In;
    template <typename Kind>
    friend struct Out;

    std::array<EventType, Capacity> events_{};
    int count_ = 0;
};

// --- Channel masks ---

// The bits of one channel vector, as its comparison yields them: 16 on AVX512, 8 on AVX2, the
// lowest for the vector's first channel. A type of its own, so set_vector's two arguments can't be
// swapped.
struct ChannelVectorBits {
    unsigned bits = 0;
};

// One bit per channel of a stream, e.g. whether the channel crossed a threshold on this sample.
// Bit c is channel c on every instruction set. Padding channels' bits are always 0, so a padding
// channel can never seem to fire.
template <typename StreamInfoType>
class ChannelMask {
    static constexpr std::size_t word_count =
        (static_cast<std::size_t>(StreamInfoType::padded_channels) + 63) / 64;

public:
    [[nodiscard]] static constexpr ChannelMask every_channel() {
        ChannelMask mask;
        for (std::size_t channel = 0; channel < static_cast<std::size_t>(StreamInfoType::channels);
             ++channel) {
            mask.words_[channel / 64] |= u64{1} << (channel % 64);
        }
        return mask;
    }

    void set_vector(int simd_vector_index, ChannelVectorBits vector) {
        const auto first_channel = static_cast<std::size_t>(simd_vector_index) *
                                   static_cast<std::size_t>(FLOATS_PER_SIMD_VECTOR);
        const std::size_t word_index = first_channel / 64;
        const auto shift = static_cast<unsigned>(first_channel % 64);
        const u64 real_bits = static_cast<u64>(vector.bits) & real_channel_bits(first_channel);
        words_[word_index] = (words_[word_index] & ~(vector_bits << shift)) | (real_bits << shift);
    }

    [[nodiscard]] bool is_empty() const { return *this == ChannelMask{}; }

    [[nodiscard]] int count() const {
        int set_bit_count = 0;
        for (const u64 word : words_) {
            set_bit_count += std::popcount(word);
        }
        return set_bit_count;
    }

    [[nodiscard]] bool has_channel(u32 channel) const {
        if (channel >= static_cast<u32>(StreamInfoType::channels)) {
            return false;
        }
        return ((words_[channel / 64] >> (channel % 64)) & 1U) != 0;
    }

    ChannelMask& operator|=(const ChannelMask& other) {
        for (std::size_t word_index = 0; word_index < word_count; ++word_index) {
            words_[word_index] |= other.words_[word_index];
        }
        return *this;
    }

    [[nodiscard]] ChannelMask operator&(const ChannelMask& other) const {
        ChannelMask result = *this;
        for (std::size_t word_index = 0; word_index < word_count; ++word_index) {
            result.words_[word_index] &= other.words_[word_index];
        }
        return result;
    }

    [[nodiscard]] bool operator==(const ChannelMask& other) const = default;

    // Walks the channel numbers of the set bits, lowest first: `for (u32 channel : mask)`.
    class ChannelIterator {
    public:
        explicit ChannelIterator(const std::array<u64, word_count>& words)
            : words_(&words), remaining_bits_(words[0]) {
            skip_empty_words();
        }

        [[nodiscard]] u32 operator*() const {
            return static_cast<u32>((word_index_ * 64) +
                                    static_cast<std::size_t>(std::countr_zero(remaining_bits_)));
        }

        ChannelIterator& operator++() {
            remaining_bits_ &= remaining_bits_ - 1;  // clears the lowest set bit
            skip_empty_words();
            return *this;
        }

        [[nodiscard]] bool operator==(std::default_sentinel_t /*end*/) const {
            return word_index_ == word_count;
        }

    private:
        const std::array<u64, word_count>* words_;
        std::size_t word_index_ = 0;
        u64 remaining_bits_ = 0;

        void skip_empty_words() {
            while (remaining_bits_ == 0) {
                ++word_index_;
                if (word_index_ == word_count) {
                    return;
                }
                remaining_bits_ = (*words_)[word_index_];
            }
        }
    };

    [[nodiscard]] ChannelIterator begin() const { return ChannelIterator(words_); }
    [[nodiscard]] std::default_sentinel_t end() const { return {}; }

private:
    static constexpr u64 vector_bits =
        (u64{1} << static_cast<unsigned>(FLOATS_PER_SIMD_VECTOR)) - 1;

    std::array<u64, word_count> words_{};

    // The bits of a channel vector's real channels, from its first channel on.
    [[nodiscard]] static constexpr u64 real_channel_bits(std::size_t first_channel) {
        const auto channel_count = static_cast<std::size_t>(StreamInfoType::channels);
        if (first_channel >= channel_count) {
            return 0;
        }
        const std::size_t real_channels = channel_count - first_channel;
        if (real_channels >= static_cast<std::size_t>(FLOATS_PER_SIMD_VECTOR)) {
            return vector_bits;
        }
        return (u64{1} << real_channels) - 1;
    }
};

// An event that holds a mask of a stream's channels, such as Spikes or LevelCrossings.
template <typename EventType, typename StreamInfoType>
concept ChannelMaskEvent =
    std::derived_from<EventType, StreamEvent> && requires(const EventType& event) {
        { event.channels } -> std::same_as<const ChannelMask<StreamInfoType>&>;
    };

// --- State values ---

// A number, bool or enum, so a state value fits in one register.
template <typename StateType>
concept StateValueType = std::is_arithmetic_v<StateType> || std::is_enum_v<StateType>;

// The producer's StateOutput fixes initial_value, so pipeline.cpp can't declare a different one.
template <StateValueType StateType, StateType initial_value>
struct StateValue {
    StateType value = initial_value;
};

// --- In and Out ---
// process() takes one struct, Connections, declared inside its processor. Every member of it is an
// In<Kind> or an Out<Kind>, and each exposes only what its direction allows.

template <typename Kind>
struct In;
template <typename Kind>
struct Out;

template <typename StreamInfoType>
struct In<SampleBuffer<StreamInfoType>> {
    In(const SampleBuffer<StreamInfoType>& samples)
        : header(samples.sample_->header), buffer_(samples) {}

    const SampleHeader& header;

    [[nodiscard]] bool is_empty() const { return buffer_.count_ == 0; }

    [[nodiscard]] SimdVector load(int simd_vector_index) const {
        const float* address =
            &buffer_.sample_->values[static_cast<std::size_t>(simd_vector_index) *
                                     static_cast<std::size_t>(FLOATS_PER_SIMD_VECTOR)];
        // Not if constexpr: nothing here depends on StreamInfoType, so the discarded branch would
        // still be type-checked, and an __m512 doesn't convert to the AVX2 build's SimdVector.
#ifdef __AVX512F__
        return _mm512_load_ps(address);
#else
        return _mm256_load_ps(address);
#endif
    }

private:
    const SampleBuffer<StreamInfoType>& buffer_;
};

template <typename StreamInfoType>
struct Out<SampleBuffer<StreamInfoType>> {
    // Clears the buffer right before its producer runs.
    Out(SampleBuffer<StreamInfoType>& samples) : buffer_(samples) { buffer_.count_ = 0; }

    void set_header(const SampleHeader& header) {
        buffer_.sample_->header = header;
        buffer_.count_ = 1;
    }

    void store(int simd_vector_index, SimdVector values) {
        float* address = &buffer_.sample_->values[static_cast<std::size_t>(simd_vector_index) *
                                                  static_cast<std::size_t>(FLOATS_PER_SIMD_VECTOR)];
        // Not if constexpr, for the same reason as in load().
#ifdef __AVX512F__
        _mm512_store_ps(address, values);
#else
        _mm256_store_ps(address, values);
#endif
    }

private:
    SampleBuffer<StreamInfoType>& buffer_;
};

template <std::derived_from<Event> EventType, int Capacity>
struct In<EventBuffer<EventType, Capacity>> {
    In(const EventBuffer<EventType, Capacity>& events) : buffer_(events) {}

    [[nodiscard]] bool is_empty() const { return buffer_.count_ == 0; }
    [[nodiscard]] int size() const { return buffer_.count_; }
    [[nodiscard]] const EventType* begin() const { return buffer_.events_.data(); }
    [[nodiscard]] const EventType* end() const { return buffer_.events_.data() + buffer_.count_; }

private:
    const EventBuffer<EventType, Capacity>& buffer_;
};

template <std::derived_from<Event> EventType, int Capacity>
struct Out<EventBuffer<EventType, Capacity>> {
    // Clears the buffer right before its producer runs.
    Out(EventBuffer<EventType, Capacity>& events) : buffer_(events) { buffer_.count_ = 0; }

    // Neither push checks Capacity: a producer never pushes more than Capacity per pass.
    // A stream event can only be pushed with its sample's header, never without one.
    void push(const EventType& event)
        requires(!std::derived_from<EventType, StreamEvent>)
    {
        buffer_.events_[static_cast<std::size_t>(buffer_.count_++)] = event;
    }

    // The header is a parameter because designated initializers can't name a base's members.
    void push(const SampleHeader& header, const EventType& event)
        requires std::derived_from<EventType, StreamEvent>
    {
        EventType& stored = buffer_.events_[static_cast<std::size_t>(buffer_.count_++)];
        stored = event;
        stored.header = header;
    }

private:
    EventBuffer<EventType, Capacity>& buffer_;
};

// Takes a state value with any initial value: only its producer picks one.
template <StateValueType StateType>
struct In<StateType> {
    template <StateType initial_value>
    In(const StateValue<StateType, initial_value>& state) : value_(state.value) {}

    operator StateType() const { return value_; }

private:
    const StateType& value_;
};

// Never clears its state value, which keeps its value until the producer changes it.
template <StateValueType StateType, StateType initial_value>
struct Out<StateValue<StateType, initial_value>> {
    Out(StateValue<StateType, initial_value>& state) : value_(state.value) {}

    Out& operator=(StateType new_value) {
        value_ = new_value;
        return *this;
    }

private:
    StateType& value_;
};

template <typename StreamInfoType>
using SampleInputBuffer = In<SampleBuffer<StreamInfoType>>;
template <typename StreamInfoType>
using SampleOutputBuffer = Out<SampleBuffer<StreamInfoType>>;
template <std::derived_from<Event> EventType, int Capacity>
using EventInputBuffer = In<EventBuffer<EventType, Capacity>>;
template <std::derived_from<Event> EventType, int Capacity>
using EventOutputBuffer = Out<EventBuffer<EventType, Capacity>>;
template <StateValueType StateType>
using StateInput = In<StateType>;
template <StateValueType StateType, StateType initial_value>
using StateOutput = Out<StateValue<StateType, initial_value>>;

// --- Rings ---

// Realtime cores never print, so a ring only counts its dropped samples and the main thread warns.
struct RingDropCounter {
    const char* ring_name = "";
    std::atomic<u64> dropped_samples = 0;
    u64 reported_samples = 0;              // touched only by the main thread
    RingDropCounter* next_ring = nullptr;  // the list of every ring's counter
};

// Every ring adds its counter when it is built, so none can be left out of the warnings. A linked
// list through the counters, since rings are globals: adding to it can't throw, and an exception
// while globals are built can't be caught.
inline RingDropCounter* first_ring_drop_counter = nullptr;

// One writer core and one reader core. A full ring never makes the writer wait: the sample goes
// into spare storage and is dropped.
template <typename StreamInfoType, int slot_count>
struct SampleRing {
    static_assert(slot_count > 0 && std::has_single_bit(static_cast<unsigned>(slot_count)),
                  "slot_count must be a power of 2");
    static_assert(std::atomic<u64>::is_always_lock_free);

    explicit SampleRing(const char* ring_name) noexcept {
        drop_counter_.ring_name = ring_name;
        drop_counter_.next_ring = first_ring_drop_counter;
        first_ring_drop_counter = &drop_counter_;
    }
    SampleRing(const SampleRing&) = delete;
    SampleRing& operator=(const SampleRing&) = delete;

    // Writer core: the buffer to pass to the producer's output, pointing at the next free slot.
    SampleBuffer<StreamInfoType>& next_slot() {
        const u64 write_position = write_position_.load(std::memory_order_relaxed);
        const u64 read_position = read_position_.load(std::memory_order_acquire);
        if (write_position - read_position == static_cast<u64>(slot_count)) {
            writer_buffer_.sample_ = &writer_buffer_.storage_;
        } else {
            writer_buffer_.sample_ = &slots_[write_position % static_cast<u64>(slot_count)];
        }
        return writer_buffer_;
    }

    // Writer core: makes the sample written into next_slot() visible to the reader, if there is
    // one.
    void publish() {
        if (writer_buffer_.count_ == 0) {
            return;
        }
        if (writer_buffer_.sample_ == &writer_buffer_.storage_) {
            drop_counter_.dropped_samples.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Only this core writes the drop count, so the relaxed load reads its own latest value.
        writer_buffer_.sample_->header.lost_sample_count +=
            drop_counter_.dropped_samples.load(std::memory_order_relaxed);
        const u64 write_position = write_position_.load(std::memory_order_relaxed);
        write_position_.store(write_position + 1, std::memory_order_release);
    }

    // Reader core: points `buffer` at the next sample, or leaves it empty. The slot handed out on
    // the previous pass is freed only now, since processors read it until then.
    void read(SampleBuffer<StreamInfoType>& buffer) {
        const u64 read_position = read_position_.load(std::memory_order_relaxed);
        u64 next_read_position = read_position;
        if (is_holding_slot_) {
            next_read_position = read_position + 1;
            read_position_.store(next_read_position, std::memory_order_release);
            is_holding_slot_ = false;
        }
        const u64 write_position = write_position_.load(std::memory_order_acquire);
        if (write_position == next_read_position) {
            buffer.count_ = 0;
            return;
        }
        buffer.sample_ = &slots_[next_read_position % static_cast<u64>(slot_count)];
        buffer.count_ = 1;
        is_holding_slot_ = true;
    }

private:
    // Each position on its own cache line, so the two cores share only the line they must.
    alignas(64) std::atomic<u64> write_position_ = 0;
    alignas(64) std::atomic<u64> read_position_ = 0;
    alignas(64) bool is_holding_slot_ = false;    // reader core only
    SampleBuffer<StreamInfoType> writer_buffer_;  // writer core only; its storage is the spare
    RingDropCounter drop_counter_;
    std::array<SampleWithHeader<StreamInfoType>, slot_count> slots_{};
};

// --- Cores ---

// Every core's loop runs while it is set. Cleared by run_on_cores() on a stop signal, or by a core
// whose processor threw.
std::atomic<bool> is_pipeline_running = true;
// Set when a constructor threw on any core, or a core's thread couldn't be started.
std::atomic<bool> has_startup_failed = false;
// Set when a processor threw once its core's loop had started, which ended the run.
std::atomic<bool> has_run_failed = false;
std::latch* start_barrier = nullptr;  // created by run_on_cores()
thread_local bool has_arrived_at_start_barrier = false;
thread_local int core_number_of_this_thread = -1;  // set by run_on_cores()
// Set once the core's loop may start: from then on, its messages go through its message channel
// instead of straight to the logger.
thread_local bool is_in_loop_of_this_thread = false;

// --- Processor names ---

[[nodiscard]] constexpr bool is_letter_or_digit(char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9');
}

// A processor's name, as the user gave it in the UI: the same as its variable's in pipeline.cpp.
// consteval, so it is a string literal, whose text outlives the processor: the main thread may
// print a line about it after it is gone.
class ProcessorName {
public:
    consteval ProcessorName(const char* text) : text_(text) {
        if (text[0] == '\0') {
            throw std::invalid_argument("a processor's name can't be empty");
        }
        if (text[0] >= '0' && text[0] <= '9') {
            throw std::invalid_argument("a processor's name, like its variable's, can't start with "
                                        "a digit");
        }
        for (std::size_t index = 0; text[index] != '\0'; ++index) {
            if (!is_letter_or_digit(text[index]) && text[index] != '_') {
                throw std::invalid_argument("a processor's name holds only letters, digits and _, "
                                            "like its variable's");
            }
        }
    }

    [[nodiscard]] constexpr const char* c_str() const { return text_; }

private:
    const char* text_;
};

// The names of the processors that exist, touched only while processors are built or destroyed.
std::mutex registered_names_mutex;
std::vector<std::string_view> registered_names;

// A processor's `name` member. While it exists, no other processor can take the name, so every
// line names one processor. It can't be copied, since a copy would register the name again.
class RegisteredName {
public:
    explicit RegisteredName(ProcessorName name) : name_(name) {
        const std::string_view text = name.c_str();
        const std::scoped_lock lock(registered_names_mutex);
        if (std::ranges::contains(registered_names, text)) {
            throw std::runtime_error(std::format("two processors are named {}", text));
        }
        registered_names.push_back(text);
    }

    ~RegisteredName() {
        const std::scoped_lock lock(registered_names_mutex);
        std::erase(registered_names, std::string_view(name_.c_str()));
    }

    RegisteredName(const RegisteredName&) = delete;
    RegisteredName& operator=(const RegisteredName&) = delete;
    RegisteredName(RegisteredName&&) = delete;
    RegisteredName& operator=(RegisteredName&&) = delete;

    // Implicit, so a processor passes its member straight to warn(), log() or fail().
    operator ProcessorName() const { return name_; }

private:
    ProcessorName name_;
};

template <typename ProcessorType>
concept Processor = requires(const ProcessorType& processor) {
    { processor.name } -> std::same_as<const RegisteredName&>;
};

// --- Runs ---

// The graph's name, as the user gave it in the UI. It names the run's directory, so it holds only
// letters, digits, _ and -. consteval, so any other name is a compile error.
class PipelineName {
public:
    consteval PipelineName(const char* text) : text_(text) {
        if (text[0] == '\0') {
            throw std::invalid_argument("a pipeline's name can't be empty");
        }
        for (std::size_t index = 0; text[index] != '\0'; ++index) {
            if (!is_letter_or_digit(text[index]) && text[index] != '_' && text[index] != '-') {
                throw std::invalid_argument("a pipeline's name holds only letters, digits, _ and -");
            }
        }
    }

    [[nodiscard]] constexpr const char* c_str() const { return text_; }

private:
    const char* text_;
};

// Who a run's directory and files belong to. Under sudo, the engine runs as root, but they go to
// the user who ran sudo, in that user's home.
struct RunOwner {
    std::filesystem::path home;
    uid_t user_id = 0;
    gid_t group_id = 0;
    bool is_sudo = false;
};

[[nodiscard]] RunOwner find_run_owner() {
    const char* const sudo_user = std::getenv("SUDO_USER");
    const bool is_sudo = geteuid() == 0 && sudo_user != nullptr && sudo_user[0] != '\0';
    constexpr std::size_t entry_text_capacity = 16384;
    std::array<char, entry_text_capacity> entry_text{};
    passwd entry{};
    passwd* found_entry = nullptr;
    const int error_number =
        is_sudo ? getpwnam_r(sudo_user, &entry, entry_text.data(), entry_text.size(), &found_entry)
                : getpwuid_r(getuid(), &entry, entry_text.data(), entry_text.size(), &found_entry);
    if (error_number != 0 || found_entry == nullptr) {
        throw std::runtime_error(is_sudo
                                     ? std::format("can't find the user {}, who ran sudo", sudo_user)
                                     : std::string("can't find the current user"));
    }
    return {.home = entry.pw_dir,
            .user_id = entry.pw_uid,
            .group_id = entry.pw_gid,
            .is_sudo = is_sudo,
    };
}

void give_to_owner(const std::filesystem::path& path, const RunOwner& owner) {
    if (owner.is_sudo && chown(path.c_str(), owner.user_id, owner.group_id) != 0) {
        throw std::runtime_error(
            std::format("can't give {} to the user who ran sudo", path.string()));
    }
}

void make_missing_directory(const std::filesystem::path& path, const RunOwner& owner) {
    if (std::filesystem::create_directory(path)) {
        give_to_owner(path, owner);
    }
}

// ~/falcon/runs/<pipeline name>_<date>_<time>/, new for every run.
[[nodiscard]] std::filesystem::path make_run_directory(PipelineName pipeline_name,
                                                       const RunOwner& owner) {
    const std::filesystem::path falcon_directory = owner.home / "falcon";
    const std::filesystem::path runs_directory = falcon_directory / "runs";
    make_missing_directory(falcon_directory, owner);
    make_missing_directory(runs_directory, owner);

    const std::time_t now = std::time(nullptr);
    std::tm local_time{};
    if (localtime_r(&now, &local_time) == nullptr) {
        throw std::runtime_error("can't read the local time for the run's directory");
    }
    constexpr int tm_first_year = 1900;
    const std::filesystem::path run_directory_path =
        runs_directory / std::format("{}_{:04}-{:02}-{:02}_{:02}-{:02}-{:02}",
                                     pipeline_name.c_str(), local_time.tm_year + tm_first_year,
                                     local_time.tm_mon + 1, local_time.tm_mday, local_time.tm_hour,
                                     local_time.tm_min, local_time.tm_sec);
    // A run never writes into an earlier run's directory.
    if (!std::filesystem::create_directory(run_directory_path)) {
        throw std::runtime_error(
            std::format("the run's directory {} exists already", run_directory_path.string()));
    }
    give_to_owner(run_directory_path, owner);
    return run_directory_path;
}

// Set by run_on_cores() before any core's thread starts, so every constructor and destructor may
// use it.
std::filesystem::path run_directory_path;

// A file option is a name inside the run's directory, so it can't lead out of it.
[[nodiscard]] constexpr bool is_file_name(const Str& text) {
    const std::string_view name = text.c_str();
    return !name.empty() && name != "." && name != ".." && !name.contains('/');
}

// Where a processor writes its files; a file option is a name inside it.
[[nodiscard]] const std::filesystem::path& run_directory() {
    if (run_directory_path.empty()) {
        throw std::logic_error("run_directory() is valid only once run_on_cores() has made it");
    }
    return run_directory_path;
}

// Under sudo, processors make their files as root, so these go to the owner once the run ends.
void give_run_files_to_owner(const std::filesystem::path& directory,
                             const RunOwner& owner) noexcept {
    if (!owner.is_sudo) {
        return;
    }
    bool has_failed = false;
    try {
        std::error_code listing_error;
        for (std::filesystem::recursive_directory_iterator entry(directory, listing_error);
             !listing_error && entry != std::filesystem::recursive_directory_iterator();
             entry.increment(listing_error)) {
            if (chown(entry->path().c_str(), owner.user_id, owner.group_id) != 0) {
                has_failed = true;
            }
        }
        has_failed = has_failed || static_cast<bool>(listing_error);
    } catch (...) {
        has_failed = true;
    }
    if (has_failed) {
        std::fputs("falcon: can't give every file of the run to the user who ran sudo\n", stderr);
    }
}

// --- Clocks ---

// The TSC and CLOCK_MONOTONIC_RAW at the start of the run. The TSC ticks at a constant rate,
// measured against that clock over the run so far.
u64 run_start_tsc = 0;
double run_start_seconds = 0;
// Measured before any core's thread starts, so a constructor can turn a duration into TSC ticks
// and its loop then times it without a system call.
double tsc_ticks_per_second = 0;
// The throttle lets one record of a kind into the ring per interval.
u64 message_interval_ticks = 0;
inline constexpr double MESSAGE_INTERVAL_SECONDS = 0.1;  // how often the main thread reads them

[[nodiscard]] timespec read_clock(clockid_t clock) noexcept {
    timespec time{};
    clock_gettime(clock, &time);
    return time;
}

[[nodiscard]] double seconds_of(const timespec& time) noexcept {
    constexpr double seconds_per_nanosecond = 1e-9;
    return static_cast<double>(time.tv_sec) +
           (static_cast<double>(time.tv_nsec) * seconds_per_nanosecond);
}

struct ClockReading {
    u64 tsc = 0;
    double monotonic_seconds = 0;
    timespec wall_clock{};
};

[[nodiscard]] ClockReading read_clocks() noexcept {
    return {.tsc = __rdtsc(),
            .monotonic_seconds = seconds_of(read_clock(CLOCK_MONOTONIC_RAW)),
            .wall_clock = read_clock(CLOCK_REALTIME),
    };
}

void start_run_clock() {
    run_start_tsc = __rdtsc();
    run_start_seconds = seconds_of(read_clock(CLOCK_MONOTONIC_RAW));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const ClockReading now = read_clocks();
    tsc_ticks_per_second =
        static_cast<double>(now.tsc - run_start_tsc) / (now.monotonic_seconds - run_start_seconds);
    message_interval_ticks = static_cast<u64>(tsc_ticks_per_second * MESSAGE_INTERVAL_SECONDS);
}

// The wall-clock time of a TSC reading taken shortly before `now`.
[[nodiscard]] timespec wall_clock_of(u64 tsc, const ClockReading& now) noexcept {
    const double elapsed_seconds = now.monotonic_seconds - run_start_seconds;
    if (tsc > now.tsc || now.tsc == run_start_tsc || elapsed_seconds <= 0) {
        return now.wall_clock;
    }
    const double seconds_before = static_cast<double>(now.tsc - tsc) * elapsed_seconds /
                                  static_cast<double>(now.tsc - run_start_tsc);
    constexpr std::int64_t nanoseconds_per_second = 1'000'000'000;
    constexpr double nanoseconds_per_second_as_double = 1e9;
    const std::int64_t nanoseconds =
        (static_cast<std::int64_t>(now.wall_clock.tv_sec) * nanoseconds_per_second) +
        now.wall_clock.tv_nsec -
        static_cast<std::int64_t>(seconds_before * nanoseconds_per_second_as_double);
    return {.tv_sec = static_cast<std::time_t>(nanoseconds / nanoseconds_per_second),
            .tv_nsec = static_cast<long>(nanoseconds % nanoseconds_per_second),
    };
}

// --- Logger ---

enum class MessageType : std::uint8_t { Log, Warning, Error };

[[nodiscard]] constexpr std::string_view word_of(MessageType type) {
    switch (type) {
        case MessageType::Log:
            return "log";
        case MessageType::Warning:
            return "warning";
        case MessageType::Error:
            return "error";
    }
    return "message";
}

// Every line of a run, from any thread, goes through here, so each reaches the console and the log
// file whole and in one format.
class Logger {
public:
    void open(const std::filesystem::path& path) {
        const std::scoped_lock lock(mutex_);
        file_ = std::fopen(path.c_str(), "w");
        if (file_ == nullptr) {
            throw std::runtime_error(std::format("can't open the log file {}", path.string()));
        }
    }

    // `count` is how often it happened since its previous line: the throttle shows the rest only
    // as that count.
    void write(MessageType type, const char* source_name, std::string_view text, u64 count,
               const timespec& wall_clock) noexcept {
        try {
            std::tm local_time{};
            const std::time_t seconds = wall_clock.tv_sec;
            localtime_r(&seconds, &local_time);
            constexpr int tm_first_year = 1900;
            constexpr long nanoseconds_per_microsecond = 1000;
            std::string line = std::format(
                "{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:06} {} {}: {}",
                local_time.tm_year + tm_first_year, local_time.tm_mon + 1, local_time.tm_mday,
                local_time.tm_hour, local_time.tm_min, local_time.tm_sec,
                wall_clock.tv_nsec / nanoseconds_per_microsecond, word_of(type), source_name, text);
            if (count > 1) {
                line += std::format(" (×{})", count);
            }
            line += '\n';
            const std::scoped_lock lock(mutex_);
            std::fwrite(line.data(), 1, line.size(), stderr);
            if (file_ != nullptr) {
                std::fwrite(line.data(), 1, line.size(), file_);
            }
        } catch (...) {
            std::fputs("falcon: a line couldn't be logged\n", stderr);
        }
    }

    void flush() noexcept {
        try {
            const std::scoped_lock lock(mutex_);
            if (file_ != nullptr) {
                std::fflush(file_);
            }
        } catch (...) {
            std::fputs("falcon: the log file couldn't be flushed\n", stderr);
        }
    }

    void close() noexcept {
        try {
            const std::scoped_lock lock(mutex_);
            if (file_ != nullptr) {
                std::fclose(file_);
                file_ = nullptr;
            }
        } catch (...) {
            std::fputs("falcon: the log file couldn't be closed\n", stderr);
        }
    }

private:
    std::mutex mutex_;
    std::FILE* file_ = nullptr;
};

inline Logger logger;

// A line of the engine's own, written at once.
template <typename... Arguments>
void log_engine(MessageType type, std::format_string<Arguments...> format,
                Arguments&&... arguments) noexcept {
    try {
        logger.write(type, "engine", std::format(format, std::forward<Arguments>(arguments)...), 1,
                     read_clock(CLOCK_REALTIME));
    } catch (...) {
        std::fputs("falcon: a line couldn't be logged\n", stderr);
    }
}

// Main thread only.
void report_ring_drops() noexcept {
    for (RingDropCounter* drop_counter = first_ring_drop_counter; drop_counter != nullptr;
         drop_counter = drop_counter->next_ring) {
        const u64 dropped_samples = drop_counter->dropped_samples.load(std::memory_order_relaxed);
        if (dropped_samples > drop_counter->reported_samples) {
            try {
                logger.write(MessageType::Warning, drop_counter->ring_name,
                             std::format("the ring is full: dropped {} samples ({} in total)",
                                         dropped_samples - drop_counter->reported_samples,
                                         dropped_samples),
                             1, read_clock(CLOCK_REALTIME));
            } catch (...) {
                std::fputs("falcon: a line couldn't be logged\n", stderr);
            }
            drop_counter->reported_samples = dropped_samples;
        }
    }
}

// --- Messages ---

template <typename Type>
inline constexpr bool IS_CHANNEL_MASK = false;
template <typename StreamInfoType>
inline constexpr bool IS_CHANNEL_MASK<ChannelMask<StreamInfoType>> = true;

// Lists a mask's channels, e.g. "3, 17, 40".
template <typename StreamInfoType>
struct std::formatter<ChannelMask<StreamInfoType>> {
    static constexpr std::format_parse_context::iterator parse(
        std::format_parse_context& context) {
        return context.begin();
    }

    static std::format_context::iterator format(const ChannelMask<StreamInfoType>& mask,
                                                std::format_context& context) {
        std::format_context::iterator output = context.out();
        bool is_first = true;
        for (const u32 channel : mask) {
            output = is_first ? std::format_to(output, "{}", channel)
                              : std::format_to(output, ", {}", channel);
            is_first = false;
        }
        if (is_first) {
            output = std::format_to(output, "none");
        }
        return output;
    }
};

template <typename Argument>
inline constexpr bool IS_MESSAGE_TEXT =
    std::same_as<Argument, std::string_view> || std::same_as<Argument, const char*>;

// In the loop, a message's arguments are copied into a record and formatted later by the main
// thread, by which time a pointer could dangle. So an argument is a plain value, or text, whose
// characters are copied.
template <typename Argument>
concept MessageArgument =
    IS_MESSAGE_TEXT<Argument> ||
    (std::is_trivially_copyable_v<Argument> &&
     (std::is_arithmetic_v<Argument> || std::is_enum_v<Argument> || IS_CHANNEL_MASK<Argument>));

// What std::format sees of an argument: an enum's number, text as a string_view.
template <MessageArgument Argument>
[[nodiscard]] constexpr auto formatted(const Argument& argument) {
    if constexpr (std::is_enum_v<Argument>) {
        return std::to_underlying(argument);
    } else if constexpr (std::same_as<Argument, const char*>) {
        return argument == nullptr ? std::string_view("(null)") : std::string_view(argument);
    } else {
        return argument;
    }
}

template <MessageArgument Argument>
using FormattedAs = decltype(formatted(std::declval<const Argument&>()));

// What the main thread rebuilds of an argument from a record: it owns its copy of text.
template <MessageArgument Argument>
using StoredAs = std::conditional_t<IS_MESSAGE_TEXT<Argument>, std::string, Argument>;

template <typename Value>
[[nodiscard]] auto formatted_value(const Value& value) {
    if constexpr (std::same_as<Value, std::string>) {
        return std::string_view(value);
    } else {
        return formatted(value);
    }
}

template <typename... Values>
[[nodiscard]] std::string format_values(std::string_view format, const Values&... values) {
    auto formatted_values = std::make_tuple(formatted_value(values)...);
    return std::apply(
        [format](auto&... formatted_values_of_tuple) {
            return std::vformat(format, std::make_format_args(formatted_values_of_tuple...));
        },
        formatted_values);
}

inline constexpr std::size_t MESSAGE_ARGUMENTS_CAPACITY = 256;  // bytes
using MessageArgumentBytes = std::array<std::byte, MESSAGE_ARGUMENTS_CAPACITY>;
using MessageTextLength = u32;

// Text takes its length here; its characters share what the other arguments leave.
template <MessageArgument Argument>
inline constexpr std::size_t MESSAGE_ARGUMENT_FIXED_SIZE =
    IS_MESSAGE_TEXT<Argument> ? sizeof(MessageTextLength) : sizeof(Argument);

template <MessageArgument... Arguments>
inline constexpr std::size_t MESSAGE_ARGUMENTS_FIXED_SIZE =
    (std::size_t{0} + ... + MESSAGE_ARGUMENT_FIXED_SIZE<Arguments>);

struct MessageArgumentCursor {
    std::size_t offset = 0;
    std::size_t text_room = 0;  // bytes the text arguments still may take
};

// Text that doesn't fit is cut and ends in "...".
template <MessageArgument Argument>
void store_message_argument(MessageArgumentBytes& bytes, MessageArgumentCursor& cursor,
                            const Argument& argument) noexcept {
    if constexpr (IS_MESSAGE_TEXT<Argument>) {
        const std::string_view text = formatted(argument);
        const std::size_t kept_size = std::min(text.size(), cursor.text_room);
        const auto length = static_cast<MessageTextLength>(kept_size);
        std::memcpy(bytes.data() + cursor.offset, &length, sizeof(length));
        cursor.offset += sizeof(length);
        if (kept_size > 0) {
            std::memcpy(bytes.data() + cursor.offset, text.data(), kept_size);
        }
        constexpr std::string_view cut_mark = "...";
        if (kept_size < text.size() && kept_size >= cut_mark.size()) {
            std::memcpy(bytes.data() + cursor.offset + kept_size - cut_mark.size(),
                        cut_mark.data(), cut_mark.size());
        }
        cursor.offset += kept_size;
        cursor.text_room -= kept_size;
    } else {
        std::memcpy(bytes.data() + cursor.offset, &argument, sizeof(Argument));
        cursor.offset += sizeof(Argument);
    }
}

template <MessageArgument... Arguments>
void store_message_arguments(MessageArgumentBytes& bytes, const Arguments&... arguments) noexcept {
    MessageArgumentCursor cursor{
        .offset = 0,
        .text_room = MESSAGE_ARGUMENTS_CAPACITY - MESSAGE_ARGUMENTS_FIXED_SIZE<Arguments...>,
    };
    (store_message_argument(bytes, cursor, arguments), ...);
}

template <MessageArgument Argument>
void load_message_argument(const MessageArgumentBytes& bytes, std::size_t& offset,
                           StoredAs<Argument>& value) {
    if constexpr (IS_MESSAGE_TEXT<Argument>) {
        MessageTextLength length = 0;
        std::memcpy(&length, bytes.data() + offset, sizeof(length));
        offset += sizeof(length);
        value.assign(reinterpret_cast<const char*>(bytes.data() + offset), length);
        offset += length;
    } else {
        std::memcpy(&value, bytes.data() + offset, sizeof(Argument));
        offset += sizeof(Argument);
    }
}

// Formats what store_message_arguments() copied into a record.
template <MessageArgument... Arguments>
[[nodiscard]] std::string format_stored_message(std::string_view format,
                                                const MessageArgumentBytes& bytes) {
    std::tuple<StoredAs<Arguments>...> values{};
    [[maybe_unused]] std::size_t offset = 0;
    // A default capture, since a message without arguments uses neither.
    std::apply(
        [&](StoredAs<Arguments>&... value) {
            (load_message_argument<Arguments>(bytes, offset, value), ...);
        },
        values);
    return std::apply(
        [format](const StoredAs<Arguments>&... value) { return format_values(format, value...); },
        values);
}

using MessageFormatter = std::string (*)(std::string_view format,
                                         const MessageArgumentBytes& bytes);

// One kind of message on one core: a processor's name with a format string. It counts every
// occurrence, including those the throttle keeps out of the ring.
struct MessageKind {
    const char* processor_name = nullptr;
    std::string_view format;
    MessageType type = MessageType::Log;
    bool has_sent_record = false;  // core only
    u64 last_record_tsc = 0;       // core only
    std::atomic<u64> occurrence_count = 0;
};

struct MessageRecord {
    const MessageKind* kind = nullptr;
    MessageFormatter formatter = nullptr;
    u64 tsc = 0;
    MessageArgumentBytes arguments{};
};

// One per core: the ring through which its loop's messages reach the main thread, and its kinds.
// The core writes it, the main thread reads it.
struct MessageChannel {
    static constexpr std::size_t record_count = 64;
    static constexpr std::size_t kind_capacity = 64;

    // Each position on its own cache line, so the two threads share only the line they must.
    alignas(64) std::atomic<u64> write_position = 0;
    alignas(64) std::atomic<u64> read_position = 0;
    alignas(64) std::atomic<std::size_t> kind_count = 0;
    std::atomic<u64> dropped_record_count = 0;      // the ring was full
    std::atomic<u64> dropped_occurrence_count = 0;  // the core had more kinds than it holds
    std::array<MessageRecord, record_count> records{};
    std::array<MessageKind, kind_capacity> kinds{};
};

thread_local MessageChannel* message_channel_of_this_thread = nullptr;  // set by run_on_cores()

// nullptr if the core already has as many kinds as it holds.
[[nodiscard]] MessageKind* find_message_kind(MessageChannel& channel, MessageType type,
                                             const char* processor_name,
                                             std::string_view format) noexcept {
    // Only this core adds kinds, so it reads its own latest count.
    const std::size_t kind_count = channel.kind_count.load(std::memory_order_relaxed);
    for (std::size_t kind_index = 0; kind_index < kind_count; ++kind_index) {
        MessageKind& kind = channel.kinds[kind_index];
        if (kind.processor_name == processor_name && kind.format.data() == format.data() &&
            kind.type == type) {
            return &kind;
        }
    }
    if (kind_count == MessageChannel::kind_capacity) {
        return nullptr;
    }
    MessageKind& kind = channel.kinds[kind_count];
    kind.processor_name = processor_name;
    kind.format = format;
    kind.type = type;
    channel.kind_count.store(kind_count + 1, std::memory_order_release);
    return &kind;
}

// Core only. A full ring drops the record and counts it.
template <MessageArgument... Arguments>
void publish_message_record(MessageChannel& channel, MessageKind& kind, u64 tsc,
                            const Arguments&... arguments) noexcept {
    const u64 write_position = channel.write_position.load(std::memory_order_relaxed);
    if (write_position - channel.read_position.load(std::memory_order_acquire) ==
        MessageChannel::record_count) {
        channel.dropped_record_count.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    kind.has_sent_record = true;
    kind.last_record_tsc = tsc;
    MessageRecord& record = channel.records[write_position % MessageChannel::record_count];
    record.kind = &kind;
    record.formatter = &format_stored_message<Arguments...>;
    record.tsc = tsc;
    store_message_arguments(record.arguments, arguments...);
    channel.write_position.store(write_position + 1, std::memory_order_release);
}

template <MessageArgument... Arguments>
void send_through_channel(MessageChannel& channel, MessageType type, ProcessorName name,
                          std::string_view format, const Arguments&... arguments) noexcept {
    MessageKind* const kind = find_message_kind(channel, type, name.c_str(), format);
    if (kind == nullptr) {
        channel.dropped_occurrence_count.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const u64 now = __rdtsc();
    const bool is_throttled =
        kind->has_sent_record && now - kind->last_record_tsc < message_interval_ticks;
    if (!is_throttled) {
        publish_message_record(channel, *kind, now, arguments...);
    }
    // Counted only once its record is in the ring: the main thread reads the counts before the
    // records, so it has read the record of every occurrence it counts.
    // Only this core writes the count, so a load and a store need no locked instruction.
    kind->occurrence_count.store(kind->occurrence_count.load(std::memory_order_relaxed) + 1,
                                 std::memory_order_release);
}

template <MessageArgument... Arguments>
void send_message(MessageType type, ProcessorName name, std::string_view format,
                  const Arguments&... arguments) noexcept {
    static_assert(MESSAGE_ARGUMENTS_FIXED_SIZE<Arguments...> <= MESSAGE_ARGUMENTS_CAPACITY,
                  "a message's arguments take at most 256 bytes");
    MessageChannel* const channel = message_channel_of_this_thread;
    if (channel != nullptr && is_in_loop_of_this_thread &&
        is_pipeline_running.load(std::memory_order_relaxed)) {
        send_through_channel(*channel, type, name, format, arguments...);
        return;
    }
    // Outside a loop (setup, teardown, or a thread that runs none), waiting is allowed.
    try {
        logger.write(type, name.c_str(), format_values(format, arguments...), 1,
                     read_clock(CLOCK_REALTIME));
    } catch (...) {
        std::fputs("falcon: a line couldn't be logged\n", stderr);
    }
}

// Cold and never inlined, so a call in process() costs it only a branch and doesn't keep clang
// from inlining it.
template <MessageArgument... Arguments>
[[gnu::cold, gnu::noinline]] void warn(ProcessorName name,
                                       std::format_string<FormattedAs<Arguments>...> format,
                                       Arguments... arguments) noexcept {
    send_message(MessageType::Warning, name, format.get(), arguments...);
}

template <MessageArgument... Arguments>
[[gnu::cold, gnu::noinline]] void log(ProcessorName name,
                                      std::format_string<FormattedAs<Arguments>...> format,
                                      Arguments... arguments) noexcept {
    send_message(MessageType::Log, name, format.get(), arguments...);
}

// Ends the run: the core's thread catches the exception, every loop stops and every destructor
// runs. Cold and never inlined, as warn() is.
template <MessageArgument... Arguments>
[[noreturn, gnu::cold, gnu::noinline]] void fail(
    ProcessorName name, std::format_string<FormattedAs<Arguments>...> format,
    Arguments... arguments) {
    throw std::runtime_error(
        std::format("{}: {}", name.c_str(), format_values(format.get(), arguments...)));
}

// The main thread's side of a core's message channel: what it has reported so far.
struct ReportedKind {
    u64 reported_count = 0;
    std::string text;  // its latest record's, shown again when only its count changes
    // `text` is a record's that no line has shown yet. A later record doesn't replace it, so the
    // kind's next line shows its first occurrence since its previous line.
    bool has_unreported_record = false;
    u64 unreported_record_tsc = 0;
};

struct MessageChannelReader {
    MessageChannel* channel = nullptr;
    int core_number = 0;
    std::array<ReportedKind, MessageChannel::kind_capacity> kinds{};
    u64 reported_dropped_record_count = 0;
    u64 reported_dropped_occurrence_count = 0;
};

// A core's occurrence counts at one moment.
struct PolledCounts {
    std::size_t kind_count = 0;
    std::array<u64, MessageChannel::kind_capacity> occurrence_counts{};
};

[[nodiscard]] PolledCounts read_message_counts(const MessageChannel& channel) noexcept {
    PolledCounts counts;
    counts.kind_count = channel.kind_count.load(std::memory_order_acquire);
    for (std::size_t kind_index = 0; kind_index < counts.kind_count; ++kind_index) {
        counts.occurrence_counts[kind_index] =
            channel.kinds[kind_index].occurrence_count.load(std::memory_order_acquire);
    }
    return counts;
}

void read_message_records(MessageChannelReader& reader) {
    MessageChannel& channel = *reader.channel;
    const u64 write_position = channel.write_position.load(std::memory_order_acquire);
    for (u64 read_position = channel.read_position.load(std::memory_order_relaxed);
         read_position != write_position; ++read_position) {
        const MessageRecord& record =
            channel.records[read_position % MessageChannel::record_count];
        const auto kind_index = static_cast<std::size_t>(record.kind - channel.kinds.data());
        ReportedKind& reported_kind = reader.kinds[kind_index];
        if (!reported_kind.has_unreported_record) {
            try {
                reported_kind.text = record.formatter(record.kind->format, record.arguments);
            } catch (...) {
                reported_kind.text = "(its text couldn't be formatted)";
            }
            reported_kind.has_unreported_record = true;
            reported_kind.unreported_record_tsc = record.tsc;
        }
        channel.read_position.store(read_position + 1, std::memory_order_release);
    }
}

void report_message_counts(MessageChannelReader& reader, const PolledCounts& counts,
                           const ClockReading& now) {
    const MessageChannel& channel = *reader.channel;
    for (std::size_t kind_index = 0; kind_index < counts.kind_count; ++kind_index) {
        const MessageKind& kind = channel.kinds[kind_index];
        ReportedKind& reported_kind = reader.kinds[kind_index];
        const u64 occurrence_count = counts.occurrence_counts[kind_index];
        if (occurrence_count == reported_kind.reported_count) {
            continue;
        }
        const timespec wall_clock = reported_kind.has_unreported_record
                                        ? wall_clock_of(reported_kind.unreported_record_tsc, now)
                                        : now.wall_clock;
        const std::string_view text =
            reported_kind.text.empty()
                ? std::string_view("(its text was lost: the core's message ring was full)")
                : std::string_view(reported_kind.text);
        logger.write(kind.type, kind.processor_name, text,
                     occurrence_count - reported_kind.reported_count, wall_clock);
        reported_kind.reported_count = occurrence_count;
        reported_kind.has_unreported_record = false;
    }
}

void report_message_drops(MessageChannelReader& reader) {
    const MessageChannel& channel = *reader.channel;
    const u64 dropped_record_count = channel.dropped_record_count.load(std::memory_order_relaxed);
    if (dropped_record_count > reader.reported_dropped_record_count) {
        log_engine(MessageType::Warning,
                   "core {}'s message ring was full: {} messages from its loop were dropped",
                   reader.core_number, dropped_record_count - reader.reported_dropped_record_count);
        reader.reported_dropped_record_count = dropped_record_count;
    }
    const u64 dropped_occurrence_count =
        channel.dropped_occurrence_count.load(std::memory_order_relaxed);
    if (dropped_occurrence_count > reader.reported_dropped_occurrence_count) {
        log_engine(MessageType::Warning,
                   "core {} raised more than {} kinds of messages: {} occurrences weren't shown",
                   reader.core_number, MessageChannel::kind_capacity,
                   dropped_occurrence_count - reader.reported_dropped_occurrence_count);
        reader.reported_dropped_occurrence_count = dropped_occurrence_count;
    }
}

// Main thread only: at every poll, and once more after the loops end.
void report_messages(MessageChannelReader& reader) noexcept {
    try {
        // The counts before the records: a core counts an occurrence only once its record is in
        // the ring, so the record of every occurrence counted here is read below.
        const PolledCounts counts = read_message_counts(*reader.channel);
        read_message_records(reader);
        // Read after the records, so no record is newer than it.
        const ClockReading now = read_clocks();
        report_message_counts(reader, counts, now);
        report_message_drops(reader);
    } catch (...) {
        std::fputs("falcon: a core's messages couldn't be reported\n", stderr);
    }
}

void report_everything(std::span<MessageChannelReader> readers) noexcept {
    for (MessageChannelReader& reader : readers) {
        report_messages(reader);
    }
    report_ring_drops();
    logger.flush();
}

// Every core calls this once its processors are built. Returns false if startup failed on any
// core, so the core returns without running its loop. The thread pins itself to its core only
// now: a thread that a library starts in a constructor (e.g. to receive packets) inherits its
// creator's CPUs, and on the realtime core it would compete with a loop that never yields.
bool wait_at_start_barrier() {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core_number_of_this_thread, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
        throw std::runtime_error(std::format("can't pin to CPU {}", core_number_of_this_thread));
    }
    has_arrived_at_start_barrier = true;
    start_barrier->arrive_and_wait();
    is_in_loop_of_this_thread = !has_startup_failed;
    return !has_startup_failed;
}

struct CoreThread {
    int core_number;        // CPU core the thread pins itself to in wait_at_start_barrier()
    void (*entry_point)();  // declares the core's processors and buffers, calls
                            // wait_at_start_barrier(), then runs its loop
};

// The stop signals: SIGINT (Ctrl+C) and SIGTERM.
sigset_t stop_signals() {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    return signals;
}

// Waits for a stop signal while the cores run, then clears `is_pipeline_running`, reporting the
// cores' messages and the rings' drops at every wake-up. The timeout ends the wait when
// `is_pipeline_running` was cleared without a signal (a processor threw).
void wait_for_stop_signal(const sigset_t& signals, std::span<MessageChannelReader> readers) {
    while (is_pipeline_running) {
        const timespec timeout{.tv_sec = 0, .tv_nsec = 100'000'000};  // MESSAGE_INTERVAL_SECONDS
        const int signal_number = sigtimedwait(&signals, nullptr, &timeout);
        if (signal_number > 0) {
            log_engine(MessageType::Log, "{} received, stopping",
                       signal_number == SIGINT ? "Ctrl+C" : "SIGTERM");
            is_pipeline_running = false;
        }
        report_everything(readers);
    }
}

// Makes the run's directory, opens its log file, and makes one message channel per core.
// Returns false if the run can't start.
[[nodiscard]] bool start_run(PipelineName pipeline_name, std::size_t core_count,
                             RunOwner& owner,
                             std::vector<std::unique_ptr<MessageChannel>>& channels) noexcept {
    try {
        start_run_clock();
        owner = find_run_owner();
        run_directory_path = make_run_directory(pipeline_name, owner);
        const std::filesystem::path log_path = run_directory_path / "falcon.log";
        logger.open(log_path);
        give_to_owner(log_path, owner);
        for (std::size_t core_index = 0; core_index < core_count; ++core_index) {
            channels.push_back(std::make_unique<MessageChannel>());
        }
        return true;
    } catch (const std::exception& error) {
        std::fputs("falcon: the run can't start: ", stderr);
        std::fputs(error.what(), stderr);
        std::fputs("\n", stderr);
        return false;
    }
}

// A processor threw: a constructor, so the graph doesn't start, or in the loop, which ends the run.
void handle_core_failure(int core_number, std::string_view description) noexcept {
    log_engine(MessageType::Error, "core {}: {}", core_number, description);
    if (has_arrived_at_start_barrier) {
        has_run_failed = true;
    } else {
        has_startup_failed = true;
    }
    is_pipeline_running = false;
    if (!has_arrived_at_start_barrier) {
        start_barrier->count_down();  // release the cores waiting at the barrier
    }
}

// An exception that isn't a std::exception has no message, so its line names its type.
void handle_core_failure_without_message(int core_number, const std::type_info& type) noexcept {
    int status = 0;
    char* const readable_name = abi::__cxa_demangle(type.name(), nullptr, nullptr, &status);
    try {
        handle_core_failure(
            core_number,
            std::format("threw an exception of type {}, which isn't a std::exception, so it has "
                        "no message",
                        readable_name != nullptr ? readable_name : type.name()));
    } catch (...) {
        handle_core_failure(core_number, "threw an exception that isn't a std::exception");
    }
    std::free(readable_name);
}

// Runs one pinned thread per core until stopped. Returns 1 if the graph didn't start or a
// processor ended the run, 0 otherwise. No exception leaves run_on_cores().
int run_on_cores(PipelineName pipeline_name, std::initializer_list<CoreThread> core_threads) {
    // Blocked before any core's thread starts, so every thread inherits the block and no stop
    // signal ever interrupts a core's loop. Only wait_for_stop_signal() takes them.
    const sigset_t signals = stop_signals();
    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        std::fputs("falcon: can't block the stop signals\n", stderr);
        return 1;
    }

    RunOwner owner;
    std::vector<std::unique_ptr<MessageChannel>> channels;
    if (!start_run(pipeline_name, core_threads.size(), owner, channels)) {
        return 1;
    }
    log_engine(MessageType::Log, "{} starts, writing into {}", pipeline_name.c_str(),
               run_directory_path.c_str());

    std::vector<MessageChannelReader> readers;
    std::latch latch(static_cast<std::ptrdiff_t>(core_threads.size()));
    start_barrier = &latch;

    std::vector<std::thread> threads;
    try {
        readers.reserve(core_threads.size());
        threads.reserve(core_threads.size());
        for (std::size_t core_index = 0; const CoreThread core_thread : core_threads) {
            MessageChannel* const channel = channels[core_index].get();
            readers.push_back({.channel = channel, .core_number = core_thread.core_number});
            threads.emplace_back([core_thread, channel] {
                try {
                    core_number_of_this_thread = core_thread.core_number;
                    message_channel_of_this_thread = channel;
                    // Denormals, e.g. a filter's state on a flat channel, are far slower.
                    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
                    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
                    core_thread.entry_point();
                } catch (const std::exception& error) {
                    handle_core_failure(core_thread.core_number, error.what());
                } catch (...) {
                    const std::type_info* const type = abi::__cxa_current_exception_type();
                    if (type == nullptr) {
                        // Not a C++ exception, e.g. the unwinding of a cancelled thread, which
                        // must go on.
                        throw;
                    }
                    handle_core_failure_without_message(core_thread.core_number, *type);
                }
            });
            ++core_index;
        }
    } catch (const std::exception& error) {
        // A core's thread couldn't be started. Count down once for every core that never started,
        // so the started ones pass the start barrier, see has_startup_failed and return.
        log_engine(MessageType::Error, "can't start a core's thread: {}", error.what());
        has_startup_failed = true;
        is_pipeline_running = false;
        for (std::size_t missing = threads.size(); missing < core_threads.size(); ++missing) {
            latch.count_down();
        }
    }

    wait_for_stop_signal(signals, readers);
    for (std::thread& thread : threads) {
        thread.join();
    }
    // What the loops raised after the last poll.
    report_everything(readers);
    logger.close();
    give_run_files_to_owner(run_directory_path, owner);
    return (has_startup_failed || has_run_failed) ? 1 : 0;
}
