// User preferences as a plain "key=value" text file. Unknown keys are kept, malformed lines are ignored, so the file
// survives version changes in both directions and can be edited by hand.
module;
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>

export module wb.io.settings;

import wb.io.file;

export namespace wb
{
class Settings
{
public:
	// A missing or unreadable file leaves the settings empty (defaults apply)
	void load(const std::filesystem::path& p_Path);
	[[nodiscard]] IoResult save(const std::filesystem::path& p_Path) const;

	void set(std::string_view p_Key, std::string p_Value);
	void setFloat(std::string_view p_Key, float p_Value);
	void setInt(std::string_view p_Key, long long p_Value);
	void setBool(std::string_view p_Key, bool p_Value);

	[[nodiscard]] std::string getString(std::string_view p_Key, std::string_view p_Default = {}) const;
	[[nodiscard]] float getFloat(std::string_view p_Key, float p_Default) const;
	[[nodiscard]] long long getInt(std::string_view p_Key, long long p_Default) const;
	[[nodiscard]] bool getBool(std::string_view p_Key, bool p_Default) const;

private:
	std::map<std::string, std::string, std::less<>> m_Values;
};
} // namespace wb
