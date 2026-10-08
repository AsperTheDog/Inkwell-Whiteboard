module;
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

module wb.io.file;

namespace wb
{
namespace fs = std::filesystem;

fs::path pathFromUtf8(const std::string_view p_Utf8)
{
	return fs::path(std::u8string(reinterpret_cast<const char8_t*>(p_Utf8.data()), p_Utf8.size()));
}

std::string pathToUtf8(const fs::path& p_Path)
{
	const std::u8string l_Utf8 = p_Path.u8string();
	return std::string(reinterpret_cast<const char*>(l_Utf8.data()), l_Utf8.size());
}

IoResult writeFileAtomic(const fs::path& p_Path, const std::span<const uint8_t> p_Bytes)
{
	std::error_code l_Error;
	if (p_Path.has_parent_path())
		fs::create_directories(p_Path.parent_path(), l_Error);

	fs::path l_Temp = p_Path;
	l_Temp += ".tmp";
	{
		std::ofstream l_File(l_Temp, std::ios::binary | std::ios::trunc);
		if (!l_File)
			return IoResult::failure("Cannot create " + pathToUtf8(l_Temp));
		l_File.write(reinterpret_cast<const char*>(p_Bytes.data()), static_cast<std::streamsize>(p_Bytes.size()));
		l_File.flush();
		if (!l_File)
		{
			l_File.close();
			fs::remove(l_Temp, l_Error);
			return IoResult::failure("Cannot write " + pathToUtf8(p_Path) + " (disk full or no permission?)");
		}
	}

	fs::rename(l_Temp, p_Path, l_Error);
	if (l_Error)
	{
		std::error_code l_Ignored;
		fs::remove(l_Temp, l_Ignored);
		return IoResult::failure("Cannot replace " + pathToUtf8(p_Path) + ": " + l_Error.message());
	}
	return {};
}

IoResult readFile(const fs::path& p_Path, std::vector<uint8_t>& p_Bytes, const uint64_t p_MaxBytes)
{
	p_Bytes.clear();
	std::ifstream l_File(p_Path, std::ios::binary | std::ios::ate);
	if (!l_File)
		return IoResult::failure("Cannot open " + pathToUtf8(p_Path));
	const std::streamoff l_Size = l_File.tellg();
	if (l_Size < 0)
		return IoResult::failure("Cannot read " + pathToUtf8(p_Path));
	if (static_cast<uint64_t>(l_Size) > p_MaxBytes)
		return IoResult::failure(pathToUtf8(p_Path) + " is too large");
	p_Bytes.resize(static_cast<size_t>(l_Size));
	l_File.seekg(0);
	l_File.read(reinterpret_cast<char*>(p_Bytes.data()), l_Size);
	if (!l_File)
	{
		p_Bytes.clear();
		return IoResult::failure("Cannot read " + pathToUtf8(p_Path));
	}
	return {};
}

void removeFileQuiet(const fs::path& p_Path)
{
	std::error_code l_Error;
	fs::remove(p_Path, l_Error);
}
} // namespace wb
