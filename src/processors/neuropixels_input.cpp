#pragma once
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <thread>
#define NP1_PROBE_CHANNEL_COUNT 384
#define NP1_PROBE_SUPERFRAMESIZE 12

struct electrodePacket {
    uint32_t timestamp[NP1_PROBE_SUPERFRAMESIZE];
    int16_t apData[NP1_PROBE_SUPERFRAMESIZE][NP1_PROBE_CHANNEL_COUNT];
    int16_t lfpData[NP1_PROBE_CHANNEL_COUNT];
    uint16_t Status[NP1_PROBE_SUPERFRAMESIZE];
};

enum class WaveformType : int8_t { Sine, Square, Sawtooth, Triangle, Noise };

class NeuropixelsInput {
   private:
    static constexpr size_t BUFFER_SIZE = 64;
    electrodePacket m_ringBuffer[BUFFER_SIZE];

    std::atomic<size_t> m_head{0};
    std::atomic<size_t> m_tail{0};

    std::atomic<bool> m_running{false};
    std::thread m_simulatorThread;
    uint32_t m_globalTimestampCounter{0};
    uint64_t m_rngState{0x123456789ABCDEF0ULL};

    inline double fastWhiteNoise() {
        m_rngState ^= (m_rngState << 13);
        m_rngState ^= (m_rngState >> 7);
        m_rngState ^= (m_rngState << 17);
        return (static_cast<double>(m_rngState & 0xFFFFFFFF) / 4294967295.0) * 2.0 - 1.0;
    }
    int16_t generateWaveform(WaveformType type, double phase, double amplitude = 100.0) {
        double normalizedPhase =
            (phase / (2.0 * std::numbers::pi)) - std::floor(phase / (2.0 * std::numbers::pi));

        switch (type) {
            case WaveformType::Sine:
                return static_cast<int16_t>(amplitude * std::sin(phase));

            case WaveformType::Square:
                return static_cast<int16_t>((normalizedPhase < 0.5) ? amplitude : -amplitude);

            case WaveformType::Sawtooth:
                return static_cast<int16_t>(amplitude * (2.0 * normalizedPhase - 1.0));

            case WaveformType::Triangle:
                return static_cast<int16_t>(amplitude *
                                            (4.0 * std::abs(normalizedPhase - 0.5) - 1.0));

            case WaveformType::Noise:
                return static_cast<int16_t>(amplitude * fastWhiteNoise());

            default:
                return 0;
        }
    }

    void simulatorLoop() {
        auto waveform = WaveformType::Noise;
        auto freq = 40;
        auto startTime = std::chrono::high_resolution_clock::now();
        uint64_t packetCount = 0;
        const std::chrono::nanoseconds packetInterval(400000);

        while (m_running.load(std::memory_order_relaxed)) {
            size_t current_head = m_head.load(std::memory_order_relaxed);
            size_t current_tail = m_tail.load(std::memory_order_acquire);

            if ((current_head + 1) % BUFFER_SIZE != current_tail) {
                electrodePacket& packet = m_ringBuffer[current_head];

                for (int s = 0; s < NP1_PROBE_SUPERFRAMESIZE; ++s) {
                    packet.timestamp[s] = m_globalTimestampCounter++;
                    packet.Status[s] = 0;

                    for (int c = 0; c < NP1_PROBE_CHANNEL_COUNT; ++c) {
                        double phase = (static_cast<double>(m_globalTimestampCounter) *
                                        ((freq * 2 * std::numbers::pi) / 30000.0)) +
                                       c;

                        packet.apData[s][c] = generateWaveform(waveform, phase);
                    }
                }

                m_head.store((current_head + 1) % BUFFER_SIZE, std::memory_order_release);
            }

            packetCount++;
            auto nextTargetTime = startTime + (packetInterval * packetCount);

            while (std::chrono::high_resolution_clock::now() < nextTargetTime) {
#if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
#endif
            }
        }
    }

   public:
    NeuropixelsInput() = default;

    ~NeuropixelsInput() { stop(); }
    void start() {
        if (m_running.load(std::memory_order_relaxed)) return;
        m_running.store(true, std::memory_order_relaxed);
        m_simulatorThread = std::thread(&NeuropixelsInput::simulatorLoop, this);

#if defined(__linux__)
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(7, &cpuset);
        pthread_setaffinity_np(m_simulatorThread.native_handle(), sizeof(cpu_set_t), &cpuset);
#endif
    }

    void stop() {
        if (!m_running.load(std::memory_order_relaxed)) return;
        m_running.store(false, std::memory_order_relaxed);
        if (m_simulatorThread.joinable()) {
            m_simulatorThread.join();
        }
    }

    bool pop(electrodePacket& packetOut) {
        size_t current_tail = m_tail.load(std::memory_order_relaxed);
        size_t current_head = m_head.load(std::memory_order_acquire);

        if (current_tail == current_head) {
            return false;
        }

        packetOut = m_ringBuffer[current_tail];
        m_tail.store((current_tail + 1) % BUFFER_SIZE, std::memory_order_release);
        return true;
    }
};
