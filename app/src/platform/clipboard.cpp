module;
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <SDL3/SDL.h>
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

module wb.platform.clipboard;

namespace wb::platform
{
namespace
{
bool readFile(const std::filesystem::path& p_Path, const uint64_t p_MaxBytes, std::vector<uint8_t>& p_Bytes)
{
	std::error_code l_Error;
	if (!std::filesystem::is_regular_file(p_Path, l_Error))
		return false;
	std::ifstream l_File(p_Path, std::ios::binary | std::ios::ate);
	if (!l_File)
		return false;
	const std::streamsize l_Size = l_File.tellg();
	if (l_Size <= 0 || static_cast<uint64_t>(l_Size) > p_MaxBytes)
		return false;
	p_Bytes.resize(static_cast<size_t>(l_Size));
	l_File.seekg(0);
	l_File.read(reinterpret_cast<char*>(p_Bytes.data()), l_Size);
	return static_cast<bool>(l_File);
}

std::string fileName(const std::filesystem::path& p_Path)
{
	const std::u8string l_Name = p_Path.filename().u8string();
	return std::string(l_Name.begin(), l_Name.end());
}

// "file:///home/me/a%20b.png" -> path
std::filesystem::path pathFromUri(const std::string_view p_Uri)
{
	constexpr std::string_view PREFIX = "file://";
	if (!p_Uri.starts_with(PREFIX))
		return {};
	std::string l_Path;
	for (size_t i = PREFIX.size(); i < p_Uri.size(); ++i)
	{
		if (p_Uri[i] == '%' && i + 2 < p_Uri.size() + 0 && std::isxdigit(static_cast<unsigned char>(p_Uri[i + 1])) && std::isxdigit(static_cast<unsigned char>(p_Uri[i + 2])))
		{
			l_Path.push_back(static_cast<char>(std::stoi(std::string(p_Uri.substr(i + 1, 2)), nullptr, 16)));
			i += 2;
		}
		else
		{
			l_Path.push_back(p_Uri[i]);
		}
	}
#if defined(_WIN32)
	if (l_Path.size() > 2 && l_Path[0] == '/' && l_Path[2] == ':')
		l_Path.erase(0, 1);
#endif
	return std::filesystem::path(std::u8string(l_Path.begin(), l_Path.end()));
}

// Data in one of SDL's MIME types
bool sdlData(const char* p_Mime, std::vector<uint8_t>& p_Bytes, const uint64_t p_MaxBytes)
{
	if (!SDL_HasClipboardData(p_Mime))
		return false;
	size_t l_Size = 0;
	void* l_Data = SDL_GetClipboardData(p_Mime, &l_Size);
	if (l_Data == nullptr)
		return false;
	const bool l_Ok = l_Size > 0 && l_Size <= p_MaxBytes;
	if (l_Ok)
		p_Bytes.assign(static_cast<const uint8_t*>(l_Data), static_cast<const uint8_t*>(l_Data) + l_Size);
	SDL_free(l_Data);
	return l_Ok;
}

#if defined(_WIN32)
// Reads a global-memory clipboard format. Must be called with the clipboard open.
bool windowsFormat(const UINT p_Format, std::vector<uint8_t>& p_Bytes, const uint64_t p_MaxBytes)
{
	if (!IsClipboardFormatAvailable(p_Format))
		return false;
	const HANDLE l_Handle = GetClipboardData(p_Format);
	if (l_Handle == nullptr)
		return false;
	const SIZE_T l_Size = GlobalSize(l_Handle);
	const void* l_Data = GlobalLock(l_Handle);
	if (l_Data == nullptr)
		return false;
	const bool l_Ok = l_Size > 0 && l_Size <= p_MaxBytes;
	if (l_Ok)
		p_Bytes.assign(static_cast<const uint8_t*>(l_Data), static_cast<const uint8_t*>(l_Data) + l_Size);
	GlobalUnlock(l_Handle);
	return l_Ok;
}

// A device-independent bitmap (what screenshots tools copy) becomes a .bmp file by adding the file header
std::vector<uint8_t> bitmapFileFromDib(const std::vector<uint8_t>& p_Dib)
{
	if (p_Dib.size() < sizeof(BITMAPINFOHEADER))
		return {};
	BITMAPINFOHEADER l_Header{};
	std::memcpy(&l_Header, p_Dib.data(), sizeof(l_Header));
	if (l_Header.biSize < sizeof(BITMAPINFOHEADER))
		return {};
	uint32_t l_PaletteBytes = 0;
	if (l_Header.biBitCount <= 8)
		l_PaletteBytes = (l_Header.biClrUsed != 0 ? l_Header.biClrUsed : (1u << l_Header.biBitCount)) * 4u;
	else if (l_Header.biClrUsed != 0)
		l_PaletteBytes = l_Header.biClrUsed * 4u;
	if (l_Header.biCompression == BI_BITFIELDS && l_Header.biSize == sizeof(BITMAPINFOHEADER))
		l_PaletteBytes += 12; // the three colour masks follow the header

	const uint32_t l_PixelOffset = 14 + l_Header.biSize + l_PaletteBytes;
	std::vector<uint8_t> l_File(14 + p_Dib.size());
	l_File[0] = 'B';
	l_File[1] = 'M';
	const uint32_t l_Total = static_cast<uint32_t>(l_File.size());
	std::memcpy(l_File.data() + 2, &l_Total, 4);
	std::memcpy(l_File.data() + 10, &l_PixelOffset, 4);
	std::memcpy(l_File.data() + 14, p_Dib.data(), p_Dib.size());
	return l_File;
}
#endif
} // namespace

bool clipboardMayHavePictures()
{
#if defined(_WIN32)
	if (IsClipboardFormatAvailable(CF_HDROP) || IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(RegisterClipboardFormatW(L"PNG")))
		return true;
#endif
	return SDL_HasClipboardData("image/png") || SDL_HasClipboardData("image/jpeg") || SDL_HasClipboardData("image/bmp") || SDL_HasClipboardData("image/gif") || SDL_HasClipboardData("text/uri-list");
}

std::vector<ClipboardPicture> readClipboardPictures(const uint64_t p_MaxBytes)
{
	std::vector<ClipboardPicture> l_Result;

#if defined(_WIN32)
	if (OpenClipboard(nullptr))
	{
		if (IsClipboardFormatAvailable(CF_HDROP))
		{
			if (const HDROP l_Drop = static_cast<HDROP>(GetClipboardData(CF_HDROP)))
			{
				const UINT l_Count = DragQueryFileW(l_Drop, 0xFFFFFFFFu, nullptr, 0);
				for (UINT i = 0; i < l_Count; ++i)
				{
					std::wstring l_Path(DragQueryFileW(l_Drop, i, nullptr, 0), L'\0');
					DragQueryFileW(l_Drop, i, l_Path.data(), static_cast<UINT>(l_Path.size() + 1));
					ClipboardPicture l_Picture;
					if (readFile(std::filesystem::path(l_Path), p_MaxBytes, l_Picture.bytes))
					{
						l_Picture.name = fileName(std::filesystem::path(l_Path));
						l_Result.push_back(std::move(l_Picture));
					}
				}
			}
		}
		if (l_Result.empty())
		{
			ClipboardPicture l_Picture;
			l_Picture.name = "Pasted picture";
			// PNG keeps transparency; browsers also offer GIF; screenshots come as bitmaps
			if (windowsFormat(RegisterClipboardFormatW(L"PNG"), l_Picture.bytes, p_MaxBytes) || windowsFormat(RegisterClipboardFormatW(L"GIF"), l_Picture.bytes, p_MaxBytes))
			{
				l_Result.push_back(std::move(l_Picture));
			}
			else
			{
				std::vector<uint8_t> l_Dib;
				if (windowsFormat(CF_DIBV5, l_Dib, p_MaxBytes) || windowsFormat(CF_DIB, l_Dib, p_MaxBytes))
				{
					l_Picture.bytes = bitmapFileFromDib(l_Dib);
					if (!l_Picture.bytes.empty())
						l_Result.push_back(std::move(l_Picture));
				}
			}
		}
		CloseClipboard();
	}
	if (!l_Result.empty())
		return l_Result;
#endif

	// Everywhere else (and as a fallback): what SDL can see
	if (std::vector<uint8_t> l_List; sdlData("text/uri-list", l_List, 1 << 20))
	{
		const std::string_view l_Text(reinterpret_cast<const char*>(l_List.data()), l_List.size());
		size_t l_Pos = 0;
		while (l_Pos < l_Text.size())
		{
			size_t l_End = l_Text.find_first_of("\r\n", l_Pos);
			if (l_End == std::string_view::npos)
				l_End = l_Text.size();
			const std::filesystem::path l_Path = pathFromUri(l_Text.substr(l_Pos, l_End - l_Pos));
			ClipboardPicture l_Picture;
			if (!l_Path.empty() && readFile(l_Path, p_MaxBytes, l_Picture.bytes))
			{
				l_Picture.name = fileName(l_Path);
				l_Result.push_back(std::move(l_Picture));
			}
			l_Pos = l_End + 1;
		}
	}
	if (l_Result.empty())
	{
		for (const char* l_Mime : { "image/png", "image/gif", "image/jpeg", "image/bmp" })
		{
			ClipboardPicture l_Picture;
			l_Picture.name = "Pasted picture";
			if (sdlData(l_Mime, l_Picture.bytes, p_MaxBytes))
			{
				l_Result.push_back(std::move(l_Picture));
				break;
			}
		}
	}
	return l_Result;
}
} // namespace wb::platform
