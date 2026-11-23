#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <x86intrin.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include "../core/engine.cpp"
#include "../core/misc.cpp"

// NeuralynxInput: reads a Neuralynx acquisition system's raw data records from UDP, one record per
// datagram and one sample per record, and outputs a run of its A/D channels in µV, one sample per
// pass. It asks its socket once per pass without waiting, so it needs a spinning core. Lost samples
// are found from the records' own timestamps, never from when a datagram arrived.

// A raw data record as Neuralynx's "Data File Formats" describes it (section Raw Data A/D Record):
// 32-bit fields in this order, with one data value per A/D channel between the extra values and
// the checksum.
struct NeuralynxRecord {
    static constexpr std::size_t start_mark_field = 0;
    static constexpr std::size_t packet_id_field = 1;
    static constexpr std::size_t packet_size_field = 2;
    static constexpr std::size_t timestamp_high_field = 3;
    static constexpr std::size_t timestamp_low_field = 4;
    static constexpr std::size_t first_data_field = 17;

    static constexpr std::int32_t start_mark = 2048;
    static constexpr std::int32_t raw_data_packet_id = 1;
    // The size field counts these along with the data values.
    static constexpr std::size_t extra_value_count = 10;
    static constexpr std::size_t fields_besides_data = 18;

    // No UDP datagram holds more than 65,507 bytes, so a buffer of this many fields takes any
    // record whole.
    static constexpr std::size_t most_fields = 16384;
    static constexpr std::size_t most_data_values = most_fields - fields_besides_data;
};

// Whether a string option is an IPv4 address as the socket takes one: four numbers from 0 to 255
// with dots between them, none written with a leading zero.
[[nodiscard]] constexpr bool is_ipv4_address(const Str& text) {
    const std::string_view address = text.c_str();
    int finished_number_count = 0;
    int digit_count = 0;
    int number = 0;
    for (const char character : address) {
        if (character == '.') {
            if (digit_count == 0) {
                return false;
            }
            ++finished_number_count;
            digit_count = 0;
            number = 0;
            continue;
        }
        const bool is_digit = character >= '0' && character <= '9';
        const bool has_leading_zero = digit_count > 0 && number == 0;
        if (!is_digit || has_leading_zero) {
            return false;
        }
        number = (number * 10) + (character - '0');
        ++digit_count;
        if (number > 255) {
            return false;
        }
    }
    return digit_count > 0 && finished_number_count == 3;
}

struct NeuralynxInputOptions {
    /// The IP address on this computer that the data arrives at, e.g. 192.168.3.100. The
    /// acquisition system sends its data to one fixed address, so the network card that receives
    /// it must be given that address.
    Str address = "127.0.0.1";
    /// The UDP port the data arrives at, from 1 to 65535.
    int port = 5000;
    /// The first A/D channel of the stream, counted from 0. It can't be negative.
    int first_channel = 0;
    /// The last A/D channel of the stream. The stream takes every A/D channel from the first to
    /// the last, so the two must span exactly as many as the stream has channels. It can't be
    /// before the first.
    int last_channel = 127;
    /// The acquisition system's sampling rate, in Hz. It differs between machines, and it must
    /// equal the rate of the stream.
    double sampling_rate = 32768;
    /// Neuralynx's ADBitVolts: what one step of a raw value is worth, in volts. It depends on the
    /// acquisition hardware, and it must be the value for the raw data. A negative value turns
    /// the signal upside down; it can't be 0.
    double ad_bit_volts = 0.000000305175781250000006;
    /// How long no data may arrive before a warning is logged, in milliseconds. It must be above
    /// 0.
    double no_data_warning_milliseconds = 100;
};

/// Reads the raw signal of a Neuralynx acquisition system from the network: a run of its A/D
/// channels, in µV. It needs a core of its own.
template <typename OutputStreamInfo, NeuralynxInputOptions options = NeuralynxInputOptions{}>
struct NeuralynxInput {
    static_assert(is_ipv4_address(options.address),
                  "the address must be four numbers from 0 to 255 with dots between them, e.g. "
                  "192.168.3.100");
    static_assert(options.port >= 1 && options.port <= 65535, "the port must be from 1 to 65535");
    static_assert(options.first_channel >= 0, "the first channel can't be negative");
    static_assert(options.last_channel >= options.first_channel,
                  "the last channel can't be before the first");
    static_assert(options.last_channel - options.first_channel + 1 == OutputStreamInfo::channels,
                  "the first and the last channel must span exactly as many channels as the "
                  "stream has");
    static_assert(std::cmp_less(options.last_channel, NeuralynxRecord::most_data_values),
                  "the last channel is beyond what one UDP datagram can hold");
    static_assert(options.sampling_rate == OutputStreamInfo::rate,
                  "the sampling rate must equal the rate of the stream");
    static_assert(options.sampling_rate <= 1'000'000.0,
                  "a record's timestamp counts whole microseconds, so the sampling rate can't be "
                  "above 1 MHz");
    static_assert(options.ad_bit_volts != 0 &&
                      options.ad_bit_volts >= -std::numeric_limits<double>::max() &&
                      options.ad_bit_volts <= std::numeric_limits<double>::max(),
                  "the ADBitVolts must be a number other than 0");
    static_assert(options.no_data_warning_milliseconds > 0,
                  "the time without data before a warning must be above 0");

    struct Connections {
        /// The raw signal of the chosen A/D channels, in µV, one sample at a time at the
        /// acquisition system's rate.
        SampleOutputBuffer<OutputStreamInfo> output;
    };

    explicit NeuralynxInput(ProcessorName processor_name) : name(processor_name) {
        constexpr double milliseconds_per_second = 1000;
        no_data_warning_ticks_ = static_cast<u64>(options.no_data_warning_milliseconds /
                                                  milliseconds_per_second * tsc_ticks_per_second);
        warn_about_interrupt_batching(name);
        socket_.open_and_listen(name);
        log(name, "listening on {}:{} for the acquisition system's records",
            options.address.c_str(), options.port);
    }

    ~NeuralynxInput() {
        if (lost_sample_count_ > 0) {
            warn(name, "lost {} samples in all before they reached Falcon", lost_sample_count_);
        }
        if (invalid_record_count_ > 0) {
            warn(name, "discarded {} invalid records in all", invalid_record_count_);
        }
        if (stale_record_count_ > 0) {
            warn(name, "discarded {} records in all that weren't newer than the one before them",
                 stale_record_count_);
        }
    }

    NeuralynxInput(const NeuralynxInput&) = delete;
    NeuralynxInput& operator=(const NeuralynxInput&) = delete;
    NeuralynxInput(NeuralynxInput&&) = delete;
    NeuralynxInput& operator=(NeuralynxInput&&) = delete;

    RegisteredName name;

    void process(Connections connections) {
        if (!has_discarded_startup_backlog_) [[unlikely]] {
            discard_startup_backlog();
            return;
        }
        const ssize_t received_bytes =
            ::recv(socket_.descriptor(), record_.data(), sizeof(record_), 0);
        if (received_bytes < 0) {
            note_nothing_received(errno);
            return;
        }
        const u64 arrival_tsc = __rdtsc();
        last_arrival_tsc_ = arrival_tsc;
        has_warned_about_no_data_ = false;

        const auto record_bytes = static_cast<std::size_t>(received_bytes);
        const char* const defect = defect_of_record(record_bytes);
        if (defect != nullptr) {
            ++invalid_record_count_;
            warn(name, "discarded an invalid record: {} ({} discarded so far)", defect,
                 invalid_record_count_);
            return;
        }
        const std::size_t data_value_count =
            (record_bytes / sizeof(std::int32_t)) - NeuralynxRecord::fields_besides_data;
        if (data_value_count < needed_data_value_count) [[unlikely]] {
            fail(name,
                 "the acquisition system sends {} channels in a record, but the last channel is "
                 "set to {}",
                 data_value_count, options.last_channel);
        }

        const u64 timestamp = timestamp_of_record();
        if (!advance_sample_number(timestamp)) {
            return;
        }
        write_sample(connections.output, {
                                             .sample_number = sample_number_,
                                             .device_timestamp = timestamp,
                                             .arrival_tsc = arrival_tsc,
                                             .lost_sample_count = lost_sample_count_,
                                         });
    }

private:
    // A member, so the socket is also closed when the constructor fails after opening it: a
    // failing constructor's own destructor doesn't run, its members' destructors do.
    class Socket {
    public:
        Socket() = default;

        ~Socket() {
            if (descriptor_ >= 0) {
                ::close(descriptor_);
            }
        }

        Socket(const Socket&) = delete;
        Socket& operator=(const Socket&) = delete;
        Socket(Socket&&) = delete;
        Socket& operator=(Socket&&) = delete;

        void open_and_listen(ProcessorName processor_name) {
            descriptor_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (descriptor_ < 0) {
                fail(processor_name, "can't open a UDP socket: {}",
                     std::generic_category().message(errno).c_str());
            }
            sockaddr_in local_address{};
            local_address.sin_family = AF_INET;
            // In network byte order, the reverse of x86's.
            local_address.sin_port = std::byteswap(static_cast<std::uint16_t>(options.port));
            if (inet_pton(AF_INET, options.address.c_str(), &local_address.sin_addr) != 1) {
                fail(processor_name, "can't read {} as an IPv4 address", options.address.c_str());
            }
            // Without SO_REUSEADDR: a second program listening on the same port would take the
            // datagrams away, so its start must fail instead.
            if (::bind(descriptor_, reinterpret_cast<const sockaddr*>(&local_address),
                       sizeof(local_address)) != 0) {
                fail(processor_name, "can't listen on {}:{}: {}", options.address.c_str(),
                     options.port, std::generic_category().message(errno).c_str());
            }
        }

        [[nodiscard]] int descriptor() const { return descriptor_; }

    private:
        int descriptor_ = -1;
    };

    static constexpr std::size_t channel_count =
        static_cast<std::size_t>(OutputStreamInfo::channels);
    static constexpr std::size_t needed_data_value_count =
        static_cast<std::size_t>(options.last_channel) + 1;
    static constexpr std::size_t first_value_field =
        NeuralynxRecord::first_data_field + static_cast<std::size_t>(options.first_channel);

    static constexpr double microvolts_per_raw_step = options.ad_bit_volts * 1'000'000.0;

    static constexpr double sample_period_microseconds = 1'000'000.0 / options.sampling_rate;
    // A timestamp holds whole microseconds, so two samples in a row are the sample period cut to
    // whole microseconds apart, or one more.
    static constexpr u64 longest_step_microseconds =
        static_cast<u64>(sample_period_microseconds) + 1;

    Socket socket_;
    alignas(64) std::array<std::int32_t, NeuralynxRecord::most_fields> record_{};
    // The stream's channels of the record; the padding stays 0.
    alignas(64) std::array<std::int32_t, OutputStreamInfo::padded_channels> raw_values_{};

    bool has_discarded_startup_backlog_ = false;
    bool has_started_ = false;
    u64 previous_timestamp_ = 0;
    u64 sample_number_ = 0;
    u64 lost_sample_count_ = 0;
    u64 invalid_record_count_ = 0;
    u64 stale_record_count_ = 0;
    u64 no_data_warning_ticks_ = 0;
    u64 last_arrival_tsc_ = 0;
    bool has_warned_about_no_data_ = false;

    // Datagrams that arrived while the pipeline was still starting are old by now, and the kernel
    // dropped some if its buffer filled up meanwhile, which would look like a gap at every start.
    // The one place where process() loops: once, on its first pass.
    void discard_startup_backlog() {
        // A sender far faster than an acquisition system could otherwise keep the loop busy for
        // good.
        constexpr int most_discarded_datagrams = 1'000'000;
        for (int discarded_count = 0; discarded_count < most_discarded_datagrams;
             ++discarded_count) {
            if (::recv(socket_.descriptor(), record_.data(), sizeof(record_), 0) < 0) {
                break;
            }
        }
        has_discarded_startup_backlog_ = true;
        last_arrival_tsc_ = __rdtsc();
    }

    void note_nothing_received(int error_number) {
        // What a socket that doesn't wait gives when no datagram is there.
        if (error_number != EAGAIN) {
            warn(name, "can't read from its socket (errno {})", error_number);
            return;
        }
        warn_once_data_stops();
    }

    // Once per stop, e.g. while the network cable is out; the samples lost meanwhile are warned
    // about when data comes back.
    void warn_once_data_stops() {
        if (has_warned_about_no_data_ || __rdtsc() - last_arrival_tsc_ < no_data_warning_ticks_) {
            return;
        }
        has_warned_about_no_data_ = true;
        warn(name, "no data from the acquisition system for {} ms",
             options.no_data_warning_milliseconds);
    }

    // What is wrong with the record just received, checked in the order Neuralynx gives, or
    // nullptr for a record that passes.
    [[nodiscard]] const char* defect_of_record(std::size_t record_bytes) const {
        if (record_bytes % sizeof(std::int32_t) != 0 ||
            record_bytes < NeuralynxRecord::fields_besides_data * sizeof(std::int32_t)) {
            return "its length is no record's";
        }
        if (record_[NeuralynxRecord::start_mark_field] != NeuralynxRecord::start_mark) {
            return "it doesn't begin with the start mark";
        }
        if (record_[NeuralynxRecord::packet_id_field] != NeuralynxRecord::raw_data_packet_id) {
            return "its packet type isn't the raw data's";
        }
        const std::size_t field_count = record_bytes / sizeof(std::int32_t);
        const std::size_t data_value_count = field_count - NeuralynxRecord::fields_besides_data;
        if (std::cmp_not_equal(record_[NeuralynxRecord::packet_size_field],
                               data_value_count + NeuralynxRecord::extra_value_count)) {
            return "its size field doesn't fit its length";
        }
        // The checksum field makes all of a record's fields, itself included, XOR to 0.
        const std::span<const std::int32_t> fields(record_.data(), field_count);
        if (std::reduce(fields.begin(), fields.end(), std::int32_t{0}, std::bit_xor<>()) != 0) {
            return "its checksum is wrong";
        }
        return nullptr;
    }

    [[nodiscard]] u64 timestamp_of_record() const {
        const auto high = static_cast<u32>(record_[NeuralynxRecord::timestamp_high_field]);
        const auto low = static_cast<u32>(record_[NeuralynxRecord::timestamp_low_field]);
        return (static_cast<u64>(high) << 32U) | static_cast<u64>(low);
    }

    // A value beyond what u64 holds becomes the largest u64, where a plain cast would be
    // undefined.
    [[nodiscard]] static u64 rounded_to_u64(long double value) {
        constexpr auto largest = static_cast<long double>(std::numeric_limits<u64>::max());
        const long double rounded = std::round(value);
        return rounded >= largest ? std::numeric_limits<u64>::max() : static_cast<u64>(rounded);
    }

    // Sets sample_number_ for the record with this timestamp. Returns false for a record that
    // isn't newer than the one before, which is discarded.
    [[nodiscard]] bool advance_sample_number(u64 timestamp) {
        if (!has_started_) {
            constexpr long double microseconds_per_second = 1'000'000.0L;
            sample_number_ = rounded_to_u64(static_cast<long double>(timestamp) *
                                            static_cast<long double>(options.sampling_rate) /
                                            microseconds_per_second);
            previous_timestamp_ = timestamp;
            has_started_ = true;
            return true;
        }
        if (timestamp <= previous_timestamp_) {
            ++stale_record_count_;
            warn(name,
                 "discarded a record whose timestamp {} isn't above the one before it, {} ({} "
                 "discarded so far)",
                 timestamp, previous_timestamp_, stale_record_count_);
            return false;
        }
        const u64 step_microseconds = timestamp - previous_timestamp_;
        u64 step_samples = 1;
        if (step_microseconds > longest_step_microseconds) [[unlikely]] {
            // Counted from this step alone, so a device whose rate is slightly off the nominal one
            // never adds up to a sample.
            step_samples = std::max<u64>(
                1, rounded_to_u64(static_cast<long double>(step_microseconds) /
                                  static_cast<long double>(sample_period_microseconds)));
            const u64 lost_samples = step_samples - 1;
            lost_sample_count_ += lost_samples;
            warn(name,
                 "a gap in the acquisition system's timestamps: two records in a row are {} "
                 "microseconds apart, where at most {} are expected; {} samples lost before "
                 "sample {}",
                 step_microseconds, longest_step_microseconds, lost_samples,
                 sample_number_ + step_samples);
        }
        sample_number_ += step_samples;
        previous_timestamp_ = timestamp;
        return true;
    }

    void write_sample(SampleOutputBuffer<OutputStreamInfo>& output, const SampleHeader& header) {
        std::copy_n(&record_[first_value_field], channel_count, raw_values_.data());
        for (int simd_vector_index = 0; simd_vector_index < OutputStreamInfo::simd_vectors;
             ++simd_vector_index) {
            const std::int32_t* const vector_raw_values =
                &raw_values_[static_cast<std::size_t>(simd_vector_index) *
                             static_cast<std::size_t>(FLOATS_PER_SIMD_VECTOR)];
            output.store(simd_vector_index, to_microvolts(vector_raw_values));
        }
        output.set_header(header);
    }

    // One channel vector of raw values, in µV. Computed in double, so a value is rounded to
    // float32 once, after the multiplication, and a raw value of more than 24 bits loses nothing
    // before it. `auto`, so the AVX512 branch's intrinsics depend on the call and aren't checked
    // in an AVX2 build.
    [[nodiscard]] static auto to_microvolts(const auto* raw_values) {
        constexpr int values_per_half = FLOATS_PER_SIMD_VECTOR / 2;
        if constexpr (HAS_AVX512) {
            const __m512d factor = _mm512_set1_pd(microvolts_per_raw_step);
            const __m256i first_raw_half =
                _mm256_load_si256(reinterpret_cast<const __m256i*>(raw_values));
            const __m256i second_raw_half =
                _mm256_load_si256(reinterpret_cast<const __m256i*>(raw_values + values_per_half));
            const __m256 first_half =
                _mm512_cvtpd_ps(_mm512_mul_pd(_mm512_cvtepi32_pd(first_raw_half), factor));
            const __m256 second_half =
                _mm512_cvtpd_ps(_mm512_mul_pd(_mm512_cvtepi32_pd(second_raw_half), factor));
            const __m512d halves =
                _mm512_insertf64x4(_mm512_castps_pd(_mm512_castps256_ps512(first_half)),
                                   _mm256_castps_pd(second_half), 1);
            return _mm512_castpd_ps(halves);
        } else {
            const __m256d factor = _mm256_set1_pd(microvolts_per_raw_step);
            const __m128i first_raw_half =
                _mm_load_si128(reinterpret_cast<const __m128i*>(raw_values));
            const __m128i second_raw_half =
                _mm_load_si128(reinterpret_cast<const __m128i*>(raw_values + values_per_half));
            const __m128 first_half =
                _mm256_cvtpd_ps(_mm256_mul_pd(_mm256_cvtepi32_pd(first_raw_half), factor));
            const __m128 second_half =
                _mm256_cvtpd_ps(_mm256_mul_pd(_mm256_cvtepi32_pd(second_raw_half), factor));
            return _mm256_set_m128(second_half, first_half);
        }
    }
};
