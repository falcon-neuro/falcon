#pragma once
#include <imgui.h>

// The colours and the roundings that more than one screen of the UI uses.

inline constexpr ImVec4 WINDOW_COLOUR{0.957f, 0.925f, 0.847f, 1.0f};
inline constexpr ImVec4 TEXT_COLOUR{0.169f, 0.149f, 0.133f, 1.0f};
inline constexpr ImVec4 CARD_COLOUR{1.0f, 0.984f, 0.941f, 1.0f};
inline constexpr ImVec4 CARD_BORDER_COLOUR{0.890f, 0.862f, 0.800f, 1.0f};
inline constexpr ImVec4 HOVERED_CARD_COLOUR{0.902f, 0.957f, 0.937f, 1.0f};
inline constexpr ImVec4 SELECTED_COLOUR{0.043f, 0.420f, 0.439f, 1.0f};
inline constexpr ImVec4 FAINT_TEXT_COLOUR{0.431f, 0.396f, 0.341f, 1.0f};
inline constexpr ImVec4 FAILURE_COLOUR{0.788f, 0.220f, 0.180f, 1.0f};
inline constexpr ImVec4 INPUT_COLOUR{0.929f, 0.890f, 0.784f, 1.0f};
inline constexpr ImVec4 HOVERED_INPUT_COLOUR{0.902f, 0.855f, 0.725f, 1.0f};
inline constexpr ImVec4 ACTIVE_INPUT_COLOUR{0.835f, 0.925f, 0.902f, 1.0f};
// One of the layers a shadow is drawn in: they lie on one another, so it is nearly clear.
inline constexpr ImVec4 SHADOW_LAYER_COLOUR{0.10f, 0.09f, 0.08f, 0.008f};
inline constexpr ImVec4 PANEL_COLOUR{0.890f, 0.851f, 0.757f, 1.0f};
inline constexpr ImVec4 KEY_COLOUR{0.910f, 0.875f, 0.784f, 1.0f};
inline constexpr ImVec4 SIDE_PANEL_COLOUR{0.984f, 0.965f, 0.918f, 1.0f};
inline constexpr float CARD_ROUNDING = 8.0f;
inline constexpr float FRAME_ROUNDING = 6.0f;
