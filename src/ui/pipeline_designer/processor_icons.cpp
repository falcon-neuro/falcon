#pragma once
#include <filesystem>
#include <format>
#include <map>
#include "../common/pictures.cpp"
#include "all_processors.cpp"

// The icon of a processor: the picture named after the file its source is in, e.g.
// assets/png/processors/spike_detector.png for processors/spike_detector.cpp. A processor without
// such a file has a picture that isn't loaded.
[[nodiscard]] inline const Picture& icon_of_processor(const ProcessorDescription& processor) {
    static std::map<const ProcessorDescription*, const Picture*> icons;
    const auto found = icons.find(&processor);
    if (found != icons.end()) {
        return *found->second;
    }
    const std::string file_name = std::filesystem::path(processor.file).stem().string();
    const Picture& icon = picture_named(std::format("processors/{}", file_name));
    icons.emplace(&processor, &icon);
    return icon;
}
