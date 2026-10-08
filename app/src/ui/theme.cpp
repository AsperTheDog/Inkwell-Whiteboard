module;
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

module wb.ui.theme;

import wb.math;

namespace wb::ui
{
namespace
{
constexpr Color rgba(const uint32_t p_Rgba)
{
	return Color::fromRgba8(p_Rgba);
}
} // namespace

Theme lightTheme()
{
	return Theme{
		.canvas = rgba(0xF6F6F3FFu),
		.grid = rgba(0x00000030u),
		.panel = rgba(0xFFFFFFFFu),
		.panelBorder = rgba(0x0000001Au),
		.shadow = rgba(0x1A1D2438u),
		.scrim = rgba(0x1015204Cu),
		.text = rgba(0x1F2328FFu),
		.textMuted = rgba(0x636B75FFu),
		.textFaint = rgba(0xA3A9B0FFu),
		.textOnAccent = rgba(0xFFFFFFFFu),
		.accent = rgba(0x2F6FEDFFu),
		.accentSoft = rgba(0x2F6FED22u),
		.hover = rgba(0x0F172A0Fu),
		.pressed = rgba(0x0F172A1Cu),
		.divider = rgba(0x0F172A1Au),
		.track = rgba(0x0F172A1Fu),
		.danger = rgba(0xD93025FFu),
		.handleFill = rgba(0xFFFFFFFFu),
		.tooltip = rgba(0x23272DF2u),
		.tooltipText = rgba(0xF4F5F7FFu),
	};
}

Theme darkTheme()
{
	return Theme{
		.canvas = rgba(0x1E1F23FFu),
		.grid = rgba(0xFFFFFF20u),
		.panel = rgba(0x2B2D33FFu),
		.panelBorder = rgba(0xFFFFFF1Cu),
		.shadow = rgba(0x00000070u),
		.scrim = rgba(0x000000A0u),
		.text = rgba(0xECEEF1FFu),
		.textMuted = rgba(0xA4AAB3FFu),
		.textFaint = rgba(0x6A707AFFu),
		.textOnAccent = rgba(0x0B1220FFu),
		.accent = rgba(0x6EA6FFFFu),
		.accentSoft = rgba(0x6EA6FF2Eu),
		.hover = rgba(0xFFFFFF14u),
		.pressed = rgba(0xFFFFFF24u),
		.divider = rgba(0xFFFFFF1Fu),
		.track = rgba(0xFFFFFF2Au),
		.danger = rgba(0xFF7266FFu),
		.handleFill = rgba(0x2B2D33FFu),
		.tooltip = rgba(0xECEEF1F2u),
		.tooltipText = rgba(0x1A1C20FFu),
	};
}

Color mixColors(const Color p_A, const Color p_B, const float p_T)
{
	return Color{
		p_A.r + (p_B.r - p_A.r) * p_T,
		p_A.g + (p_B.g - p_A.g) * p_T,
		p_A.b + (p_B.b - p_A.b) * p_T,
		p_A.a + (p_B.a - p_A.a) * p_T,
	};
}

Theme blendThemes(const Theme& p_A, const Theme& p_B, const float p_T)
{
	return Theme{
		.canvas = mixColors(p_A.canvas, p_B.canvas, p_T),
		.grid = mixColors(p_A.grid, p_B.grid, p_T),
		.panel = mixColors(p_A.panel, p_B.panel, p_T),
		.panelBorder = mixColors(p_A.panelBorder, p_B.panelBorder, p_T),
		.shadow = mixColors(p_A.shadow, p_B.shadow, p_T),
		.scrim = mixColors(p_A.scrim, p_B.scrim, p_T),
		.text = mixColors(p_A.text, p_B.text, p_T),
		.textMuted = mixColors(p_A.textMuted, p_B.textMuted, p_T),
		.textFaint = mixColors(p_A.textFaint, p_B.textFaint, p_T),
		.textOnAccent = mixColors(p_A.textOnAccent, p_B.textOnAccent, p_T),
		.accent = mixColors(p_A.accent, p_B.accent, p_T),
		.accentSoft = mixColors(p_A.accentSoft, p_B.accentSoft, p_T),
		.hover = mixColors(p_A.hover, p_B.hover, p_T),
		.pressed = mixColors(p_A.pressed, p_B.pressed, p_T),
		.divider = mixColors(p_A.divider, p_B.divider, p_T),
		.track = mixColors(p_A.track, p_B.track, p_T),
		.danger = mixColors(p_A.danger, p_B.danger, p_T),
		.handleFill = mixColors(p_A.handleFill, p_B.handleFill, p_T),
		.tooltip = mixColors(p_A.tooltip, p_B.tooltip, p_T),
		.tooltipText = mixColors(p_A.tooltipText, p_B.tooltipText, p_T),
	};
}

Hsv rgbToHsv(const Color p_Color)
{
	const float l_Max = std::max({ p_Color.r, p_Color.g, p_Color.b });
	const float l_Min = std::min({ p_Color.r, p_Color.g, p_Color.b });
	const float l_Delta = l_Max - l_Min;
	Hsv l_Result{ .h = 0.f, .s = l_Max > 0.f ? l_Delta / l_Max : 0.f, .v = l_Max };
	if (l_Delta <= 1e-6f)
		return l_Result;
	float l_Hue;
	if (l_Max == p_Color.r)
		l_Hue = std::fmod((p_Color.g - p_Color.b) / l_Delta, 6.f);
	else if (l_Max == p_Color.g)
		l_Hue = (p_Color.b - p_Color.r) / l_Delta + 2.f;
	else
		l_Hue = (p_Color.r - p_Color.g) / l_Delta + 4.f;
	l_Hue /= 6.f;
	l_Result.h = l_Hue < 0.f ? l_Hue + 1.f : l_Hue;
	return l_Result;
}

Color hsvToRgb(const Hsv p_Hsv)
{
	const auto l_Channel = [&](const float p_Offset)
	{
		const float l_K = std::clamp(std::abs(std::fmod(p_Hsv.h + p_Offset, 1.f) * 6.f - 3.f) - 1.f, 0.f, 1.f);
		return p_Hsv.v * (1.f - p_Hsv.s + p_Hsv.s * l_K);
	};
	return Color{ l_Channel(0.f), l_Channel(2.f / 3.f), l_Channel(1.f / 3.f), 1.f };
}

std::string colorToHex(const Color p_Color)
{
	const uint32_t l_Packed = Color{ p_Color.r, p_Color.g, p_Color.b, 1.f }.toRgba8();
	char l_Hex[16];
	std::snprintf(l_Hex, sizeof(l_Hex), "#%02X%02X%02X", (l_Packed >> 24) & 0xFFu, (l_Packed >> 16) & 0xFFu, (l_Packed >> 8) & 0xFFu);
	return l_Hex;
}

std::optional<Color> colorFromHex(std::string_view p_Text)
{
	if (!p_Text.empty() && p_Text.front() == '#')
		p_Text.remove_prefix(1);
	if (p_Text.size() != 6 && p_Text.size() != 3)
		return std::nullopt;
	uint32_t l_Value = 0;
	for (const char l_Char : p_Text)
	{
		const unsigned char l_Byte = static_cast<unsigned char>(l_Char);
		const int l_Digit = std::isdigit(l_Byte) ? l_Char - '0' : (std::isxdigit(l_Byte) ? std::tolower(l_Byte) - 'a' + 10 : -1);
		if (l_Digit < 0)
			return std::nullopt;
		l_Value = l_Value * 16 + static_cast<uint32_t>(l_Digit);
	}
	if (p_Text.size() == 3)
	{
		const uint32_t l_R = (l_Value >> 8) & 0xF, l_G = (l_Value >> 4) & 0xF, l_B = l_Value & 0xF;
		l_Value = (l_R * 17 << 16) | (l_G * 17 << 8) | (l_B * 17);
	}
	return Color::fromRgba8((l_Value << 8) | 0xFFu);
}
} // namespace wb::ui
