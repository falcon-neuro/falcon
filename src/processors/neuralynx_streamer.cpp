#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
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
#include <system_error>
#include <utility>
#include "../core/engine.cpp"
#include "neuralynx_input.cpp"

// NeuralynxStreamer: sends the stream it receives over UDP as a Neuralynx acquisition system
// would, one raw data record per datagram and per sample, so that with a signal generator before
// it, it stands in for such a system when a graph that reads from one is tested. It only makes and
// sends records: what a test needs beyond a healthy system's records is made by the processors
// before it.

struct NeuralynxStreamerOptions {
    /// The IP address the records are sent to, e.g. 192.168.3.100. 127.0.0.1 is this computer.
    Str address = "127.0.0.1";
    /// The UDP port the records are sent to, from 1 to 65535.
    int port = 5000;
    /// Neuralynx's ADBitVolts: what one step of a raw value is worth, in volts. The processor that
    /// reads the records must be given the same value. A negative value turns the signal upside
    /// down; it can't be 0.
    double ad_bit_volts = 0.000000305175781250000006;
};

/// Sends the signal it receives over the network as a Neuralynx acquisition system would, one
/// record for each sample, so that a graph that reads from such a system can be tested without
/// one.
template <typename InputStreamInfo, NeuralynxStreamerOptions options = NeuralynxStreamerOptions{}>
struct NeuralynxStreamer {
    static_assert(is_ipv4_address(options.address),
                  "the address must be four numbers from 0 to 255 with dots between them, e.g. "
                  "192.168.3.100");
    static_assert(options.port >= 1 && options.port <= 65535, "the port must be from 1 to 65535");
    static_assert(options.ad_bit_volts != 0 &&
                      options.ad_bit_volts >= -std::numeric_limits<double>::max() &&
                      options.ad_bit_volts <= std::numeric_limits<double>::max(),
                  "the ADBitVolts must be a number other than 0");
    static_assert((NeuralynxRecord::fields_besides_data +
                   static_cast<std::size_t>(InputStreamInfo::channels)) *
                          sizeof(std::int32_t) <=
                      65507,
                  "the stream has more channels than one UDP datagram can hold");
    static_assert(InputStreamInfo::rate <= 1'000'000.0,
                  "a record's timestamp counts whole microseconds, so the stream's rate can't be "
                  "above 1 MHz");

    struct Connections {
        /// The signal to send, in µV. Each sample is sent as one record that holds every channel
        /// of the stream.
        SampleInputBuffer<InputStreamInfo> input;
    };

    explicit NeuralynxStreamer(ProcessorName processor_name) : name(processor_name) {
        socket_.open(name);
        destination_.sin_family = AF_INET;
        // In network byte order, the reverse of x86's.
        destination_.sin_port = std::byteswap(static_cast<std::uint16_t>(options.port));
        if (inet_pton(AF_INET, options.address.c_str(), &destination_.sin_addr) != 1) {
            fail(name, "can't read {} as an IPv4 address", options.address.c_str());
        }
        record_[NeuralynxRecord::start_mark_field] = NeuralynxRecord::start_mark;
        record_[NeuralynxRecord::packet_id_field] = NeuralynxRecord::raw_data_packet_id;
        record_[NeuralynxRecord::packet_size_field] =
            static_cast<std::int32_t>(channel_count + NeuralynxRecord::extra_value_count);
        log(name, "sending records of {} channels to {}:{}", channel_count, options.address.c_str(),
            options.port);
    }

    ~NeuralynxStreamer() {
        if (unsent_record_count_ > 0) {
            warn(name, "couldn't send {} records in all", unsent_record_count_);
        }
    }

    NeuralynxStreamer(const NeuralynxStreamer&) = delete;
    NeuralynxStreamer& operator=(const NeuralynxStreamer&) = delete;
    NeuralynxStreamer(NeuralynxStreamer&&) = delete;
    NeuralynxStreamer& operator=(NeuralynxStreamer&&) = delete;

    RegisteredName name;

    void process(Connections connections) {
        if (connections.input.is_empty()) {
            return;
        }
        fill_record(connections.input);
        send_record();
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

        void open(ProcessorName processor_name) {
            descriptor_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (descriptor_ < 0) {
                fail(processor_name, "can't open a UDP socket: {}",
                     std::generic_category().message(errno).c_str());
            }
        }

        [[nodiscard]] int descriptor() const { return descriptor_; }

    private:
        int descriptor_ = -1;
    };

    static constexpr std::size_t channel_count =
        static_cast<std::size_t>(InputStreamInfo::channels);
    static constexpr std::size_t record_field_count =
        NeuralynxRecord::fields_besides_data + channel_count;
    static constexpr std::size_t checksum_field = record_field_count - 1;

    static constexpr double microvolts_per_raw_step = options.ad_bit_volts * 1'000'000.0;
    static constexpr double lowest_raw_value = std::numeric_limits<std::int32_t>::min();
    static constexpr double highest_raw_value = std::numeric_limits<std::int32_t>::max();

    Socket socket_;
    sockaddr_in destination_{};
    // The status, the parallel input port and the extra values stay 0.
    alignas(64) std::array<std::int32_t, record_field_count> record_{};
    u64 unsent_record_count_ = 0;

    [[nodiscard]] static bool is_raw_value(double raw_steps) {
        return raw_steps >= lowest_raw_value && raw_steps <= highest_raw_value;
    }

    // The time a device would have taken the sample at, in whole microseconds: its number times
    // the sample period, with the fraction cut off.
    [[nodiscard]] static u64 timestamp_of(u64 sample_number) {
        constexpr long double microseconds_per_second = 1'000'000.0L;
        return static_cast<u64>(static_cast<long double>(sample_number) * microseconds_per_second /
                                static_cast<long double>(InputStreamInfo::rate));
    }

    void fill_record(const SampleInputBuffer<InputStreamInfo>& input) {
        const u64 timestamp = timestamp_of(input.header.sample_number);
        record_[NeuralynxRecord::timestamp_high_field] =
            std::bit_cast<std::int32_t>(static_cast<u32>(timestamp >> 32U));
        record_[NeuralynxRecord::timestamp_low_field] =
            std::bit_cast<std::int32_t>(static_cast<u32>(timestamp));

        for (int simd_vector_index = 0; simd_vector_index < InputStreamInfo::simd_vectors;
             ++simd_vector_index) {
            alignas(64) std::array<float, FLOATS_PER_SIMD_VECTOR> lane_values{};
            // auto, not SimdVector: a type that depends on the template keeps the other
            // instruction set's branch from being compiled.
            const auto values = input.load(simd_vector_index);
            if constexpr (HAS_AVX512) {
                _mm512_store_ps(lane_values.data(), values);
            } else {
                _mm256_store_ps(lane_values.data(), values);
            }
            const std::size_t first_channel = static_cast<std::size_t>(simd_vector_index) *
                                              static_cast<std::size_t>(FLOATS_PER_SIMD_VECTOR);
            // The padding channels after the stream's last one aren't sent.
            for (std::size_t lane = 0;
                 lane < lane_values.size() && first_channel + lane < channel_count; ++lane) {
                const std::size_t channel = first_channel + lane;
                const double raw_steps =
                    std::round(static_cast<double>(lane_values[lane]) / microvolts_per_raw_step);
                if (!is_raw_value(raw_steps)) [[unlikely]] {
                    fail(name,
                         "channel {} holds {} microvolts, which a 32-bit raw value can't hold "
                         "with this ADBitVolts",
                         channel, lane_values[lane]);
                }
                record_[NeuralynxRecord::first_data_field + channel] =
                    static_cast<std::int32_t>(raw_steps);
            }
        }

        // The checksum field makes all of a record's fields, itself included, XOR to 0.
        const std::span<const std::int32_t> fields_before_checksum(record_.data(), checksum_field);
        record_[checksum_field] =
            std::reduce(fields_before_checksum.begin(), fields_before_checksum.end(),
                        std::int32_t{0}, std::bit_xor<>());
    }

    // Without waiting, and to an address that needn't be listening, as a device sends. A record
    // that can't be sent is dropped, which its reader sees as a gap.
    void send_record() {
        const ssize_t sent_bytes =
            ::sendto(socket_.descriptor(), record_.data(), sizeof(record_), 0,
                     reinterpret_cast<const sockaddr*>(&destination_), sizeof(destination_));
        if (std::cmp_not_equal(sent_bytes, sizeof(record_))) [[unlikely]] {
            ++unsent_record_count_;
            warn(name, "couldn't send a record (errno {}); {} not sent so far", errno,
                 unsent_record_count_);
        }
    }
};
