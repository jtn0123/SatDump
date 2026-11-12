#pragma once

/**
 * @file tree.h
 */

#include "core/style.h"
#include "imgui/imgui_soft.h"

namespace satdump
{
    namespace handlers
    {
        /**
         * @brief Handler Tree Drawer
         *
         * Draws the left-panel explorer tree lines.
         */
        struct TreeDrawer
        {
            float SmallOffsetX = 11.0f - 22; // for now, a hardcoded value; should take into account tree indent size

            ImDrawList *drawList = nullptr;

            ImVec2 verticalLineStart = {0, 0};

            ImVec2 verticalLineEnd = {0, 0};

            // Soft look: the connecting lines recede, the icons keep their colour
            static ImU32 lineColor()
            {
                ImU32 c = style::theme.treeview_icon;
                if (!GImSoft.Enabled)
                    return c;
                const ImU32 a = ((c >> IM_COL32_A_SHIFT) & 0xFF) * 35 / 100;
                return (c & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
            }

            void start()
            {
                drawList = ImGui::GetWindowDrawList();

                verticalLineStart = ImGui::GetCursorScreenPos();
                verticalLineStart.x += SmallOffsetX * ui_scale; // to nicely line up with the arrow symbol
                verticalLineEnd = verticalLineStart;
            }

            bool node(std::string icon)
            {
                const float HorizontalTreeLineSize = 8.0f * ui_scale; // chosen arbitrarily
                // Line up with the row just drawn, whatever the theme's row height is
                const float row_min = ImGui::GetItemRectMin().y, row_max = ImGui::GetItemRectMax().y;
                const float minY = row_min + (row_max - row_min - ImGui::GetFontSize()) * 0.5f;
                const float midpoint = (row_min + row_max) * 0.5f;
                drawList->AddLine(ImVec2(verticalLineStart.x, midpoint), ImVec2(verticalLineStart.x + HorizontalTreeLineSize, midpoint), lineColor());
                drawList->AddText(ImVec2(verticalLineStart.x + HorizontalTreeLineSize * 2.0f, minY), style::theme.treeview_icon, icon.c_str());
                verticalLineEnd.y = midpoint;
                return false;
            }

            float end()
            {
                drawList->AddLine(verticalLineStart, verticalLineEnd, lineColor());

                return verticalLineEnd.y - verticalLineStart.y;
            }
        };
    } // namespace handlers
} // namespace satdump