#pragma once
#include <imgui.h>

// The colours and the roundings that more than one screen of the UI uses.

inline constexpr ImVec4 CARD_COLOUR{1.0f, 1.0f, 1.0f, 1.0f};
inline constexpr ImVec4 CARD_BORDER_COLOUR{0.84f, 0.84f, 0.86f, 1.0f};
inline constexpr ImVec4 HOVERED_CARD_COLOUR{0.93f, 0.94f, 0.96f, 1.0f};
inline constexpr ImVec4 SELECTED_COLOUR{0.12f, 0.31f, 0.63f, 1.0f};
inline constexpr ImVec4 FAINT_TEXT_COLOUR{0.45f, 0.45f, 0.45f, 1.0f};
inline constexpr ImVec4 FAILURE_COLOUR{0.80f, 0.22f, 0.20f, 1.0f};
inline constexpr ImVec4 INPUT_COLOUR{0.92f, 0.92f, 0.94f, 1.0f};
inline constexpr ImVec4 HOVERED_INPUT_COLOUR{0.88f, 0.88f, 0.91f, 1.0f};
inline constexpr ImVec4 ACTIVE_INPUT_COLOUR{0.84f, 0.86f, 0.92f, 1.0f};
inline constexpr float CARD_ROUNDING = 8.0f;
