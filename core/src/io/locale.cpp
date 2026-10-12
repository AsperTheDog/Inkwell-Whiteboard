module;
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

module wb.io.locale;

namespace wb
{
namespace
{
struct StringHash
{
	using is_transparent = void;
	size_t operator()(const std::string_view p_Text) const noexcept { return std::hash<std::string_view>{}(p_Text); }
};

using Table = std::unordered_map<std::string, std::string, StringHash, std::equal_to<>>;

// Tables are never freed: `tr` hands out references into them and may be called from any thread while the
// language changes. A language switch is rare and a table is a few tens of kilobytes.
std::mutex g_Mutex;
std::vector<std::unique_ptr<Table>> g_Tables;
std::atomic<const Table*> g_Active{ nullptr };
std::string g_Code = "en";
std::set<std::string, std::less<>> g_Unknown; // ids nobody defines; node-based so references stay put

std::string_view trim(std::string_view p_Text)
{
	while (!p_Text.empty() && (p_Text.front() == ' ' || p_Text.front() == '\t' || p_Text.front() == '\r'))
		p_Text.remove_prefix(1);
	while (!p_Text.empty() && (p_Text.back() == ' ' || p_Text.back() == '\t' || p_Text.back() == '\r'))
		p_Text.remove_suffix(1);
	return p_Text;
}

std::string unescape(const std::string_view p_Text)
{
	std::string l_Out;
	l_Out.reserve(p_Text.size());
	for (size_t i = 0; i < p_Text.size(); ++i)
	{
		if (p_Text[i] == '\\' && i + 1 < p_Text.size())
		{
			const char l_Next = p_Text[i + 1];
			if (l_Next == 'n' || l_Next == 't' || l_Next == '\\')
			{
				l_Out += l_Next == 'n' ? '\n' : l_Next == 't' ? '\t' : '\\';
				++i;
				continue;
			}
		}
		l_Out += p_Text[i];
	}
	return l_Out;
}

bool readText(const std::filesystem::path& p_Path, std::string& p_Out)
{
	// Plain streams: this module sits below wb.io.file, whose own errors are translated
	std::ifstream l_File(p_Path, std::ios::binary);
	if (!l_File)
		return false;
	constexpr std::streamsize MAX_BYTES = 4 << 20;
	std::string l_Text(static_cast<size_t>(MAX_BYTES), '\0');
	l_File.read(l_Text.data(), MAX_BYTES);
	l_Text.resize(static_cast<size_t>(l_File.gcount()));
	if (l_Text.starts_with("\xEF\xBB\xBF")) // byte order mark from editors on Windows
		l_Text.erase(0, 3);
	p_Out = std::move(l_Text);
	return true;
}

bool loadInto(Table& p_Table, const std::filesystem::path& p_Path)
{
	std::string l_Text;
	if (!readText(p_Path, l_Text))
		return false;
	for (auto& [l_Key, l_Value] : parseLanguageText(l_Text))
		p_Table[std::move(l_Key)] = std::move(l_Value);
	return true;
}
} // namespace

std::vector<std::pair<std::string, std::string>> parseLanguageText(const std::string_view p_Text)
{
	std::vector<std::pair<std::string, std::string>> l_Entries;
	size_t l_Pos = 0;
	while (l_Pos < p_Text.size())
	{
		size_t l_End = p_Text.find('\n', l_Pos);
		if (l_End == std::string_view::npos)
			l_End = p_Text.size();
		const std::string_view l_Line = trim(p_Text.substr(l_Pos, l_End - l_Pos));
		l_Pos = l_End + 1;

		const size_t l_Equals = l_Line.find('=');
		if (l_Line.empty() || l_Line.front() == '#' || l_Equals == std::string_view::npos)
			continue;
		const std::string_view l_Key = trim(l_Line.substr(0, l_Equals));
		if (!l_Key.empty())
			l_Entries.emplace_back(std::string(l_Key), unescape(trim(l_Line.substr(l_Equals + 1))));
	}
	return l_Entries;
}

std::vector<LanguageInfo> scanLanguages(const std::filesystem::path& p_Dir)
{
	std::vector<LanguageInfo> l_Languages;
	std::error_code l_Error;
	for (const auto& l_Entry : std::filesystem::directory_iterator(p_Dir, l_Error))
	{
		if (!l_Entry.is_regular_file(l_Error) || l_Entry.path().extension() != ".txt")
			continue;
		LanguageInfo l_Info;
		l_Info.code = l_Entry.path().stem().string();
		l_Info.name = l_Info.code;
		std::string l_Text;
		if (readText(l_Entry.path(), l_Text))
		{
			for (const auto& [l_Key, l_Value] : parseLanguageText(l_Text))
			{
				if (l_Key == "language.name" && !l_Value.empty())
					l_Info.name = l_Value;
			}
		}
		l_Languages.push_back(std::move(l_Info));
	}
	std::ranges::sort(l_Languages, {}, &LanguageInfo::code);
	return l_Languages;
}

bool setLanguage(const std::filesystem::path& p_Dir, const std::string_view p_Code)
{
	auto l_Table = std::make_unique<Table>();
	const bool l_English = loadInto(*l_Table, p_Dir / "en.txt");
	std::string l_Code = "en";
	// Only plain file stems, so a setting can never point outside the language directory
	const bool l_Plain = !p_Code.empty() && p_Code != "en" && p_Code.find_first_of("/\\.") == std::string_view::npos;
	if (l_Plain && loadInto(*l_Table, p_Dir / (std::string(p_Code) + ".txt")))
		l_Code = std::string(p_Code);

	std::scoped_lock l_Lock(g_Mutex);
	g_Active.store(l_Table.get());
	g_Tables.push_back(std::move(l_Table));
	g_Code = std::move(l_Code);
	return l_English;
}

std::string currentLanguage()
{
	std::scoped_lock l_Lock(g_Mutex);
	return g_Code;
}

const std::string& tr(const std::string_view p_Id)
{
	if (const Table* l_Table = g_Active.load())
	{
		const auto l_It = l_Table->find(p_Id);
		if (l_It != l_Table->end())
			return l_It->second;
	}
	std::scoped_lock l_Lock(g_Mutex);
	return *g_Unknown.emplace(p_Id).first;
}

std::string formatText(const std::string_view p_Template, const std::initializer_list<std::string_view> p_Args)
{
	std::string l_Out;
	l_Out.reserve(p_Template.size() + 16);
	for (size_t i = 0; i < p_Template.size(); ++i)
	{
		const char l_Char = p_Template[i];
		if ((l_Char == '{' || l_Char == '}') && i + 1 < p_Template.size() && p_Template[i + 1] == l_Char)
		{
			l_Out += l_Char;
			++i;
		}
		else if (l_Char == '{')
		{
			const size_t l_Close = p_Template.find('}', i);
			size_t l_Index = 0;
			bool l_Valid = l_Close != std::string_view::npos && l_Close > i + 1;
			for (size_t j = i + 1; l_Valid && j < l_Close; ++j)
			{
				l_Valid = p_Template[j] >= '0' && p_Template[j] <= '9';
				l_Index = l_Index * 10 + static_cast<size_t>(p_Template[j] - '0');
			}
			if (l_Valid && l_Index < p_Args.size())
			{
				l_Out += *(p_Args.begin() + static_cast<std::ptrdiff_t>(l_Index));
				i = l_Close;
			}
			else
			{
				l_Out += l_Char; // not a placeholder we can fill: keep it visible
			}
		}
		else
		{
			l_Out += l_Char;
		}
	}
	return l_Out;
}

std::string tr(const std::string_view p_Id, const std::initializer_list<std::string_view> p_Args)
{
	return formatText(tr(p_Id), p_Args);
}
} // namespace wb
