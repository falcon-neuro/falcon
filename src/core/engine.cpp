#pragma once
#include <cxxabi.h>
#include <immintrin.h>
#include <pmmintrin.h>
#include <pthread.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#include <xmmintrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
#include <type_traits>
#include <typeinfo>
#include <vector>
#include "logger.cpp"

// Falcon engine. Its design is described in src/core/spec.md.

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

// A message may carry a channel mask.
template <typename StreamInfoType>
inline constexpr bool IS_CHANNEL_MASK<ChannelMask<StreamInfoType>> = true;

// Lists a mask's channels, e.g. "3, 17, 40".
template <typename StreamInfoType>
struct std::formatter<ChannelMask<StreamInfoType>> {
    static constexpr std::format_parse_context::iterator parse(std::format_parse_context& context) {
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

// Set when a constructor threw on any core, or a core's thread couldn't be started.
std::atomic<bool> has_startup_failed = false;
// Set when a processor threw once its core's loop had started, which ended the run.
std::atomic<bool> has_run_failed = false;
std::latch* start_barrier = nullptr;  // created by run_on_cores()
thread_local bool has_arrived_at_start_barrier = false;
thread_local int core_number_of_this_thread = -1;  // set by run_on_cores()

// --- Processor names ---

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
                throw std::invalid_argument(
                    "a pipeline's name holds only letters, digits, _ and -");
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
        throw std::runtime_error(
            is_sudo ? std::format("can't find the user {}, who ran sudo", sudo_user)
                    : std::string("can't find the current user"));
    }
    return {
        .home = entry.pw_dir,
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

// --- Reporting ---

// Main thread only.
void report_ring_drops() noexcept {
    for (RingDropCounter* drop_counter = first_ring_drop_counter; drop_counter != nullptr;
         drop_counter = drop_counter->next_ring) {
        const u64 dropped_samples = drop_counter->dropped_samples.load(std::memory_order_relaxed);
        if (dropped_samples > drop_counter->reported_samples) {
            log_as(MessageType::Warning, drop_counter->ring_name,
                   "the ring is full: dropped {} samples ({} in total)",
                   dropped_samples - drop_counter->reported_samples, dropped_samples);
            drop_counter->reported_samples = dropped_samples;
        }
    }
}

void report_everything(std::span<MessageChannelReader> readers) noexcept {
    report_messages(readers);
    report_ring_drops();
    logger.flush();
}

// --- Start and stop ---

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
[[nodiscard]] bool start_run(PipelineName pipeline_name, std::size_t core_count, RunOwner& owner,
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
void end_run_after_core_failure() noexcept {
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

// For an exception that fail() didn't throw, so no line reports it yet.
void handle_core_failure(int core_number, std::string_view description) noexcept {
    log_engine(MessageType::Error, "core {}: {}", core_number, description);
    end_run_after_core_failure();
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
                } catch (const ProcessorFailure&) {
                    end_run_after_core_failure();
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
