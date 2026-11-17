#pragma once
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Which cores the machine has and which of them are isolated from the OS, as the kernel says.

// The core numbers in a list as the kernel writes it, e.g. "0-3,8". Empty for an empty list or
// one that can't be read as such.
[[nodiscard]] inline std::vector<int> parse_core_list(std::string_view text) {
    std::vector<int> core_numbers;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        std::string_view range = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        while (!range.empty() && (range.back() == '\n' || range.back() == ' ')) {
            range.remove_suffix(1);
        }
        if (range.empty()) {
            continue;
        }
        const std::size_t dash = range.find('-');
        const std::string_view first_text = range.substr(0, dash);
        const std::string_view last_text =
            dash == std::string_view::npos ? first_text : range.substr(dash + 1);
        int first = 0;
        int last = 0;
        const bool is_first_read =
            std::from_chars(first_text.data(), first_text.data() + first_text.size(), first).ec ==
            std::errc{};
        const bool is_last_read =
            std::from_chars(last_text.data(), last_text.data() + last_text.size(), last).ec ==
            std::errc{};
        if (!is_first_read || !is_last_read || first < 0 || last < first) {
            return {};
        }
        for (int core_number = first; core_number <= last; ++core_number) {
            core_numbers.push_back(core_number);
        }
    }
    return core_numbers;
}

// The reverse, for showing a list: {0, 1, 2, 3, 8} is "0-3, 8".
[[nodiscard]] inline std::string core_list_text(const std::vector<int>& core_numbers) {
    std::string text;
    for (std::size_t first = 0; first < core_numbers.size();) {
        std::size_t last = first;
        while (last + 1 < core_numbers.size() &&
               core_numbers.at(last + 1) == core_numbers.at(last) + 1) {
            ++last;
        }
        if (!text.empty()) {
            text += ", ";
        }
        text += std::to_string(core_numbers.at(first));
        if (last > first) {
            text += "-" + std::to_string(core_numbers.at(last));
        }
        first = last + 1;
    }
    return text;
}

[[nodiscard]] inline std::vector<int> read_core_list(const char* path) {
    std::ifstream file(path);
    std::string text;
    std::getline(file, text);
    return parse_core_list(text);
}

class CpuCores {
public:
    // Not from std::thread::hardware_concurrency() alone: that counts the cores this program may
    // run on, which leaves out the isolated ones.
    CpuCores()
        : present_(read_core_list("/sys/devices/system/cpu/present")),
          isolated_(read_core_list("/sys/devices/system/cpu/isolated")) {
        if (present_.empty()) {
            const int count = static_cast<int>(std::max(1U, std::thread::hardware_concurrency()));
            for (int core_number = 0; core_number < count; ++core_number) {
                present_.push_back(core_number);
            }
        }
        // Isolated cores first, each group by number: the order in which cores are offered.
        offered_ = present_;
        std::ranges::stable_sort(offered_, [this](int first, int second) {
            return is_isolated(first) && !is_isolated(second);
        });
    }

    [[nodiscard]] bool is_isolated(int core_number) const {
        return std::ranges::contains(isolated_, core_number);
    }

    // Every core, the isolated ones first.
    [[nodiscard]] const std::vector<int>& offered() const { return offered_; }

    [[nodiscard]] std::vector<int> isolated() const {
        std::vector<int> core_numbers;
        std::ranges::copy_if(present_, std::back_inserter(core_numbers),
                             [this](int core_number) { return is_isolated(core_number); });
        return core_numbers;
    }

    [[nodiscard]] std::vector<int> not_isolated() const {
        std::vector<int> core_numbers;
        std::ranges::copy_if(present_, std::back_inserter(core_numbers),
                             [this](int core_number) { return !is_isolated(core_number); });
        return core_numbers;
    }

private:
    std::vector<int> present_;
    std::vector<int> isolated_;
    std::vector<int> offered_;
};
