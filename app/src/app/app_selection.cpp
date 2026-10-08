// App: the selection's on-canvas overlay (box, handles, marquee, lasso) and the floating action bar.
// Temporary ImGui drawing, replaced together with the rest of the UI in the polish milestone.
module;
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <glm/glm.hpp>
#include <imgui.h>

module wb.app;

import wb.doc.commands;
import wb.doc.gizmo;
import wb.editor;
import wb.math;
import wb.tools.select;
import wb.tools.tool;

namespace wb
{
namespace
{
constexpr ImU32 ACCENT = IM_COL32(66, 133, 244, 255);
constexpr ImU32 ACCENT_SOFT = IM_COL32(66, 133, 244, 110);
constexpr ImU32 ACCENT_FILL = IM_COL32(66, 133, 244, 36);
constexpr ImU32 HANDLE_FILL = IM_COL32(255, 255, 255, 255);
constexpr ImDrawFlags CLOSED = ImDrawFlags_Closed;

ImVec2 toImVec2(const Vec2 p_Point)
{
	return ImVec2(p_Point.x, p_Point.y);
}
} // namespace

void App::buildSelectionUi()
{
	const tools::SelectionOverlay l_Overlay = m_Editor.selectionOverlay();
	const float l_Scale = m_Window.displayScale();
	ImDrawList* l_List = ImGui::GetForegroundDrawList();

	if (l_Overlay.marquee)
	{
		l_List->AddRectFilled(toImVec2(l_Overlay.marqueeMin), toImVec2(l_Overlay.marqueeMax), ACCENT_FILL);
		l_List->AddRect(toImVec2(l_Overlay.marqueeMin), toImVec2(l_Overlay.marqueeMax), ACCENT, 0.f, 1.5f * l_Scale);
	}
	if (l_Overlay.lasso.size() >= 2)
	{
		std::vector<ImVec2> l_Points;
		l_Points.reserve(l_Overlay.lasso.size());
		for (const Vec2 l_Point : l_Overlay.lasso)
			l_Points.push_back(toImVec2(l_Point));
		l_List->AddPolyline(l_Points.data(), static_cast<int>(l_Points.size()), ACCENT, 1.5f * l_Scale, CLOSED);
	}
	if (!l_Overlay.visible)
		return;

	for (const auto& [l_Min, l_Max] : l_Overlay.objectBoxes)
		l_List->AddRect(toImVec2(l_Min), toImVec2(l_Max), ACCENT_SOFT, 0.f, 1.f * l_Scale);

	const std::array<ImVec2, 4> l_Corners{ toImVec2(l_Overlay.corners[0]), toImVec2(l_Overlay.corners[1]), toImVec2(l_Overlay.corners[2]), toImVec2(l_Overlay.corners[3]) };
	l_List->AddPolyline(l_Corners.data(), 4, ACCENT, 1.5f * l_Scale, CLOSED);

	if (l_Overlay.interactive)
	{
		const auto l_Emphasis = [&](const Handle p_Handle) { return p_Handle == l_Overlay.active ? 2 : (p_Handle == l_Overlay.hovered ? 1 : 0); };

		l_List->AddLine(toImVec2(l_Overlay.rotateStem), toImVec2(l_Overlay.rotateHandle), ACCENT, 1.5f * l_Scale);
		const int l_RotateEmphasis = l_Emphasis(Handle::Rotate);
		const float l_RotateRadius = (6.f + static_cast<float>(l_RotateEmphasis)) * l_Scale;
		l_List->AddCircleFilled(toImVec2(l_Overlay.rotateHandle), l_RotateRadius, l_RotateEmphasis > 0 ? ACCENT : HANDLE_FILL);
		l_List->AddCircle(toImVec2(l_Overlay.rotateHandle), l_RotateRadius, ACCENT, 0, 1.5f * l_Scale);

		for (int i = 0; i < SCALE_HANDLE_COUNT; ++i)
		{
			if (!l_Overlay.handleShown[static_cast<size_t>(i)])
				continue;
			const int l_HandleEmphasis = l_Emphasis(scaleHandle(i));
			const float l_Half = (4.5f + static_cast<float>(l_HandleEmphasis)) * l_Scale;
			const ImVec2 l_Center = toImVec2(l_Overlay.handles[static_cast<size_t>(i)]);
			const ImVec2 l_Min(l_Center.x - l_Half, l_Center.y - l_Half);
			const ImVec2 l_Max(l_Center.x + l_Half, l_Center.y + l_Half);
			l_List->AddRectFilled(l_Min, l_Max, l_HandleEmphasis > 0 ? ACCENT : HANDLE_FILL, 1.5f * l_Scale);
			l_List->AddRect(l_Min, l_Max, ACCENT, 1.5f * l_Scale, 1.5f * l_Scale);
		}
	}

	// Action bar next to the box (hidden while a gesture is in progress so it never gets in the way)
	if (m_Editor.isBusy())
		return;
	const ImGuiViewport* l_Viewport = ImGui::GetMainViewport();
	const float l_MenuHeight = ImGui::GetFrameHeight();
	const float l_CenterX = std::clamp((l_Overlay.boundsMin.x + l_Overlay.boundsMax.x) * 0.5f, 180.f * l_Scale, std::max(180.f * l_Scale, l_Viewport->Size.x - 180.f * l_Scale));
	const float l_Gap = 10.f * l_Scale;
	const bool l_Above = l_Overlay.boundsMin.y - l_Gap - 44.f * l_Scale > l_MenuHeight;
	const float l_Y = l_Above ? l_Overlay.boundsMin.y - l_Gap : std::min(l_Overlay.boundsMax.y + l_Gap, l_Viewport->Size.y - 120.f * l_Scale);
	ImGui::SetNextWindowPos(ImVec2(l_CenterX, l_Y), ImGuiCond_Always, ImVec2(0.5f, l_Above ? 1.f : 0.f));
	ImGui::SetNextWindowBgAlpha(0.94f);
	constexpr ImGuiWindowFlags l_Flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
	if (!ImGui::Begin("##selectionbar", nullptr, l_Flags))
	{
		ImGui::End();
		return;
	}

	ImGui::TextDisabled("%zu", m_Editor.selection().size());
	ImGui::SameLine();
	if (ImGui::Button("Duplicate"))
		m_Editor.duplicateSelection();
	ImGui::SetItemTooltip("Ctrl+D");
	ImGui::SameLine();
	if (ImGui::Button("Delete"))
		m_Editor.deleteSelection();
	ImGui::SetItemTooltip("Delete");
	ImGui::SameLine();
	if (ImGui::Button("Front"))
		m_Editor.reorderSelection(ZOrderMove::ToFront);
	ImGui::SetItemTooltip("Bring to front (Ctrl+Shift+])");
	ImGui::SameLine();
	if (ImGui::Button("Back"))
		m_Editor.reorderSelection(ZOrderMove::ToBack);
	ImGui::SetItemTooltip("Send to back (Ctrl+Shift+[)");

	const float l_Swatch = 18.f * l_Scale;
	for (size_t i = 0; i < PALETTE.size(); ++i)
	{
		const Color l_Color = Color::fromRgba8(PALETTE[i]);
		ImGui::PushID(static_cast<int>(i));
		if (ImGui::ColorButton("##recolor", ImVec4(l_Color.r, l_Color.g, l_Color.b, 1.f), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(l_Swatch, l_Swatch)))
			m_Editor.recolorSelection(l_Color);
		ImGui::PopID();
		if (i + 1 < PALETTE.size())
			ImGui::SameLine(0.f, 3.f * l_Scale);
	}
	ImGui::SetItemTooltip("Recolour the selected strokes");
	ImGui::SameLine(0.f, 3.f * l_Scale);
	if (ImGui::ColorButton("##recolorcustom", ImVec4(m_CustomColor[0], m_CustomColor[1], m_CustomColor[2], 1.f), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha, ImVec2(l_Swatch, l_Swatch)))
		m_Editor.recolorSelection(Color{ m_CustomColor[0], m_CustomColor[1], m_CustomColor[2], 1.f });
	ImGui::SetItemTooltip("Recolour with your custom colour");
	ImGui::End();
}
} // namespace wb
