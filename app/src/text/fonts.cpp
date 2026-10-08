module;
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <SDL3/SDL.h>
#include "ui/stb_truetype.hpp"

module wb.text.fonts;

import wb.math;
import wb.doc.object;
import wb.text.layout;
import wb.text.utf8;

namespace fs = std::filesystem;

namespace wb::text
{
struct FontRegistry::Face
{
	std::vector<uint8_t> data;
	stbtt_fontinfo info{};
	std::string family;
	uint8_t style = 0;
	bool embeddable = true;
	bool bundled = false;
	float emScale = 0.f; // font units -> ems
};

namespace
{
constexpr uint64_t MAX_FONT_FILE_BYTES = 256ull << 20;

// ------------------------------------------------------------------------------------------------ sfnt names

using RangeReader = std::function<bool(uint64_t p_Offset, uint64_t p_Size, std::vector<uint8_t>& p_Out)>;

uint32_t be16(const std::vector<uint8_t>& p_Bytes, const size_t p_At)
{
	return p_At + 2 <= p_Bytes.size() ? (static_cast<uint32_t>(p_Bytes[p_At]) << 8) | p_Bytes[p_At + 1] : 0u;
}

uint32_t be32(const std::vector<uint8_t>& p_Bytes, const size_t p_At)
{
	return p_At + 4 <= p_Bytes.size() ? (be16(p_Bytes, p_At) << 16) | be16(p_Bytes, p_At + 2) : 0u;
}

struct FaceNames
{
	std::string family;
	std::string subfamily;
	bool embeddable = true;
};

std::string utf16beToUtf8(const std::vector<uint8_t>& p_Bytes, const size_t p_Begin, const size_t p_Length)
{
	std::string l_Out;
	for (size_t i = p_Begin; i + 1 < p_Begin + p_Length && i + 1 < p_Bytes.size(); i += 2)
	{
		uint32_t l_Unit = be16(p_Bytes, i);
		if (l_Unit >= 0xD800 && l_Unit < 0xDC00 && i + 3 < p_Begin + p_Length)
		{
			const uint32_t l_Low = be16(p_Bytes, i + 2);
			if (l_Low >= 0xDC00 && l_Low < 0xE000)
			{
				l_Unit = 0x10000 + ((l_Unit - 0xD800) << 10) + (l_Low - 0xDC00);
				i += 2;
			}
		}
		appendCodepoint(l_Out, l_Unit);
	}
	return l_Out;
}

// Reads the family / subfamily names (name IDs 1 and 2) and the embedding permission of the font at p_FontOffset
bool readFaceNames(const RangeReader& p_Read, const uint64_t p_FontOffset, FaceNames& p_Names)
{
	std::vector<uint8_t> l_Header;
	if (!p_Read(p_FontOffset, 12, l_Header) || l_Header.size() < 12)
		return false;
	const uint32_t l_Version = be32(l_Header, 0);
	if (l_Version != 0x00010000 && l_Version != 0x4F54544F /* OTTO */ && l_Version != 0x74727565 /* true */)
		return false;
	const uint32_t l_TableCount = be16(l_Header, 4);
	if (l_TableCount == 0 || l_TableCount > 256)
		return false;
	std::vector<uint8_t> l_Directory;
	if (!p_Read(p_FontOffset + 12, static_cast<uint64_t>(l_TableCount) * 16, l_Directory) || l_Directory.size() < static_cast<size_t>(l_TableCount) * 16)
		return false;

	uint32_t l_NameOffset = 0, l_NameLength = 0, l_Os2Offset = 0, l_Os2Length = 0;
	for (uint32_t i = 0; i < l_TableCount; ++i)
	{
		const size_t l_At = static_cast<size_t>(i) * 16;
		const uint32_t l_Tag = be32(l_Directory, l_At);
		if (l_Tag == 0x6E616D65 /* name */)
		{
			l_NameOffset = be32(l_Directory, l_At + 8);
			l_NameLength = be32(l_Directory, l_At + 12);
		}
		else if (l_Tag == 0x4F532F32 /* OS/2 */)
		{
			l_Os2Offset = be32(l_Directory, l_At + 8);
			l_Os2Length = be32(l_Directory, l_At + 12);
		}
	}
	if (l_NameOffset == 0 || l_NameLength < 6 || l_NameLength > (4u << 20))
		return false;

	if (l_Os2Offset != 0 && l_Os2Length >= 10)
	{
		std::vector<uint8_t> l_Os2;
		if (p_Read(l_Os2Offset, 10, l_Os2) && l_Os2.size() >= 10)
		{
			const uint32_t l_Type = be16(l_Os2, 8);
			p_Names.embeddable = (l_Type & 0x0002) == 0;
		}
	}

	std::vector<uint8_t> l_Name;
	if (!p_Read(l_NameOffset, l_NameLength, l_Name) || l_Name.size() < 6)
		return false;
	const uint32_t l_Count = be16(l_Name, 2);
	const uint32_t l_StringBase = be16(l_Name, 4);
	int l_FamilyScore = -1, l_SubScore = -1;
	for (uint32_t i = 0; i < l_Count; ++i)
	{
		const size_t l_At = 6 + static_cast<size_t>(i) * 12;
		if (l_At + 12 > l_Name.size())
			break;
		const uint32_t l_Platform = be16(l_Name, l_At);
		const uint32_t l_Encoding = be16(l_Name, l_At + 2);
		const uint32_t l_Language = be16(l_Name, l_At + 4);
		const uint32_t l_Id = be16(l_Name, l_At + 6);
		const uint32_t l_Length = be16(l_Name, l_At + 8);
		const uint32_t l_Offset = be16(l_Name, l_At + 10);
		if (l_Id != 1 && l_Id != 2)
			continue;
		const size_t l_Begin = static_cast<size_t>(l_StringBase) + l_Offset;
		if (l_Begin + l_Length > l_Name.size())
			continue;
		std::string l_Text;
		int l_Score = 0;
		if (l_Platform == 3 && (l_Encoding == 1 || l_Encoding == 10))
		{
			l_Text = utf16beToUtf8(l_Name, l_Begin, l_Length);
			l_Score = l_Language == 0x409 ? 3 : 2;
		}
		else if (l_Platform == 0)
		{
			l_Text = utf16beToUtf8(l_Name, l_Begin, l_Length);
			l_Score = 1;
		}
		else if (l_Platform == 1 && l_Encoding == 0)
		{
			l_Text.assign(reinterpret_cast<const char*>(l_Name.data()) + l_Begin, l_Length);
			l_Score = 0;
		}
		else
		{
			continue;
		}
		if (l_Text.empty())
			continue;
		if (l_Id == 1 && l_Score > l_FamilyScore)
		{
			p_Names.family = std::move(l_Text);
			l_FamilyScore = l_Score;
		}
		else if (l_Id == 2 && l_Score > l_SubScore)
		{
			p_Names.subfamily = std::move(l_Text);
			l_SubScore = l_Score;
		}
	}
	return !p_Names.family.empty();
}

std::string lower(std::string_view p_Text)
{
	std::string l_Out(p_Text);
	for (char& l_Char : l_Out)
		l_Char = static_cast<char>(std::tolower(static_cast<unsigned char>(l_Char)));
	return l_Out;
}

uint8_t styleFromSubfamily(const std::string& p_Subfamily)
{
	const std::string l_Text = lower(p_Subfamily);
	uint8_t l_Style = 0;
	if (l_Text.find("bold") != std::string::npos && l_Text.find("semi") == std::string::npos && l_Text.find("demi") == std::string::npos && l_Text.find("extra") == std::string::npos && l_Text.find("ultra") == std::string::npos)
		l_Style |= TextStyle::Bold;
	if (l_Text.find("italic") != std::string::npos || l_Text.find("oblique") != std::string::npos)
		l_Style |= TextStyle::Italic;
	return l_Style;
}

// Every face of a font file (a collection holds several)
struct FileFace
{
	std::string family;
	uint8_t style = 0;
	uint32_t index = 0;
	bool embeddable = true;
};

std::vector<FileFace> readFileFaces(const fs::path& p_Path)
{
	std::vector<FileFace> l_Faces;
	std::ifstream l_File(p_Path, std::ios::binary | std::ios::ate);
	if (!l_File)
		return l_Faces;
	const uint64_t l_Size = static_cast<uint64_t>(l_File.tellg());
	if (l_Size < 16 || l_Size > MAX_FONT_FILE_BYTES)
		return l_Faces;
	const RangeReader l_Read = [&](const uint64_t p_Offset, const uint64_t p_Length, std::vector<uint8_t>& p_Out)
	{
		if (p_Offset + p_Length > l_Size)
			return false;
		p_Out.resize(static_cast<size_t>(p_Length));
		l_File.clear();
		l_File.seekg(static_cast<std::streamoff>(p_Offset));
		l_File.read(reinterpret_cast<char*>(p_Out.data()), static_cast<std::streamsize>(p_Length));
		return static_cast<bool>(l_File);
	};

	std::vector<uint8_t> l_Magic;
	if (!l_Read(0, 12, l_Magic))
		return l_Faces;
	std::vector<uint64_t> l_Offsets;
	if (be32(l_Magic, 0) == 0x74746366 /* ttcf */)
	{
		const uint32_t l_Count = std::min<uint32_t>(be32(l_Magic, 8), 64);
		std::vector<uint8_t> l_Table;
		if (!l_Read(12, static_cast<uint64_t>(l_Count) * 4, l_Table))
			return l_Faces;
		for (uint32_t i = 0; i < l_Count; ++i)
			l_Offsets.push_back(be32(l_Table, static_cast<size_t>(i) * 4));
	}
	else
	{
		l_Offsets.push_back(0);
	}
	for (uint32_t i = 0; i < l_Offsets.size(); ++i)
	{
		FaceNames l_Names;
		if (readFaceNames(l_Read, l_Offsets[i], l_Names))
			l_Faces.push_back(FileFace{ .family = l_Names.family, .style = styleFromSubfamily(l_Names.subfamily), .index = i, .embeddable = l_Names.embeddable });
	}
	return l_Faces;
}

bool isFontFile(const fs::path& p_Path)
{
	const std::string l_Extension = lower(p_Path.extension().string());
	return l_Extension == ".ttf" || l_Extension == ".otf" || l_Extension == ".ttc" || l_Extension == ".otc";
}

int rankOf(const bool p_Bundled, const bool p_Document)
{
	return p_Bundled ? 3 : (p_Document ? 1 : 2);
}

std::vector<fs::path> systemFontDirectories()
{
	std::vector<fs::path> l_Directories;
#if defined(_WIN32)
	if (const char* l_Windir = SDL_getenv("WINDIR"))
		l_Directories.push_back(fs::path(l_Windir) / "Fonts");
	else
		l_Directories.push_back("C:/Windows/Fonts");
	if (const char* l_Local = SDL_getenv("LOCALAPPDATA"))
		l_Directories.push_back(fs::path(l_Local) / "Microsoft" / "Windows" / "Fonts");
#elif defined(__APPLE__)
	l_Directories.push_back("/System/Library/Fonts");
	l_Directories.push_back("/Library/Fonts");
	if (const char* l_Home = SDL_getenv("HOME"))
		l_Directories.push_back(fs::path(l_Home) / "Library" / "Fonts");
#else
	l_Directories.push_back("/usr/share/fonts");
	l_Directories.push_back("/usr/local/share/fonts");
	if (const char* l_Home = SDL_getenv("HOME"))
	{
		l_Directories.push_back(fs::path(l_Home) / ".fonts");
		l_Directories.push_back(fs::path(l_Home) / ".local" / "share" / "fonts");
	}
#endif
	return l_Directories;
}

// Families tried, in order, for characters the chosen font lacks (scripts, symbols, math...)
constexpr std::array<std::string_view, 24> FALLBACK_FAMILIES{
	"Segoe UI", "Segoe UI Symbol", "Nirmala UI", "Leelawadee UI", "Microsoft YaHei", "Microsoft JhengHei", "Yu Gothic", "Meiryo", "Malgun Gothic", "Cambria Math", "MS Gothic", "SimSun",
	"Noto Sans", "Noto Sans CJK JP", "Noto Sans CJK SC", "Noto Sans CJK KR", "Noto Sans CJK TC", "Noto Sans Symbols", "Noto Sans Symbols 2", "Noto Sans Math", "DejaVu Sans", "Arial Unicode MS",
	"Apple Symbols", "Hiragino Sans",
};
} // namespace

// ------------------------------------------------------------------------------------------------ registry

FontRegistry::FontRegistry() = default;

FontRegistry::~FontRegistry()
{
	if (m_ScanThread.joinable())
		m_ScanThread.join();
}

FontRegistry::Family* FontRegistry::findFamily(const std::string_view p_Name)
{
	const auto l_It = m_Families.find(lower(p_Name));
	return l_It != m_Families.end() ? &l_It->second : nullptr;
}

const FontRegistry::Family* FontRegistry::findFamily(const std::string_view p_Name) const
{
	const auto l_It = m_Families.find(lower(p_Name));
	return l_It != m_Families.end() ? &l_It->second : nullptr;
}

bool FontRegistry::hasFamily(const std::string_view p_Name) const
{
	return findFamily(p_Name) != nullptr;
}

void FontRegistry::registerSource(const std::string& p_Family, const uint8_t p_Style, Source p_Source)
{
	if (p_Family.empty())
		return;
	Family& l_Family = m_Families[lower(p_Family)];
	if (l_Family.name.empty())
		l_Family.name = p_Family;
	std::optional<Source>& l_Slot = l_Family.styles[p_Style & 3];
	if (l_Slot && rankOf(l_Slot->bundled, l_Slot->document) >= rankOf(p_Source.bundled, p_Source.document))
		return;
	l_Slot = std::move(p_Source);
}

void FontRegistry::rebuildFamilyList()
{
	m_FamilyList.clear();
	for (const auto& [l_Key, l_Family] : m_Families)
	{
		FamilyInfo l_Info{ .name = l_Family.name };
		for (size_t s = 0; s < l_Family.styles.size(); ++s)
		{
			if (!l_Family.styles[s])
				continue;
			l_Info.styles |= static_cast<uint8_t>(1u << s);
			l_Info.bundled = l_Info.bundled || l_Family.styles[s]->bundled;
			l_Info.embeddable = l_Info.embeddable && l_Family.styles[s]->embeddable;
		}
		if (l_Info.styles != 0)
			m_FamilyList.push_back(std::move(l_Info));
	}
	std::stable_sort(m_FamilyList.begin(), m_FamilyList.end(), [](const FamilyInfo& p_A, const FamilyInfo& p_B)
	{
		if (p_A.bundled != p_B.bundled)
			return p_A.bundled;
		return lower(p_A.name) < lower(p_B.name);
	});
	m_FallbackCache.clear();
	++m_Generation;
}

size_t FontRegistry::addBundledDirectory(const fs::path& p_Directory)
{
	size_t l_Count = 0;
	std::error_code l_Error;
	for (const fs::directory_entry& l_Entry : fs::recursive_directory_iterator(p_Directory, l_Error))
	{
		if (!l_Entry.is_regular_file(l_Error) || !isFontFile(l_Entry.path()))
			continue;
		for (const FileFace& l_Face : readFileFaces(l_Entry.path()))
		{
			Source l_Source;
			l_Source.path = l_Entry.path();
			l_Source.faceIndex = l_Face.index;
			l_Source.bundled = true;
			l_Source.embeddable = l_Face.embeddable;
			registerSource(l_Face.family, l_Face.style, std::move(l_Source));
			++l_Count;
		}
	}
	rebuildFamilyList();
	return l_Count;
}

void FontRegistry::setUserDirectory(const fs::path& p_Directory)
{
	m_UserDirectory = p_Directory;
}

void FontRegistry::scanSystemFonts()
{
	if (m_ScanRunning.exchange(true))
		return;
	if (m_ScanThread.joinable())
		m_ScanThread.join();
	std::vector<fs::path> l_Directories = systemFontDirectories();
	if (!m_UserDirectory.empty())
		l_Directories.push_back(m_UserDirectory);
	m_ScanReady = false;
	m_ScanThread = std::thread([this, l_Directories = std::move(l_Directories)]()
	{
		std::vector<ScanResult> l_Found;
		for (const fs::path& l_Directory : l_Directories)
		{
			std::error_code l_Error;
			fs::recursive_directory_iterator l_It(l_Directory, fs::directory_options::skip_permission_denied, l_Error);
			for (; !l_Error && l_It != fs::recursive_directory_iterator(); l_It.increment(l_Error))
			{
				std::error_code l_FileError;
				if (!l_It->is_regular_file(l_FileError) || !isFontFile(l_It->path()))
					continue;
				for (FileFace& l_Face : readFileFaces(l_It->path()))
					l_Found.push_back(ScanResult{ .family = std::move(l_Face.family), .style = l_Face.style, .path = l_It->path(), .faceIndex = l_Face.index, .embeddable = l_Face.embeddable });
			}
		}
		{
			const std::lock_guard<std::mutex> l_Lock(m_ScanMutex);
			m_ScanResults = std::move(l_Found);
			m_ScanReady = true;
		}
		m_ScanRunning = false;
	});
}

bool FontRegistry::pump()
{
	std::vector<ScanResult> l_Results;
	{
		const std::lock_guard<std::mutex> l_Lock(m_ScanMutex);
		if (!m_ScanReady)
			return false;
		l_Results = std::move(m_ScanResults);
		m_ScanReady = false;
	}
	if (m_ScanThread.joinable())
		m_ScanThread.join();
	for (ScanResult& l_Result : l_Results)
	{
		Source l_Source;
		l_Source.path = std::move(l_Result.path);
		l_Source.faceIndex = l_Result.faceIndex;
		l_Source.embeddable = l_Result.embeddable;
		registerSource(l_Result.family, l_Result.style, std::move(l_Source));
	}
	rebuildFamilyList();
	return true;
}

std::string FontRegistry::importFont(const fs::path& p_File, std::string& p_Error)
{
	std::error_code l_Error;
	if (!fs::is_regular_file(p_File, l_Error) || !isFontFile(p_File))
	{
		p_Error = "That is not a font file (.ttf, .otf or .ttc).";
		return {};
	}
	const std::vector<FileFace> l_Faces = readFileFaces(p_File);
	if (l_Faces.empty())
	{
		p_Error = "This font file could not be read.";
		return {};
	}
	fs::path l_Target = p_File;
	if (!m_UserDirectory.empty())
	{
		fs::create_directories(m_UserDirectory, l_Error);
		l_Target = m_UserDirectory / p_File.filename();
		for (int l_Counter = 2; fs::exists(l_Target, l_Error) && l_Counter < 1000; ++l_Counter)
			l_Target = m_UserDirectory / (p_File.stem().string() + "-" + std::to_string(l_Counter) + p_File.extension().string());
		fs::copy_file(p_File, l_Target, fs::copy_options::overwrite_existing, l_Error);
		if (l_Error)
		{
			p_Error = "The font could not be copied into the app's font folder.";
			return {};
		}
	}
	for (const FileFace& l_Face : l_Faces)
	{
		Source l_Source;
		l_Source.path = l_Target;
		l_Source.faceIndex = l_Face.index;
		l_Source.embeddable = l_Face.embeddable;
		// An imported font replaces an installed one of the same name: the user asked for this file
		Family& l_Family = m_Families[lower(l_Face.family)];
		if (l_Family.name.empty())
			l_Family.name = l_Face.family;
		l_Family.styles[l_Face.style & 3] = std::move(l_Source);
	}
	rebuildFamilyList();
	return l_Faces.front().family;
}

void FontRegistry::addDocumentFont(const std::string_view p_Family, const uint8_t p_Style, std::vector<uint8_t> p_Bytes)
{
	Source l_Source;
	l_Source.bytes = std::move(p_Bytes);
	l_Source.document = true;
	registerSource(std::string(p_Family), p_Style, std::move(l_Source));
	rebuildFamilyList();
}

// ------------------------------------------------------------------------------------------------ faces

FaceId FontRegistry::load(Source& p_Source, const uint8_t p_Style, const std::string& p_Family)
{
	if (p_Source.face != NO_FACE)
		return p_Source.face;
	auto l_Face = std::make_unique<Face>();
	if (!p_Source.bytes.empty())
	{
		l_Face->data = p_Source.bytes;
	}
	else
	{
		std::ifstream l_File(p_Source.path, std::ios::binary | std::ios::ate);
		if (!l_File)
			return NO_FACE;
		const std::streamsize l_Size = l_File.tellg();
		if (l_Size <= 0 || static_cast<uint64_t>(l_Size) > MAX_FONT_FILE_BYTES)
			return NO_FACE;
		l_Face->data.resize(static_cast<size_t>(l_Size));
		l_File.seekg(0);
		l_File.read(reinterpret_cast<char*>(l_Face->data.data()), l_Size);
		if (!l_File)
			return NO_FACE;
	}
	const int l_Offset = stbtt_GetFontOffsetForIndex(l_Face->data.data(), static_cast<int>(p_Source.faceIndex));
	if (l_Offset < 0 || stbtt_InitFont(&l_Face->info, l_Face->data.data(), l_Offset) == 0)
		return NO_FACE;
	l_Face->family = p_Family;
	l_Face->style = p_Style;
	l_Face->embeddable = p_Source.embeddable;
	l_Face->bundled = p_Source.bundled;
	l_Face->emScale = stbtt_ScaleForMappingEmToPixels(&l_Face->info, 1.f);
	m_Faces.push_back(std::move(l_Face));
	p_Source.face = static_cast<FaceId>(m_Faces.size() - 1);
	return p_Source.face;
}

ResolvedFace FontRegistry::resolve(const std::string_view p_Family, const uint8_t p_Style)
{
	static constexpr std::array<std::array<uint8_t, 4>, 4> ORDER{ {
		{ 0, 1, 2, 3 }, // regular
		{ 1, 3, 0, 2 }, // bold
		{ 2, 3, 0, 1 }, // italic
		{ 3, 1, 2, 0 }, // bold italic
	} };
	const uint8_t l_Wanted = p_Style & 3;
	Family* l_Family = findFamily(p_Family);
	for (int l_Attempt = 0; l_Attempt < 2; ++l_Attempt)
	{
		if (l_Family != nullptr)
		{
			for (const uint8_t l_Slot : ORDER[l_Wanted])
			{
				std::optional<Source>& l_Source = l_Family->styles[l_Slot];
				if (!l_Source)
					continue;
				const FaceId l_Id = load(*l_Source, l_Slot, l_Family->name);
				if (l_Id != NO_FACE)
					return ResolvedFace{ .face = l_Id, .actualStyle = l_Slot, .syntheticStyle = static_cast<uint8_t>(l_Wanted & ~l_Slot) };
			}
		}
		l_Family = findFamily(DEFAULT_FAMILY); // unknown or unreadable family
	}
	return ResolvedFace{};
}

FaceId FontRegistry::fallbackFor(const uint32_t p_Codepoint, const uint8_t p_Style)
{
	const uint64_t l_Key = (static_cast<uint64_t>(p_Codepoint) << 2) | (p_Style & 3);
	if (const auto l_It = m_FallbackCache.find(l_Key); l_It != m_FallbackCache.end())
		return l_It->second;
	FaceId l_Result = NO_FACE;
	const ResolvedFace l_Default = resolve(DEFAULT_FAMILY, p_Style);
	if (l_Default.face != NO_FACE && glyphIndex(l_Default.face, p_Codepoint) != 0)
	{
		l_Result = l_Default.face;
	}
	else
	{
		for (const std::string_view l_Name : FALLBACK_FAMILIES)
		{
			if (findFamily(l_Name) == nullptr)
				continue;
			const ResolvedFace l_Candidate = resolve(l_Name, p_Style);
			if (l_Candidate.face != NO_FACE && glyphIndex(l_Candidate.face, p_Codepoint) != 0)
			{
				l_Result = l_Candidate.face;
				break;
			}
		}
	}
	m_FallbackCache[l_Key] = l_Result;
	return l_Result;
}

uint32_t FontRegistry::glyphIndex(const FaceId p_Face, const uint32_t p_Codepoint) const
{
	if (p_Face >= m_Faces.size())
		return 0;
	return static_cast<uint32_t>(stbtt_FindGlyphIndex(&m_Faces[p_Face]->info, static_cast<int>(p_Codepoint)));
}

float FontRegistry::advanceEm(const FaceId p_Face, const uint32_t p_Glyph) const
{
	if (p_Face >= m_Faces.size())
		return 0.f;
	const Face& l_Face = *m_Faces[p_Face];
	int l_Advance = 0, l_Bearing = 0;
	stbtt_GetGlyphHMetrics(&l_Face.info, static_cast<int>(p_Glyph), &l_Advance, &l_Bearing);
	return static_cast<float>(l_Advance) * l_Face.emScale;
}

float FontRegistry::kerningEm(const FaceId p_Face, const uint32_t p_LeftGlyph, const uint32_t p_RightGlyph) const
{
	if (p_Face >= m_Faces.size() || p_LeftGlyph == 0 || p_RightGlyph == 0)
		return 0.f;
	const Face& l_Face = *m_Faces[p_Face];
	return static_cast<float>(stbtt_GetGlyphKernAdvance(&l_Face.info, static_cast<int>(p_LeftGlyph), static_cast<int>(p_RightGlyph))) * l_Face.emScale;
}

LineMetrics FontRegistry::lineMetrics(const FaceId p_Face) const
{
	if (p_Face >= m_Faces.size())
		return LineMetrics{};
	const Face& l_Face = *m_Faces[p_Face];
	int l_Ascent = 0, l_Descent = 0, l_Gap = 0;
	stbtt_GetFontVMetrics(&l_Face.info, &l_Ascent, &l_Descent, &l_Gap);
	return LineMetrics{ .ascent = static_cast<float>(l_Ascent) * l_Face.emScale, .descent = static_cast<float>(l_Descent) * l_Face.emScale, .lineGap = static_cast<float>(l_Gap) * l_Face.emScale };
}

uint8_t FontRegistry::faceStyle(const FaceId p_Face) const
{
	return p_Face < m_Faces.size() ? m_Faces[p_Face]->style : 0;
}

bool FontRegistry::rasterize(const FaceId p_Face, const uint32_t p_Glyph, SdfBitmap& p_Bitmap) const
{
	if (p_Face >= m_Faces.size())
		return false;
	const Face& l_Face = *m_Faces[p_Face];
	const float l_Scale = stbtt_ScaleForMappingEmToPixels(&l_Face.info, static_cast<float>(sdf::EM_PIXELS));
	int l_Width = 0, l_Height = 0, l_OffsetX = 0, l_OffsetY = 0;
	unsigned char* l_Pixels = stbtt_GetGlyphSDF(&l_Face.info, l_Scale, static_cast<int>(p_Glyph), sdf::PADDING, static_cast<unsigned char>(sdf::ON_EDGE), sdf::DISTANCE_SCALE, &l_Width, &l_Height, &l_OffsetX, &l_OffsetY);
	if (l_Pixels == nullptr)
		return false;
	p_Bitmap.width = l_Width;
	p_Bitmap.height = l_Height;
	p_Bitmap.left = static_cast<float>(l_OffsetX) / static_cast<float>(sdf::EM_PIXELS);
	p_Bitmap.top = static_cast<float>(l_OffsetY) / static_cast<float>(sdf::EM_PIXELS);
	p_Bitmap.emWidth = static_cast<float>(l_Width) / static_cast<float>(sdf::EM_PIXELS);
	p_Bitmap.emHeight = static_cast<float>(l_Height) / static_cast<float>(sdf::EM_PIXELS);
	p_Bitmap.pixels.assign(l_Pixels, l_Pixels + static_cast<size_t>(l_Width) * static_cast<size_t>(l_Height));
	stbtt_FreeSDF(l_Pixels, nullptr);
	return true;
}

std::span<const uint8_t> FontRegistry::embeddableBytes(const FaceId p_Face) const
{
	if (p_Face >= m_Faces.size() || !m_Faces[p_Face]->embeddable)
		return {};
	return m_Faces[p_Face]->data;
}

std::string_view FontRegistry::faceFamily(const FaceId p_Face) const
{
	return p_Face < m_Faces.size() ? std::string_view(m_Faces[p_Face]->family) : std::string_view();
}

bool FontRegistry::isLightweight(const std::string_view p_Family) const
{
	constexpr uint64_t LIMIT = 3u << 20;
	const Family* l_Family = findFamily(p_Family);
	if (l_Family == nullptr)
		return false;
	for (const std::optional<Source>& l_Source : l_Family->styles)
	{
		if (!l_Source)
			continue;
		if (l_Source->face != NO_FACE)
			return true;
		if (!l_Source->bytes.empty())
			return l_Source->bytes.size() <= LIMIT;
		std::error_code l_Error;
		const uint64_t l_Size = fs::file_size(l_Source->path, l_Error);
		return !l_Error && l_Size <= LIMIT;
	}
	return false;
}

bool FontRegistry::isBundled(const FaceId p_Face) const
{
	return p_Face < m_Faces.size() && m_Faces[p_Face]->bundled;
}

const uint8_t* FontRegistry::faceData(const FaceId p_Face, size_t& p_Size) const
{
	if (p_Face >= m_Faces.size())
	{
		p_Size = 0;
		return nullptr;
	}
	p_Size = m_Faces[p_Face]->data.size();
	return m_Faces[p_Face]->data.data();
}

// ------------------------------------------------------------------------------------------------ metrics

FamilyMetrics::FamilyMetrics(FontRegistry& p_Registry, const std::string_view p_Family, const uint8_t p_Style)
	: m_Registry(p_Registry), m_Primary(p_Registry.resolve(p_Family, p_Style)), m_Style(p_Style & 3)
{
}

GlyphInfo FamilyMetrics::glyph(const uint32_t p_Codepoint) const
{
	if (isFormatting(p_Codepoint))
		return GlyphInfo{ .font = m_Primary.face, .glyph = 0, .advance = 0.f };
	FaceId l_Face = m_Primary.face;
	uint32_t l_Glyph = m_Registry.glyphIndex(l_Face, p_Codepoint);
	if (l_Glyph == 0 && !isCombining(p_Codepoint))
	{
		const FaceId l_Fallback = m_Registry.fallbackFor(p_Codepoint, m_Style);
		if (l_Fallback != NO_FACE)
		{
			l_Face = l_Fallback;
			l_Glyph = m_Registry.glyphIndex(l_Fallback, p_Codepoint);
		}
	}
	float l_Advance = m_Registry.advanceEm(l_Face, l_Glyph);
	if ((m_Style & TextStyle::Bold) != 0 && (m_Registry.faceStyle(l_Face) & TextStyle::Bold) == 0)
		l_Advance += SYNTHETIC_BOLD_EM;
	return GlyphInfo{ .font = l_Face, .glyph = l_Glyph, .advance = l_Advance };
}

float FamilyMetrics::kerning(const GlyphInfo& p_Left, const uint32_t p_LeftCodepoint, const GlyphInfo& p_Right, const uint32_t p_RightCodepoint) const
{
	(void)p_LeftCodepoint;
	(void)p_RightCodepoint;
	if (p_Left.font != p_Right.font)
		return 0.f;
	return m_Registry.kerningEm(p_Left.font, p_Left.glyph, p_Right.glyph);
}

LineMetrics FamilyMetrics::lineMetrics() const
{
	return m_Registry.lineMetrics(m_Primary.face);
}
} // namespace wb::text
