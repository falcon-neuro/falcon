#pragma once
#include <sys/io.h>
#include <chrono>
#include <concepts>
#include <format>
#include <stdexcept>
#include "../core/engine.cpp"

// PCIeParallelOutput: an event sets one data pin of a PCIe parallel port card high for the pulse
// width, timed in CPU time so the pulse ends even if events stop, which needs a spinning core.
// Events during a pulse are ignored. It writes the card's whole data byte, so only one may drive a
// card. Port access needs root or CAP_SYS_RAWIO.

struct PCIeParallelOutputOptions {
    // No default: writing to a port with no card behind it can upset other hardware.
    int port_address = 0;
    int pin = 5;  // 2 to 9, the data lines D0 to D7
    double pulse_width_microseconds = 1000;
};

template <std::derived_from<EventRecord> Record, int Capacity, PCIeParallelOutputOptions options>
struct PCIeParallelOutput {
    static_assert(options.port_address > 0 && options.port_address <= 0xFFFF,
                  "set port_address to the card's I/O address");
    static_assert(options.pin >= 2 && options.pin <= 9,
                  "a parallel port's data lines are pins 2 to 9");
    static_assert(options.pulse_width_microseconds > 0);

    struct Connections {
        EventInputBuffer<Record, Capacity> input;
    };

    PCIeParallelOutput() {
        if (ioperm(data_register, register_count, 1) != 0) {
            throw std::runtime_error(std::format(
                "PCIeParallelOutput: no access to I/O port {:#x}; it needs root or CAP_SYS_RAWIO",
                options.port_address));
        }
        // Bit 5 of the control register sets the data lines' direction: 0 makes them outputs.
        outb(0, control_register);
        outb(0, data_register);
    }

    // Stopping must never leave a stimulator on.
    ~PCIeParallelOutput() {
        outb(0, data_register);
        ioperm(data_register, register_count, 0);
    }

    PCIeParallelOutput(const PCIeParallelOutput&) = delete;
    PCIeParallelOutput& operator=(const PCIeParallelOutput&) = delete;
    PCIeParallelOutput(PCIeParallelOutput&&) = delete;
    PCIeParallelOutput& operator=(PCIeParallelOutput&&) = delete;

    void process(Connections connections) {
        if (is_pin_high_ && std::chrono::steady_clock::now() >= pulse_end_) {
            outb(0, data_register);
            is_pin_high_ = false;
        }
        if (!is_pin_high_ && !connections.input.is_empty()) {
            outb(pin_mask, data_register);
            is_pin_high_ = true;
            pulse_end_ = std::chrono::steady_clock::now() + pulse_width;
        }
    }

private:
    static constexpr auto data_register = static_cast<unsigned short>(options.port_address);
    static constexpr auto control_register = static_cast<unsigned short>(options.port_address + 2);
    static constexpr unsigned long register_count = 3;  // data, status, control
    static constexpr auto pin_mask = static_cast<unsigned char>(1U << (options.pin - 2));
    static constexpr auto pulse_width =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double, std::micro>(options.pulse_width_microseconds));

    bool is_pin_high_ = false;
    std::chrono::steady_clock::time_point pulse_end_;
};
