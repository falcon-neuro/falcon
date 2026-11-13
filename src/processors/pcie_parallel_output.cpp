#pragma once
#include <sys/io.h>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include "../core/engine.cpp"

// PCIeParallelOutput: an event sets one data pin of a PCIe parallel port card high for the pulse
// width, timed in CPU time so the pulse ends even if events stop, which needs a spinning core.
// Events during a pulse are ignored. It finds the card by its PCI IDs and writes the card's whole
// data byte, so only one may drive a card. Port access needs root or CAP_SYS_RAWIO.

struct PCIeParallelOutputOptions {
    int pin = 5;  // 2 to 9, the data lines D0 to D7
    double pulse_width_microseconds = 1000;
};

template <std::derived_from<Event> EventType, int Capacity, PCIeParallelOutputOptions options>
struct PCIeParallelOutput {
    static_assert(options.pin >= 2 && options.pin <= 9,
                  "a parallel port's data lines are pins 2 to 9");
    static_assert(options.pulse_width_microseconds > 0);

    struct Connections {
        EventInputBuffer<EventType, Capacity> input;
    };

    PCIeParallelOutput()
        : data_register_(find_card_data_register()),
          control_register_(static_cast<unsigned short>(data_register_ + control_register_offset)) {
        std::println("PCIeParallelOutput: found the card at I/O address {:#x}", data_register_);
        if (ioperm(data_register_, register_count, 1) != 0) {
            throw std::runtime_error(std::format(
                "PCIeParallelOutput: no access to I/O port {:#x}; it needs root or CAP_SYS_RAWIO",
                data_register_));
        }
        // Bit 5 of the control register sets the data lines' direction: 0 makes them outputs.
        outb(0, control_register_);
        outb(0, data_register_);
    }

    // Stopping must never leave a stimulator on.
    ~PCIeParallelOutput() {
        outb(0, data_register_);
        ioperm(data_register_, register_count, 0);
    }

    PCIeParallelOutput(const PCIeParallelOutput&) = delete;
    PCIeParallelOutput& operator=(const PCIeParallelOutput&) = delete;
    PCIeParallelOutput(PCIeParallelOutput&&) = delete;
    PCIeParallelOutput& operator=(PCIeParallelOutput&&) = delete;

    void process(Connections connections) {
        if (is_pin_high_ && std::chrono::steady_clock::now() >= pulse_end_) {
            outb(0, data_register_);
            is_pin_high_ = false;
        }
        if (!is_pin_high_ && !connections.input.is_empty()) {
            outb(pin_mask, data_register_);
            is_pin_high_ = true;
            pulse_end_ = std::chrono::steady_clock::now() + pulse_width;
        }
    }

private:
    // The DeLock card's SUNIX chip, as sysfs lists it.
    static constexpr u64 card_vendor_id = 0x1fd4;
    static constexpr u64 card_device_id = 0x1999;
    static constexpr u64 card_subsystem_device_id = 0x0100;
    // The base address register that holds the parallel port's I/O range.
    static constexpr std::size_t parallel_port_bar_index = 1;
    // IORESOURCE_IO among the kernel's resource flags: the range is I/O ports, not memory.
    static constexpr u64 io_resource_flag = 0x100;
    static constexpr u64 highest_io_port = 0xFFFF;
    static constexpr unsigned long register_count = 3;  // data, status, control
    static constexpr int control_register_offset = 2;
    static constexpr auto pin_mask =
        static_cast<unsigned char>(1U << static_cast<unsigned>(options.pin - 2));
    static constexpr auto pulse_width =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double, std::micro>(options.pulse_width_microseconds));

    // Found at start, since the BIOS or the kernel assigns the card's I/O range.
    unsigned short data_register_;
    unsigned short control_register_;
    bool is_pin_high_ = false;
    std::chrono::steady_clock::time_point pulse_end_;

    struct IoRange {
        u64 first_port = 0;
        u64 last_port = 0;
    };

    // Throws unless exactly one card is installed and its parallel port's I/O range holds the
    // three registers: a write to a port with no card behind it can upset other hardware.
    [[nodiscard]] static unsigned short find_card_data_register() {
        const std::filesystem::path pci_devices_directory = "/sys/bus/pci/devices";
        std::error_code listing_error;
        const std::filesystem::directory_iterator pci_devices(pci_devices_directory, listing_error);
        if (listing_error) {
            throw std::runtime_error(std::format("PCIeParallelOutput: can't list {}: {}",
                                                 pci_devices_directory.string(),
                                                 listing_error.message()));
        }

        std::vector<std::filesystem::path> card_directories;
        for (const std::filesystem::directory_entry& pci_device : pci_devices) {
            const std::filesystem::path& device_directory = pci_device.path();
            if (read_hex_file(device_directory / "vendor") == card_vendor_id &&
                read_hex_file(device_directory / "device") == card_device_id &&
                read_hex_file(device_directory / "subsystem_device") == card_subsystem_device_id) {
                card_directories.push_back(device_directory);
            }
        }
        if (card_directories.empty()) {
            throw std::runtime_error(
                "PCIeParallelOutput: found no PCIe parallel port card (a SUNIX chip with vendor "
                "1fd4, device 1999 and subsystem 0100; see lspci -nn)");
        }
        if (card_directories.size() > 1) {
            std::string pci_addresses;
            for (const std::filesystem::path& listed_card_directory : card_directories) {
                if (!pci_addresses.empty()) {
                    pci_addresses += ", ";
                }
                pci_addresses += listed_card_directory.filename().string();
            }
            throw std::runtime_error(
                std::format("PCIeParallelOutput: found {} PCIe parallel port cards ({}) and "
                            "can't tell which one drives the output",
                            card_directories.size(), pci_addresses));
        }

        const std::filesystem::path& card_directory = card_directories.front();
        const std::optional<IoRange> io_range =
            read_io_range(card_directory / "resource", parallel_port_bar_index);
        if (!io_range.has_value()) {
            throw std::runtime_error(std::format(
                "PCIeParallelOutput: the card at PCI address {} has no I/O range in BAR{}",
                card_directory.filename().string(), parallel_port_bar_index));
        }
        if (io_range->last_port > highest_io_port ||
            io_range->last_port < io_range->first_port + register_count - 1) {
            throw std::runtime_error(
                std::format("PCIeParallelOutput: the card's I/O range {:#x}-{:#x} can't hold the "
                            "parallel port's {} registers",
                            io_range->first_port, io_range->last_port, register_count));
        }
        return static_cast<unsigned short>(io_range->first_port);
    }

    // A PCI device's `resource` file in sysfs has one line per base address register: its first
    // port, last port and flags, each in hexadecimal.
    [[nodiscard]] static std::optional<IoRange> read_io_range(
        const std::filesystem::path& resource_path, std::size_t bar_index) {
        std::ifstream resource_file(resource_path);
        std::string line;
        for (std::size_t line_index = 0; std::getline(resource_file, line); ++line_index) {
            if (line_index != bar_index) {
                continue;
            }
            std::istringstream fields(line);
            std::string first_port_text;
            std::string last_port_text;
            std::string flags_text;
            if (!(fields >> first_port_text >> last_port_text >> flags_text)) {
                return std::nullopt;
            }
            const std::optional<u64> first_port = parse_hex(first_port_text);
            const std::optional<u64> last_port = parse_hex(last_port_text);
            const std::optional<u64> flags = parse_hex(flags_text);
            if (!first_port.has_value() || !last_port.has_value() || !flags.has_value() ||
                (*flags & io_resource_flag) == 0 || *first_port == 0) {
                return std::nullopt;
            }
            return IoRange{.first_port = *first_port, .last_port = *last_port};
        }
        return std::nullopt;
    }

    // Reads a sysfs file that holds one number, e.g. a PCI device's `vendor`.
    [[nodiscard]] static std::optional<u64> read_hex_file(const std::filesystem::path& path) {
        std::ifstream file(path);
        std::string text;
        if (!(file >> text)) {
            return std::nullopt;
        }
        return parse_hex(text);
    }

    // sysfs writes these numbers as "0x" followed by hexadecimal digits.
    [[nodiscard]] static std::optional<u64> parse_hex(const std::string& text) {
        constexpr std::string_view prefix = "0x";
        constexpr int hexadecimal_base = 16;
        if (!text.starts_with(prefix)) {
            return std::nullopt;
        }
        const char* const digits_begin = text.c_str() + prefix.size();
        const char* const digits_end = text.c_str() + text.size();
        u64 value = 0;
        const std::from_chars_result result =
            std::from_chars(digits_begin, digits_end, value, hexadecimal_base);
        if (result.ec != std::errc{} || result.ptr != digits_end) {
            return std::nullopt;
        }
        return value;
    }
};
