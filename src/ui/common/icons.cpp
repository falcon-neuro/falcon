#pragma once
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <string_view>
#include "icon_list.cpp"

// icon() searches ALL_ICONS by halving, which finds nothing in a list that isn't sorted.
static_assert(std::ranges::is_sorted(ALL_ICONS, std::ranges::less{}, &Icon::name));

// The text of the icon with this name, to draw with any ImGui text call in any of the UI's fonts.
// It is looked up while compiling: a name that no icon has doesn't compile.
consteval const char* icon(std::string_view name) {
    const auto icons_with_name =
        std::ranges::equal_range(ALL_ICONS, name, std::ranges::less{}, &Icon::name);
    if (icons_with_name.empty()) {
        throw std::invalid_argument("no icon has this name: see src/ui/common/icon_list.cpp");
    }
    return icons_with_name.front().text;
}
