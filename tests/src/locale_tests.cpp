#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <gtest/gtest.h>

import wb.io.locale;

namespace
{
// The program's own English texts: the other test suites read the messages the core produces in English
std::filesystem::path realLanguageDir()
{
	return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "app" / "assets" / "lang";
}

class EnglishForAllTests : public testing::Environment
{
public:
	void SetUp() override { EXPECT_TRUE(wb::setLanguage(realLanguageDir(), "en")); }
};

const auto* const s_English = testing::AddGlobalTestEnvironment(new EnglishForAllTests);

void writeText(const std::filesystem::path& p_Path, const std::string& p_Text)
{
	std::ofstream l_File(p_Path, std::ios::binary);
	l_File << p_Text;
}

// A directory with an English file, a partial Spanish one and a file without a name
struct LanguageDir
{
	std::filesystem::path path = std::filesystem::temp_directory_path() / "wb_locale_test";

	LanguageDir()
	{
		std::filesystem::remove_all(path);
		std::filesystem::create_directories(path);
		writeText(path / "en.txt", "\xEF\xBB\xBF# comment\r\nlanguage.name=English\r\nhello=Hello\r\ngreet=Hi {0}, you have {1} boards\r\nlines=one\\ntwo\\\\three\r\nbye=Bye = see you\r\n");
		writeText(path / "es.txt", "language.name=Espa\xC3\xB1ol\ngreet=Hola {1} tableros, {0}\nbye=Adi\xC3\xB3s\n");
		writeText(path / "xx.txt", "hello=Hullo\n");
	}

	~LanguageDir()
	{
		std::filesystem::remove_all(path);
		(void)wb::setLanguage(realLanguageDir(), "en"); // later tests expect the real English
	}
};
} // namespace

TEST(Locale, ParsesEntriesCommentsAndEscapes)
{
	const auto l_Entries = wb::parseLanguageText("# a comment\n\nkey = value with = sign \nlines=a\\nb\\tc\\\\d\\q\nnoequals\n=novalue\n");
	ASSERT_EQ(l_Entries.size(), 2u); // a line without "=" and one with an empty key are ignored
	EXPECT_EQ(l_Entries[0].first, "key");
	EXPECT_EQ(l_Entries[0].second, "value with = sign");
	EXPECT_EQ(l_Entries[1].second, "a\nb\tc\\d\\q"); // an unknown escape stays as written
}

TEST(Locale, FormatsPlaceholdersAndBraces)
{
	EXPECT_EQ(wb::formatText("{0} and {1}", { "a", "b" }), "a and b");
	EXPECT_EQ(wb::formatText("{1} before {0}", { "a", "b" }), "b before a");
	EXPECT_EQ(wb::formatText("{{0}} is literal, {0} is not", { "x" }), "{0} is literal, x is not");
	EXPECT_EQ(wb::formatText("{2} has no argument, {x} is no placeholder", { "a" }), "{2} has no argument, {x} is no placeholder");
	EXPECT_EQ(wb::formatText("no placeholders", {}), "no placeholders");
}

TEST(Locale, EnglishIsTheBaseAndALanguageIsLaidOverIt)
{
	const LanguageDir l_Dir;
	ASSERT_TRUE(wb::setLanguage(l_Dir.path, "es"));
	EXPECT_EQ(wb::currentLanguage(), "es");
	EXPECT_EQ(wb::tr("hello"), "Hello"); // not translated: shows in English
	EXPECT_EQ(wb::tr("bye"), "Adi\xC3\xB3s");
	EXPECT_EQ(wb::tr("greet", { "Ana", "3" }), "Hola 3 tableros, Ana"); // the translation reorders the arguments
	EXPECT_EQ(wb::tr("lines"), "one\ntwo\\three");

	ASSERT_TRUE(wb::setLanguage(l_Dir.path, "en"));
	EXPECT_EQ(wb::currentLanguage(), "en");
	EXPECT_EQ(wb::tr("bye"), "Bye = see you");
	EXPECT_EQ(wb::tr("greet", { "Ana", "3" }), "Hi Ana, you have 3 boards");
}

TEST(Locale, UnknownIdsAndLanguagesAreHarmless)
{
	const LanguageDir l_Dir;
	ASSERT_TRUE(wb::setLanguage(l_Dir.path, "does-not-exist"));
	EXPECT_EQ(wb::currentLanguage(), "en");
	EXPECT_EQ(wb::tr("nothing.here"), "nothing.here"); // the id itself makes the gap easy to spot

	ASSERT_TRUE(wb::setLanguage(l_Dir.path, "../en")); // never leaves the language directory
	EXPECT_EQ(wb::currentLanguage(), "en");
}

TEST(Locale, ReferencesStayValidAcrossLanguageChanges)
{
	const LanguageDir l_Dir;
	ASSERT_TRUE(wb::setLanguage(l_Dir.path, "en"));
	const std::string& l_Before = wb::tr("bye");
	ASSERT_TRUE(wb::setLanguage(l_Dir.path, "es"));
	EXPECT_EQ(l_Before, "Bye = see you"); // the old text is still there for whoever holds it
	EXPECT_EQ(wb::tr("bye"), "Adi\xC3\xB3s");
}

TEST(Locale, ListsTheLanguagesInADirectory)
{
	const LanguageDir l_Dir;
	const auto l_Languages = wb::scanLanguages(l_Dir.path);
	ASSERT_EQ(l_Languages.size(), 3u);
	EXPECT_EQ(l_Languages[0].code, "en");
	EXPECT_EQ(l_Languages[0].name, "English");
	EXPECT_EQ(l_Languages[1].code, "es");
	EXPECT_EQ(l_Languages[1].name, "Espa\xC3\xB1ol");
	EXPECT_EQ(l_Languages[2].code, "xx");
	EXPECT_EQ(l_Languages[2].name, "xx"); // no language.name: listed under its code
}

TEST(Locale, TheProgramsEnglishFileLoads)
{
	ASSERT_TRUE(wb::setLanguage(realLanguageDir(), "en"));
	EXPECT_EQ(wb::tr("language.name"), "English");
	EXPECT_EQ(wb::tr("menu.new"), "New board");
	EXPECT_EQ(wb::tr("dialog.prompt.body", { "Plan.wbrd" }), "\"Plan.wbrd\" has changes that are not saved.");
}

// A translation may reorder placeholders or leave one out, but must not invent one, or the program would show "{1}" to a user
TEST(Locale, EveryShippedLanguageKeepsTheEnglishPlaceholders)
{
	const auto l_Placeholders = [](const std::string& p_Text)
	{
		std::string l_Found;
		for (size_t i = 0; i + 2 < p_Text.size(); ++i)
		{
			if (p_Text[i] == '{' && p_Text[i + 1] >= '0' && p_Text[i + 1] <= '9' && p_Text[i + 2] == '}')
				l_Found += p_Text[i + 1];
		}
		std::ranges::sort(l_Found);
		return l_Found;
	};
	const auto l_Read = [](const std::filesystem::path& p_Path)
	{
		std::ifstream l_File(p_Path, std::ios::binary);
		const std::string l_Text((std::istreambuf_iterator<char>(l_File)), std::istreambuf_iterator<char>());
		return wb::parseLanguageText(l_Text);
	};
	std::map<std::string, std::string> l_English;
	for (const auto& [l_Key, l_Value] : l_Read(realLanguageDir() / "en.txt"))
		l_English[l_Key] = l_Value;
	ASSERT_GT(l_English.size(), 100u);

	for (const wb::LanguageInfo& l_Language : wb::scanLanguages(realLanguageDir()))
	{
		for (const auto& [l_Key, l_Value] : l_Read(realLanguageDir() / (l_Language.code + ".txt")))
		{
			const auto l_It = l_English.find(l_Key);
			ASSERT_NE(l_It, l_English.end()) << l_Language.code << ": " << l_Key << " does not exist in English";
			EXPECT_TRUE(std::ranges::includes(l_Placeholders(l_It->second), l_Placeholders(l_Value))) << l_Language.code << ": " << l_Key; // leaving one out is fine
		}
	}
}
