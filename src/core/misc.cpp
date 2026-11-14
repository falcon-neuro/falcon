#pragma once
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <memory>
#include <string_view>
#include <system_error>
#include "engine.cpp"

// Checks of the machine's setup that processors run when they are built.

// Built whole and written in one call, so the line reaches the terminal in one piece: processors
// are built on their cores at the same time, and their output would interleave. A line too long
// for the buffer is cut, but keeps its newline.
class LogLine {
public:
    void append(std::string_view text) noexcept {
        const std::size_t room = characters_.size() - 1 - length_;  // 1 byte for the newline
        const std::size_t count = std::min(text.size(), room);
        std::copy_n(text.begin(), count, characters_.data() + length_);
        length_ += count;
    }

    void append_number(u64 number) noexcept {
        std::array<char, 20> digits{};  // u64 has at most 20 digits
        const std::to_chars_result result =
            std::to_chars(digits.data(), digits.data() + digits.size(), number);
        if (result.ec != std::errc{}) {
            return;
        }
        append(std::string_view(digits.data(), result.ptr));
    }

    void print() noexcept {
        characters_[length_] = '\n';
        std::fwrite(characters_.data(), 1, length_ + 1, stderr);
    }

private:
    std::array<char, 1024> characters_{};
    std::size_t length_ = 0;  // without the newline print() adds
};

inline constexpr std::size_t TEXT_LINE_CAPACITY = 512;  // bytes, the terminating zero included
using TextLine = std::array<char, TEXT_LINE_CAPACITY>;

struct FileCloser {
    void operator()(std::FILE* file) const noexcept { std::fclose(file); }
};
using OpenFile = std::unique_ptr<std::FILE, FileCloser>;

// A line too long for the buffer keeps only its start, which is the part the callers recognise a
// line by.
bool read_text_line(std::FILE* file, TextLine& buffer, std::string_view& line) noexcept {
    if (std::fgets(buffer.data(), static_cast<int>(buffer.size()), file) == nullptr) {
        return false;
    }
    line = buffer.data();
    if (line.ends_with('\n')) {
        line.remove_suffix(1);
        return true;
    }
    int character = std::fgetc(file);
    while (character != EOF && character != '\n') {
        character = std::fgetc(file);
    }
    return true;
}

void copy_into_text_line(std::string_view text, TextLine& destination) noexcept {
    const std::size_t length = std::min(text.size(), destination.size() - 1);
    std::copy_n(text.begin(), length, destination.begin());
    destination[length] = '\0';
}

bool parse_four_hex_digits(std::string_view text, std::size_t position,
                           std::uint16_t& value) noexcept {
    constexpr std::size_t digit_count = 4;
    if (position > text.size() || text.size() - position < digit_count) {
        return false;
    }
    const char* const digits_begin = text.data() + position;
    const char* const digits_end = digits_begin + digit_count;
    const std::from_chars_result result = std::from_chars(digits_begin, digits_end, value, 16);
    return result.ec == std::errc{} && result.ptr == digits_end;
}

struct PciId {
    std::uint16_t vendor = 0;
    std::uint16_t device = 0;
};

// Fails for an interface that isn't a PCI card: loopback and bridges have no device, USB adapters
// no PCI_ID line.
bool read_pci_id(std::string_view interface_name, PciId& pci_id) noexcept {
    constexpr std::size_t name_capacity = IFNAMSIZ;  // the terminating zero included
    constexpr std::string_view directory = "/sys/class/net/";
    constexpr std::string_view file_name = "/device/uevent";
    if (interface_name.size() >= name_capacity) {
        return false;
    }
    std::array<char, directory.size() + name_capacity + file_name.size()> path{};
    char* path_end = std::ranges::copy(directory, path.data()).out;
    path_end = std::ranges::copy(interface_name, path_end).out;
    std::ranges::copy(file_name, path_end);

    const OpenFile file{std::fopen(path.data(), "r")};
    if (file == nullptr) {
        return false;
    }
    constexpr std::string_view key = "PCI_ID=";
    TextLine buffer{};
    std::string_view line;
    while (read_text_line(file.get(), buffer, line)) {
        if (line.starts_with(key)) {
            line.remove_prefix(key.size());
            // VVVV:DDDD
            return line.size() == 9 && line[4] == ':' &&
                   parse_four_hex_digits(line, 0, pci_id.vendor) &&
                   parse_four_hex_digits(line, 5, pci_id.device);
        }
    }
    return false;
}

// Splits a pci.ids entry "hhhh  Name" into its ID and its name.
bool parse_pci_ids_entry(std::string_view entry, std::uint16_t& entry_id,
                         std::string_view& entry_name) noexcept {
    if (entry.size() <= 4 || entry[4] != ' ' || !parse_four_hex_digits(entry, 0, entry_id)) {
        return false;
    }
    entry_name = entry;
    entry_name.remove_prefix(4);
    const std::size_t name_start = entry_name.find_first_not_of(' ');
    if (name_start == std::string_view::npos) {
        return false;
    }
    entry_name.remove_prefix(name_start);
    return true;
}

struct NetworkCardModel {
    TextLine vendor{};  // empty if pci.ids doesn't list the vendor
    TextLine device{};  // empty if pci.ids doesn't list the device
};

// pci.ids lists each vendor as "vvvv  Vendor name", followed by its devices as "\tdddd  Device
// name", with subsystems ("\t\t...") and comments ("#...") among them. The device classes after
// the last vendor start with "C ", which no vendor ID parses as.
NetworkCardModel look_up_network_card_model(const PciId& pci_id) noexcept {
    NetworkCardModel model;
    const OpenFile file{std::fopen("/usr/share/misc/pci.ids", "r")};
    if (file == nullptr) {
        return model;
    }
    TextLine buffer{};
    std::string_view line;
    bool is_in_vendor_block = false;
    while (read_text_line(file.get(), buffer, line)) {
        if (line.empty() || line.starts_with('#') || line.starts_with("\t\t")) {
            continue;
        }
        std::uint16_t entry_id = 0;
        std::string_view entry_name;
        if (line.starts_with('\t')) {
            line.remove_prefix(1);
            if (is_in_vendor_block && parse_pci_ids_entry(line, entry_id, entry_name) &&
                entry_id == pci_id.device) {
                copy_into_text_line(entry_name, model.device);
                return model;
            }
        } else if (is_in_vendor_block) {
            return model;  // the next vendor: the device isn't listed
        } else if (parse_pci_ids_entry(line, entry_id, entry_name) && entry_id == pci_id.vendor) {
            copy_into_text_line(entry_name, model.vendor);
            is_in_vendor_block = true;
        }
    }
    return model;
}

// Appends " (Vendor Device)" after an interface name, or nothing for a card pci.ids doesn't know.
void append_network_card_model(LogLine& log_line, std::string_view interface_name) noexcept {
    PciId pci_id;
    if (!read_pci_id(interface_name, pci_id)) {
        return;
    }
    const NetworkCardModel model = look_up_network_card_model(pci_id);
    if (model.vendor.front() == '\0') {
        return;
    }
    log_line.append(" (");
    log_line.append(model.vendor.data());
    if (model.device.front() != '\0') {
        log_line.append(" ");
        log_line.append(model.device.data());
    }
    log_line.append(")");
}

void append_receive_batching_settings(LogLine& log_line,
                                      const ethtool_coalesce& coalescing) noexcept {
    log_line.append("rx-usecs ");
    log_line.append_number(coalescing.rx_coalesce_usecs);
    log_line.append(", rx-frames ");
    log_line.append_number(coalescing.rx_max_coalesced_frames);
    log_line.append(coalescing.use_adaptive_rx_coalesce != 0 ? ", adaptive-rx on"
                                                             : ", adaptive-rx off");
}

// A network card that batches its receive interrupts holds packets back for up to its batching
// time, so samples reach Falcon late by a varying amount. Every card is checked, since which one
// receives the acquisition data isn't known here.
void warn_about_interrupt_batching() noexcept {
    const int socket_descriptor = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_descriptor < 0) {
        std::fputs("warning: can't check the network cards for interrupt batching\n", stderr);
        return;
    }
    auto* const network_interfaces = if_nameindex();
    if (network_interfaces == nullptr) {
        std::fputs("warning: can't check the network cards for interrupt batching\n", stderr);
        close(socket_descriptor);
        return;
    }
    for (const auto* network_interface = network_interfaces; network_interface->if_index != 0;
         ++network_interface) {
        const std::string_view name = network_interface->if_name;
        ifreq request{};
        if (name.size() >= std::size(request.ifr_name)) {
            continue;
        }
        std::ranges::copy(name, std::begin(request.ifr_name));
        ethtool_coalesce coalescing{};
        coalescing.cmd = ETHTOOL_GCOALESCE;
        request.ifr_data = reinterpret_cast<char*>(&coalescing);
        // Fails on interfaces without batching settings, e.g. loopback or most wifi cards.
        if (ioctl(socket_descriptor, SIOCETHTOOL, &request) != 0) {
            continue;
        }
        // rx-frames 1 raises an interrupt for every packet, which is no batching.
        const bool is_batching = coalescing.rx_coalesce_usecs > 0 ||
                                 coalescing.rx_max_coalesced_frames > 1 ||
                                 coalescing.use_adaptive_rx_coalesce != 0;
        LogLine log_line;
        if (is_batching) {
            log_line.append("warning: network card ");
            log_line.append(name);
            append_network_card_model(log_line, name);
            log_line.append(" waits to collect several packets before passing them to the CPU, "
                            "so data reaches Falcon late by a varying amount (receive interrupt "
                            "batching on: ");
            append_receive_batching_settings(log_line, coalescing);
            log_line.append("; turn it off with ethtool -C)");
        } else {
            log_line.append("network card ");
            log_line.append(name);
            append_network_card_model(log_line, name);
            log_line.append(" passes each packet to the CPU immediately, without waiting (receive "
                            "interrupt batching off: ");
            append_receive_batching_settings(log_line, coalescing);
            log_line.append(")");
        }
        log_line.print();
    }
    if_freenameindex(network_interfaces);
    close(socket_descriptor);
}
