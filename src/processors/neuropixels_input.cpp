#pragma once
#include <arpa/inet.h>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <thread>
#include <vector>

// Copy pasted from the Neuropixels library: a reference to the original data structure, which we
// don't modify yet (perhaps in the future). So clang-tidy skips this block.
// NOLINTBEGIN
#define NP1_PROBE_CHANNEL_COUNT 384
#define NP1_PROBE_SUPERFRAMESIZE 12

struct electrodePacket {
    uint32_t timestamp[NP1_PROBE_SUPERFRAMESIZE];
    int16_t apData[NP1_PROBE_SUPERFRAMESIZE][NP1_PROBE_CHANNEL_COUNT];
    int16_t lfpData[NP1_PROBE_CHANNEL_COUNT];
    uint16_t Status[NP1_PROBE_SUPERFRAMESIZE];
};
// NOLINTEND

// This is not a "processor" yet, it's just a simple implementation to test the UI
// In the future, it should inherit some sort of processor interface (probably without virtual
// methods, inheritance is just a concept, not meant for actual OOP inheritance)
class NeuropixelsInput {
private:
    static constexpr size_t buffer_size = 1024;
    // user_data of a receive into drop_packet_, used when the ring is full
    static constexpr uint64_t drop_slot = std::numeric_limits<uint64_t>::max();

    alignas(64) std::vector<electrodePacket> ring_buffer_;

    alignas(std::hardware_destructive_interference_size) std::atomic<size_t> head_{0};
    alignas(std::hardware_destructive_interference_size) std::atomic<size_t> tail_{0};
    alignas(std::hardware_destructive_interference_size) std::atomic<bool> running_{false};
    alignas(std::hardware_destructive_interference_size) std::atomic<uint32_t> publish_seq_{0};

    std::thread receiver_thread_;
    std::string ip_;
    int port_;
    int socket_fd_{-1};

    io_uring uring_;
    alignas(64) electrodePacket drop_packet_;

    std::chrono::steady_clock::time_point last_log_time_{std::chrono::steady_clock::now()};

    uint64_t total_packets_received_{0};
    uint64_t total_packets_dropped_{0};
    uint64_t dropped_since_start_{0};
    uint64_t total_bytes_received_{0};
    uint64_t io_uring_errors_{0};
    uint64_t malformed_packets_{0};
    size_t max_ring_fill_{0};

    void log_diagnostics() {
        auto now = std::chrono::steady_clock::now();
        if (now - last_log_time_ >= std::chrono::seconds(1)) {
            auto duration =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log_time_).count();
            if (duration == 0) {
                duration = 1;
            }

            size_t head = head_.load(std::memory_order_relaxed);
            size_t tail = tail_.load(std::memory_order_relaxed);
            size_t fill = (head >= tail) ? (head - tail) : (buffer_size - (tail - head));

            double mib_received = static_cast<double>(total_bytes_received_) / (1024.0 * 1024.0);
            double packet_rate =
                static_cast<double>(total_packets_received_ + total_packets_dropped_) /
                (static_cast<double>(duration) / 1000.0);

            std::cout << "\033[2J\033[H" << std::flush;
            std::cout << "================ NEUROPIXELS INPUT DIAGNOSTICS ================\n";
            std::cout << "Ringbuffer Fill Level : " << fill << " / " << buffer_size << "\n";
            std::cout << "Ringbuffer Max Fill   : " << max_ring_fill_ << " / " << buffer_size
                      << "\n";
            std::cout << "Total Packets Recv    : " << total_packets_received_ << "\n";
            std::cout << "Total Packets Dropped : " << total_packets_dropped_ << "\n";
            std::cout << "Dropped Since Start   : " << dropped_since_start_ << "\n";
            std::cout << "Malformed Packets     : " << malformed_packets_ << "\n";
            std::cout << "io_uring Errors       : " << io_uring_errors_ << "\n";
            std::cout << "Data Received Total   : " << total_bytes_received_ << " bytes ("
                      << mib_received << " MiB)\n";
            std::cout << "Current Packet Rate   : " << packet_rate << " pkts/sec\n";
            std::cout << "===============================================================\n";

            total_bytes_received_ = 0;
            total_packets_received_ = 0;
            total_packets_dropped_ = 0;
            malformed_packets_ = 0;
            io_uring_errors_ = 0;
            max_ring_fill_ = 0;
            last_log_time_ = now;
        }
    }

    void submit_recv_request(size_t slot_idx, bool drop) {
        io_uring_sqe* sqe = io_uring_get_sqe(&uring_);
        if (!sqe) {
            io_uring_errors_++;
            return;
        }

        void* buf_addr =
            drop ? static_cast<void*>(&drop_packet_) : static_cast<void*>(&ring_buffer_[slot_idx]);
        size_t buf_len = sizeof(electrodePacket);

        io_uring_prep_recv(sqe, socket_fd_, buf_addr, buf_len, 0);
        sqe->user_data = drop ? drop_slot : static_cast<uint64_t>(slot_idx);

        io_uring_submit(&uring_);
    }

    void receiver_loop() {
        size_t current_head = head_.load(std::memory_order_relaxed);
        size_t current_tail = tail_.load(std::memory_order_acquire);

        bool dropping = ((current_head + 1) % buffer_size == current_tail);
        submit_recv_request(current_head, dropping);

        io_uring_cqe* cqe = nullptr;

        while (running_.load(std::memory_order_relaxed)) {
            log_diagnostics();

            int ret = io_uring_wait_cqe(&uring_, &cqe);
            if (ret < 0) {
                if (ret == -EINTR) {
                    continue;
                }
                io_uring_errors_++;
                std::this_thread::yield();
                continue;
            }

            int res = cqe->res;
            uint64_t user_data = cqe->user_data;
            io_uring_cqe_seen(&uring_, cqe);

            if (res > 0) {
                total_bytes_received_ += static_cast<uint64_t>(res);

                if (res == sizeof(electrodePacket)) {
                    if (user_data != drop_slot) {
                        total_packets_received_++;
                        head_.store((user_data + 1) % buffer_size, std::memory_order_release);
                        publish_seq_.fetch_add(1, std::memory_order_release);
                        publish_seq_.notify_one();
                    } else {
                        total_packets_dropped_++;
                        dropped_since_start_++;
                    }
                } else {
                    malformed_packets_++;
                }
            } else if (res < 0) {
                if (res != -EAGAIN && res != -EWOULDBLOCK && res != -EINTR) {
                    io_uring_errors_++;
                }
            }

            current_head = head_.load(std::memory_order_relaxed);
            current_tail = tail_.load(std::memory_order_acquire);
            dropping = ((current_head + 1) % buffer_size == current_tail);

            size_t fill = (current_head + buffer_size - current_tail) % buffer_size;
            max_ring_fill_ = std::max(fill, max_ring_fill_);

            submit_recv_request(current_head, dropping);
        }
    }

public:
    NeuropixelsInput(std::string ip_address = "0.0.0.0", int port = 6363)
        : ip_(std::move(ip_address)), port_(port) {
        ring_buffer_.resize(buffer_size);

        if (io_uring_queue_init(512, &uring_, 0) < 0) {
            std::cerr << "Failed to initialize io_uring queue\n";
        }

        if (init_socket()) {
            std::cout << "Socket initialized\n";
        }
    }

    ~NeuropixelsInput() {
        stop();
        io_uring_queue_exit(&uring_);
        if (socket_fd_ != -1) {
            shutdown(socket_fd_, SHUT_RDWR);
            close(socket_fd_);
        }
    }

    bool init_socket() {
        if (socket_fd_ != -1) {
            return true;
        }
        socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd_ < 0) {
            return false;
        }

        int opt = 1;
        setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        // Kernel doubles this, capped by net.core.rmem_max: 16 MiB -> 32 MiB buffer.
        int rcvbuf = 16 * 1024 * 1024;
        setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        struct sockaddr_in local_addr{};
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(static_cast<uint16_t>(port_));
        if (inet_pton(AF_INET, ip_.c_str(), &local_addr.sin_addr) <= 0) {
            return false;
        }
        if (bind(socket_fd_, reinterpret_cast<sockaddr*>(&local_addr), sizeof(local_addr)) < 0) {
            return false;
        }
        return true;
    }

    void start() {
        if (running_.load(std::memory_order_relaxed)) {
            return;
        }
        running_.store(true, std::memory_order_relaxed);
        receiver_thread_ = std::thread(&NeuropixelsInput::receiver_loop, this);

#ifdef __linux__
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(5, &cpuset);
        pthread_setaffinity_np(receiver_thread_.native_handle(), sizeof(cpu_set_t), &cpuset);
#endif
    }

    void stop() {
        if (!running_.load(std::memory_order_relaxed)) {
            return;
        }
        running_.store(false, std::memory_order_relaxed);

        io_uring_sqe* sqe = io_uring_get_sqe(&uring_);
        if (sqe) {
            io_uring_prep_nop(sqe);
            io_uring_submit(&uring_);
        }

        if (socket_fd_ != -1) {
            shutdown(socket_fd_, SHUT_RDWR);
        }
        if (receiver_thread_.joinable()) {
            receiver_thread_.join();
        }
    }

    // TODO: dont expose AoS, expose SoA
    // ...or offer both AoS and SoA
    bool peek_packet(electrodePacket*& packet_ptr, size_t& tail_idx_out) {
        size_t current_tail = tail_.load(std::memory_order_relaxed);
        size_t current_head = head_.load(std::memory_order_acquire);
        if (current_tail == current_head) {
            return false;
        }

        packet_ptr = &ring_buffer_[current_tail];
        tail_idx_out = current_tail;
        return true;
    }

    void release_packet(size_t expected_tail) {
        tail_.store((expected_tail + 1) % buffer_size, std::memory_order_release);
    }

    // Read publish_seq() before checking the ring, then pass it to wait_for_packet().
    // Anything published (or wake_consumer()) after that read makes the wait return.
    uint32_t publish_seq() const { return publish_seq_.load(std::memory_order_acquire); }

    void wait_for_packet(uint32_t seq) { publish_seq_.wait(seq, std::memory_order_acquire); }

    void wake_consumer() {
        publish_seq_.fetch_add(1, std::memory_order_release);
        publish_seq_.notify_all();
    }
};
