// Colours of the user interface. Two palettes (light and dark) that can be blended, so switching themes fades.
module;

export module wb.ui.theme;

import wb.math;

export namespace wb::ui
{
struct Theme
{
	// The board
	Color canvas;
	Color grid;

	// Floating panels
	Color panel;
	Color panelBorder;
	Color shadow;
	Color scrim; // behind modal dialogs

	// Text
	Color text;
	Color textMuted;
	Color textFaint;
	Color textOnAccent;

	// Interaction
	Color accent;
	Color accentSoft; // selected button background
	Color hover;      // overlay on hovered controls
	Color pressed;    // overlay on pressed controls
	Color divider;
	Color track;      // slider / switch background
	Color danger;
	Color handleFill; // selection handles

	Color tooltip;
	Color tooltipText;
};

[[nodiscard]] Theme lightTheme();
[[nodiscard]] Theme darkTheme();
// t = 0 gives p_A, t = 1 gives p_B
[[nodiscard]] Theme blendThemes(const Theme& p_A, const Theme& p_B, float p_T);

// Straight-alpha colour helpers
[[nodiscard]] Color mixColors(Color p_A, Color p_B, float p_T);
[[nodiscard]] constexpr Color withAlpha(const Color p_Color, const float p_Alpha)
{
	return Color{ p_Color.r, p_Color.g, p_Color.b, p_Alpha };
}

// Hue 0..1, saturation 0..1, value 0..1
struct Hsv
{
	float h = 0.f;
	float s = 0.f;
	float v = 0.f;
};
[[nodiscard]] Hsv rgbToHsv(Color p_Color);
[[nodiscard]] Color hsvToRgb(Hsv p_Hsv);
} // namespace wb::ui
