// Subtitle files (SRT, WebVTT, ASS/SSA) read into timed lines of plain text.
module;
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

export module wb.text.subtitles;

export namespace wb
{
struct SubtitleCue
{
	double start = 0.0; // seconds
	double end = 0.0;
	std::string text;   // lines separated by '\n'; no styling
};

// The lines of a subtitle file, in the order they start. Empty when the data is no subtitle file. UTF-8 is expected; text
// that is not valid UTF-8 is taken to be Windows-1252, which is what older files tend to be.
[[nodiscard]] std::vector<SubtitleCue> parseSubtitles(std::string_view p_Data);

// A file's text as UTF-8 (a byte order mark dropped, Windows-1252 converted)
[[nodiscard]] std::string subtitleTextToUtf8(std::string_view p_Data);
} // namespace wb
