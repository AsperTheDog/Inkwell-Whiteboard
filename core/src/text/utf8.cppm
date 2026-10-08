// UTF-8 helpers for text editing: decoding, encoding, and moving over characters and words. Strings are UTF-8 with
// '\n' line breaks; positions are byte offsets that always sit on a character boundary.
module;
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

export module wb.text.utf8;

export namespace wb::text
{
inline constexpr uint32_t REPLACEMENT_CHAR = 0xFFFD;

// Decodes the code point at p_Index and advances p_Index past it. Malformed bytes decode as U+FFFD (one byte each).
uint32_t decodeNext(std::string_view p_Text, size_t& p_Index);
void appendCodepoint(std::string& p_Out, uint32_t p_Codepoint);
// Replaces invalid sequences with U+FFFD, drops '\r' (CRLF becomes '\n') and control characters except '\n' and '\t'
[[nodiscard]] std::string sanitize(std::string_view p_Text);

// Characters that attach to the previous one and are edited together with it (combining marks, joiners, selectors)
[[nodiscard]] bool isCombining(uint32_t p_Codepoint);
[[nodiscard]] bool isSpace(uint32_t p_Codepoint);
// Zero-width and direction controls: part of the text, never drawn
[[nodiscard]] bool isFormatting(uint32_t p_Codepoint);
// Ideographs and kana: a line may break between any two of them
[[nodiscard]] bool breaksAnywhere(uint32_t p_Codepoint);

// Start of the character before p_Index / index after the character at p_Index (combining marks included)
[[nodiscard]] size_t previousCluster(std::string_view p_Text, size_t p_Index);
[[nodiscard]] size_t nextCluster(std::string_view p_Text, size_t p_Index);
// Ctrl+Left / Ctrl+Right
[[nodiscard]] size_t previousWord(std::string_view p_Text, size_t p_Index);
[[nodiscard]] size_t nextWord(std::string_view p_Text, size_t p_Index);
// The word (or run of spaces / punctuation) under p_Index, for double-click
void wordRange(std::string_view p_Text, size_t p_Index, size_t& p_Begin, size_t& p_End);
// Number of characters (code points) in p_Text
[[nodiscard]] size_t countCodepoints(std::string_view p_Text);
} // namespace wb::text
