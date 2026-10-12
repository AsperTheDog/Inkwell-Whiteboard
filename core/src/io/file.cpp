module;
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

module wb.io.file;

import wb.io.locale;

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

#ifdef _WIN32
namespace
{
// Rename with POSIX semantics: replaces a target that is open elsewhere (the open handles keep seeing the old file)
bool renamePosix(const fs::path& p_From, const fs::path& p_To)
{
	std::error_code l_Error;
	const fs::path l_Target = fs::absolute(p_To, l_Error);
	if (l_Error)
		return false;
	const HANDLE l_File = CreateFileW(p_From.c_str(), DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (l_File == INVALID_HANDLE_VALUE)
		return false;
	const std::wstring l_Name = l_Target.native();
	const size_t l_Bytes = offsetof(FILE_RENAME_INFO, FileName) + (l_Name.size() + 1) * sizeof(wchar_t);
	std::vector<uint8_t> l_Buffer(l_Bytes, 0);
	auto* l_Info = reinterpret_cast<FILE_RENAME_INFO*>(l_Buffer.data());
	l_Info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
	l_Info->RootDirectory = nullptr;
	l_Info->FileNameLength = static_cast<DWORD>(l_Name.size() * sizeof(wchar_t));
	std::memcpy(l_Info->FileName, l_Name.c_str(), (l_Name.size() + 1) * sizeof(wchar_t));
	const BOOL l_Ok = SetFileInformationByHandle(l_File, FileRenameInfoEx, l_Info, static_cast<DWORD>(l_Bytes));
	CloseHandle(l_File);
	return l_Ok != 0;
}
} // namespace
#endif

IoResult replaceFile(const fs::path& p_From, const fs::path& p_To)
{
	std::error_code l_Error;
#ifdef _WIN32
	if (renamePosix(p_From, p_To))
		return {};
#endif
	fs::rename(p_From, p_To, l_Error);
	if (l_Error)
	{
		std::error_code l_Ignored;
		fs::remove(p_From, l_Ignored);
		return IoResult::failure(tr("io.cannotReplace", { pathToUtf8(p_To), l_Error.message() }));
	}
	return {};
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
			return IoResult::failure(tr("io.cannotCreate", { pathToUtf8(l_Temp) }));
		l_File.write(reinterpret_cast<const char*>(p_Bytes.data()), static_cast<std::streamsize>(p_Bytes.size()));
		l_File.flush();
		if (!l_File)
		{
			l_File.close();
			fs::remove(l_Temp, l_Error);
			return IoResult::failure(tr("io.cannotWrite", { pathToUtf8(p_Path) }));
		}
	}

	return replaceFile(l_Temp, p_Path);
}

IoResult readFile(const fs::path& p_Path, std::vector<uint8_t>& p_Bytes, const uint64_t p_MaxBytes)
{
	p_Bytes.clear();
	std::ifstream l_File(p_Path, std::ios::binary | std::ios::ate);
	if (!l_File)
		return IoResult::failure(tr("io.cannotOpen", { pathToUtf8(p_Path) }));
	const std::streamoff l_Size = l_File.tellg();
	if (l_Size < 0)
		return IoResult::failure(tr("io.cannotRead", { pathToUtf8(p_Path) }));
	if (static_cast<uint64_t>(l_Size) > p_MaxBytes)
		return IoResult::failure(tr("io.tooLarge", { pathToUtf8(p_Path) }));
	p_Bytes.resize(static_cast<size_t>(l_Size));
	l_File.seekg(0);
	l_File.read(reinterpret_cast<char*>(p_Bytes.data()), l_Size);
	if (!l_File)
	{
		p_Bytes.clear();
		return IoResult::failure(tr("io.cannotRead", { pathToUtf8(p_Path) }));
	}
	return {};
}

void removeFileQuiet(const fs::path& p_Path)
{
	std::error_code l_Error;
	fs::remove(p_Path, l_Error);
}
} // namespace wb
