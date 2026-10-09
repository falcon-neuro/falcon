#pragma once
#include <arpa/inet.h>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <new>
#include <string>
#include <thread>
#include <vector>

#define NP1_PROBE_CHANNEL_COUNT 384
#define NP1_PROBE_SUPERFRAMESIZE 12

struct electrodePacket {
    uint32_t timestamp[NP1_PROBE_SUPERFRAMESIZE];
    int16_t apData[NP1_PROBE_SUPERFRAMESIZE][NP1_PROBE_CHANNEL_COUNT];
    int16_t lfpData[NP1_PROBE_CHANNEL_COUNT];
    uint16_t Status[NP1_PROBE_SUPERFRAMESIZE];
};

class NeuropixelsInput {
   private:
    static constexpr size_t BUFFER_SIZE = 1024;

    alignas(64) std::vector<electrodePacket> m_ringBuffer;

    alignas(std::hardware_destructive_interference_size) std::atomic<size_t> m_head{0};
    alignas(std::hardware_destructive_interference_size) std::atomic<size_t> m_tail{0};
    alignas(std::hardware_destructive_interference_size) std::atomic<bool> m_running{false};

    std::thread m_receiverThread;
    std::string m_ip;
    int m_port;
    int m_sockfd{-1};

    io_uring m_ring;
    alignas(64) electrodePacket m_dropPacket;

    std::chrono::steady_clock::time_point m_lastLogTime{std::chrono::steady_clock::now()};

    uint64_t m_totalPacketsReceived{0};
    uint64_t m_totalPacketsDropped{0};
    uint64_t m_totalBytesReceived{0};
    uint64_t m_ioUringErrors{0};
    uint64_t m_malformedPackets{0};

    void logDiagnostics() {
        auto now = std::chrono::steady_clock::now();
        if (now - m_lastLogTime >= std::chrono::seconds(1)) {
            auto duration =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastLogTime).count();
            if (duration == 0) duration = 1;

            size_t head = m_head.load(std::memory_order_relaxed);
            size_t tail = m_tail.load(std::memory_order_relaxed);
            size_t fill = (head >= tail) ? (head - tail) : (BUFFER_SIZE - (tail - head));

            double mibReceived = static_cast<double>(m_totalBytesReceived) / (1024.0 * 1024.0);
            double packetRate =
                static_cast<double>(m_totalPacketsReceived + m_totalPacketsDropped) /
                (static_cast<double>(duration) / 1000.0);

            std::cout << "\033[2J\033[H" << std::flush;
            std::cout << "================ NEUROPIXELS INPUT DIAGNOSTICS ================\n";
            std::cout << "Ringbuffer Fill Level : " << fill << " / " << BUFFER_SIZE << "\n";
            std::cout << "Total Packets Recv    : " << m_totalPacketsReceived << "\n";
            std::cout << "Total Packets Dropped : " << m_totalPacketsDropped << "\n";
            std::cout << "Malformed Packets     : " << m_malformedPackets << "\n";
            std::cout << "io_uring Errors       : " << m_ioUringErrors << "\n";
            std::cout << "Data Received Total   : " << m_totalBytesReceived << " bytes ("
                      << mibReceived << " MiB)\n";
            std::cout << "Current Packet Rate   : " << packetRate << " pkts/sec\n";
            std::cout << "===============================================================\n";

            m_totalBytesReceived = 0;
            m_totalPacketsReceived = 0;
            m_totalPacketsDropped = 0;
            m_malformedPackets = 0;
            m_ioUringErrors = 0;
            m_lastLogTime = now;
        }
    }

    void submit_recv_request(size_t slot_idx, bool drop) {
        io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
        if (!sqe) {
            m_ioUringErrors++;
            return;
        }

        void* buf_addr =
            drop ? static_cast<void*>(&m_dropPacket) : static_cast<void*>(&m_ringBuffer[slot_idx]);
        size_t buf_len = sizeof(electrodePacket);

        io_uring_prep_recv(sqe, m_sockfd, buf_addr, buf_len, 0);
        sqe->user_data = drop ? uint64_t(-1) : static_cast<uint64_t>(slot_idx);

        io_uring_submit(&m_ring);
    }

    void receiverLoop() {
        size_t current_head = m_head.load(std::memory_order_relaxed);
        size_t current_tail = m_tail.load(std::memory_order_acquire);

        bool dropping = ((current_head + 1) % BUFFER_SIZE == current_tail);
        submit_recv_request(current_head, dropping);

        io_uring_cqe* cqe = nullptr;

        while (m_running.load(std::memory_order_relaxed)) {
            logDiagnostics();

            int ret = io_uring_wait_cqe(&m_ring, &cqe);
            if (ret < 0) {
                if (ret == -EINTR) continue;
                m_ioUringErrors++;
                std::this_thread::yield();
                continue;
            }

            int res = cqe->res;
            uint64_t user_data = cqe->user_data;
            io_uring_cqe_seen(&m_ring, cqe);

            if (res > 0) {
                m_totalBytesReceived += static_cast<uint64_t>(res);

                if (res == sizeof(electrodePacket)) {
                    if (user_data != uint64_t(-1)) {
                        m_totalPacketsReceived++;
                        m_head.store((user_data + 1) % BUFFER_SIZE, std::memory_order_release);
                    } else {
                        m_totalPacketsDropped++;
                    }
                } else {
                    m_malformedPackets++;
                }
            } else if (res < 0) {
                if (res != -EAGAIN && res != -EWOULDBLOCK && res != -EINTR) {
                    m_ioUringErrors++;
                }
            }

            current_head = m_head.load(std::memory_order_relaxed);
            current_tail = m_tail.load(std::memory_order_acquire);
            dropping = ((current_head + 1) % BUFFER_SIZE == current_tail);

            submit_recv_request(current_head, dropping);
        }
    }

   public:
    NeuropixelsInput(std::string ip = "0.0.0.0", int port = 6363)
        : m_ip(std::move(ip)), m_port(port) {
        m_ringBuffer.resize(BUFFER_SIZE);

        if (io_uring_queue_init(512, &m_ring, 0) < 0) {
            std::cerr << "Failed to initialize io_uring queue\n";
        }

        if (initSocket()) {
            std::cout << "Socket initialized\n";
        }
    }

    ~NeuropixelsInput() {
        stop();
        io_uring_queue_exit(&m_ring);
        if (m_sockfd != -1) {
            shutdown(m_sockfd, SHUT_RDWR);
            close(m_sockfd);
        }
    }

    bool initSocket() {
        if (m_sockfd != -1) return true;
        m_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (m_sockfd < 0) return false;

        int opt = 1;
        setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        int rcvbuf = 64 * 1024 * 1024;
        setsockopt(m_sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        struct sockaddr_in localAddr{};
        localAddr.sin_family = AF_INET;
        localAddr.sin_port = htons(static_cast<uint16_t>(m_port));
        if (inet_pton(AF_INET, m_ip.c_str(), &localAddr.sin_addr) <= 0) return false;
        if (bind(m_sockfd, (struct sockaddr*) &localAddr, sizeof(localAddr)) < 0) return false;
        return true;
    }

    void start() {
        if (m_running.load(std::memory_order_relaxed)) return;
        m_running.store(true, std::memory_order_relaxed);
        m_receiverThread = std::thread(&NeuropixelsInput::receiverLoop, this);

#if defined(__linux__)
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(0, &cpuset);
        pthread_setaffinity_np(m_receiverThread.native_handle(), sizeof(cpu_set_t), &cpuset);
#endif
    }

    void stop() {
        if (!m_running.load(std::memory_order_relaxed)) return;
        m_running.store(false, std::memory_order_relaxed);

        io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
        if (sqe) {
            io_uring_prep_nop(sqe);
            io_uring_submit(&m_ring);
        }

        if (m_sockfd != -1) shutdown(m_sockfd, SHUT_RDWR);
        if (m_receiverThread.joinable()) m_receiverThread.join();
    }

    // TODO: dont expose AoS, expose SoA
    bool peek_packet(electrodePacket*& packet_ptr, size_t& tail_idx_out) {
        size_t current_tail = m_tail.load(std::memory_order_relaxed);
        size_t current_head = m_head.load(std::memory_order_acquire);
        if (current_tail == current_head) return false;

        packet_ptr = &m_ringBuffer[current_tail];
        tail_idx_out = current_tail;
        return true;
    }

    void release_packet(size_t expected_tail) {
        m_tail.store((expected_tail + 1) % BUFFER_SIZE, std::memory_order_release);
    }
};
