// User-visible text. Every string the user can read is looked up by a stable id (`tr("menu.file.new")`) in a
// language file: plain "id=value" lines, one file per language (`en.txt`, `es.txt`...). English is always loaded
// first and the chosen language is laid over it, so a partial translation still works: whatever it lacks shows in
// English. An id that no file defines shows as the id itself, which makes the gap easy to spot.
//
// Values can use "\n", "\t" and "\\", and placeholders "{0}", "{1}"... filled by the arguments of `tr`.
// "{{" and "}}" are literal braces. The key "language.name" is the language's own name for the picker.
module;
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module wb.io.locale;

export namespace wb
{
struct LanguageInfo
{
	std::string code; // file name without extension, e.g. "es"
	std::string name; // "language.name" from the file, e.g. "Español"
};

// Languages found in a directory, sorted by code. Files without "language.name" are listed under their code.
[[nodiscard]] std::vector<LanguageInfo> scanLanguages(const std::filesystem::path& p_Dir);

// Loads English from `p_Dir/en.txt`, then `p_Code` over it. Returns false if English could not be read; an unknown
// or unreadable `p_Code` keeps English. Safe to call while other threads use `tr`.
bool setLanguage(const std::filesystem::path& p_Dir, std::string_view p_Code);

// Code of the language set by the last `setLanguage` ("en" when none was set or it fell back)
[[nodiscard]] std::string currentLanguage();

// The text for an id. The reference stays valid for the lifetime of the program, even across `setLanguage`.
[[nodiscard]] const std::string& tr(std::string_view p_Id);

// Same, with "{0}", "{1}"... replaced by the arguments
[[nodiscard]] std::string tr(std::string_view p_Id, std::initializer_list<std::string_view> p_Args);

// Parses language file text (exposed for tests)
[[nodiscard]] std::vector<std::pair<std::string, std::string>> parseLanguageText(std::string_view p_Text);

// Fills the placeholders of a template
[[nodiscard]] std::string formatText(std::string_view p_Template, std::initializer_list<std::string_view> p_Args);
} // namespace wb
