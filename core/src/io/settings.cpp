module;
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <span>
#include <system_error>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module wb.io.settings;

import wb.io.file;

namespace wb
{
namespace
{
std::string_view trim(std::string_view p_Text)
{
	while (!p_Text.empty() && (p_Text.front() == ' ' || p_Text.front() == '\t' || p_Text.front() == '\r'))
		p_Text.remove_prefix(1);
	while (!p_Text.empty() && (p_Text.back() == ' ' || p_Text.back() == '\t' || p_Text.back() == '\r'))
		p_Text.remove_suffix(1);
	return p_Text;
}
} // namespace

void Settings::load(const std::filesystem::path& p_Path)
{
	m_Values.clear();
	std::vector<uint8_t> l_Bytes;
	if (!readFile(p_Path, l_Bytes, 1u << 20).ok)
		return;

	const std::string_view l_Text(reinterpret_cast<const char*>(l_Bytes.data()), l_Bytes.size());
	size_t l_Pos = 0;
	while (l_Pos < l_Text.size())
	{
		size_t l_End = l_Text.find('\n', l_Pos);
		if (l_End == std::string_view::npos)
			l_End = l_Text.size();
		const std::string_view l_Line = trim(l_Text.substr(l_Pos, l_End - l_Pos));
		l_Pos = l_End + 1;

		const size_t l_Equals = l_Line.find('=');
		if (l_Line.empty() || l_Line.front() == '#' || l_Equals == std::string_view::npos)
			continue;
		const std::string_view l_Key = trim(l_Line.substr(0, l_Equals));
		if (!l_Key.empty())
			m_Values[std::string(l_Key)] = std::string(trim(l_Line.substr(l_Equals + 1)));
	}
}

IoResult Settings::save(const std::filesystem::path& p_Path) const
{
	std::string l_Text = "# Inkwell settings\n";
	for (const auto& [l_Key, l_Value] : m_Values)
		l_Text += l_Key + "=" + l_Value + "\n";
	return writeFileAtomic(p_Path, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(l_Text.data()), l_Text.size()));
}

void Settings::set(const std::string_view p_Key, std::string p_Value)
{
	m_Values[std::string(p_Key)] = std::move(p_Value);
}

void Settings::setFloat(const std::string_view p_Key, const float p_Value)
{
	// 9 significant digits round-trip a float exactly
	char l_Buffer[32];
	const auto [l_End, l_Error] = std::to_chars(l_Buffer, l_Buffer + sizeof(l_Buffer), p_Value);
	set(p_Key, l_Error == std::errc{} ? std::string(l_Buffer, l_End) : std::string("0"));
}

void Settings::setInt(const std::string_view p_Key, const long long p_Value)
{
	set(p_Key, std::to_string(p_Value));
}

void Settings::setBool(const std::string_view p_Key, const bool p_Value)
{
	set(p_Key, p_Value ? "1" : "0");
}

std::string Settings::getString(const std::string_view p_Key, const std::string_view p_Default) const
{
	const auto l_It = m_Values.find(p_Key);
	return l_It != m_Values.end() ? l_It->second : std::string(p_Default);
}

float Settings::getFloat(const std::string_view p_Key, const float p_Default) const
{
	const auto l_It = m_Values.find(p_Key);
	if (l_It == m_Values.end())
		return p_Default;
	float l_Value = 0.f;
	const char* l_Begin = l_It->second.data();
	const auto [l_End, l_Error] = std::from_chars(l_Begin, l_Begin + l_It->second.size(), l_Value);
	return l_Error == std::errc{} && std::isfinite(l_Value) ? l_Value : p_Default;
}

long long Settings::getInt(const std::string_view p_Key, const long long p_Default) const
{
	const auto l_It = m_Values.find(p_Key);
	if (l_It == m_Values.end())
		return p_Default;
	long long l_Value = 0;
	const char* l_Begin = l_It->second.data();
	const auto [l_End, l_Error] = std::from_chars(l_Begin, l_Begin + l_It->second.size(), l_Value);
	return l_Error == std::errc{} ? l_Value : p_Default;
}

bool Settings::getBool(const std::string_view p_Key, const bool p_Default) const
{
	return getInt(p_Key, p_Default ? 1 : 0) != 0;
}
} // namespace wb
