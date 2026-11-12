#pragma once

/**
 * @file imgui_soft.h
 * @brief Optional "soft" look for the built-in widgets
 *
 * Switched on by a theme (a "Soft" block in the theme file). When off, every
 * widget draws exactly as stock Dear ImGui does. Kept out of imgui.h and
 * ImGuiStyle so that turning it on does not change any struct layout.
 */

#include "imgui.h"

struct ImSoftSkin
{
    bool Enabled = false;

    float RowRounding = 8.0f;   // Hover / selection highlight of list rows, tree rows and menu items
    float CardRounding = 14.0f; // Large content panels
    float CheckSize = 18.0f;    // Checkbox and radio button size

    ImU32 ControlBorder = IM_COL32(0x33, 0x33, 0x33, 0xFF); // Outline of an unchecked checkbox / radio button
    ImU32 OnAccent = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);      // Mark drawn on top of the accent colour
    ImU32 CardBg = IM_COL32(0x10, 0x10, 0x10, 0xFF);
    ImU32 CardBorder = IM_COL32(0x1C, 0x1C, 0x1C, 0xFF);
    ImU32 Muted = IM_COL32(0x81, 0x81, 0x81, 0xFF); // Quiet lines and labels in custom widgets
};

IMGUI_API extern ImSoftSkin GImSoft;
