module;
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

module wb.io.blob;

import wb.io.locale;

namespace wb
{
namespace fs = std::filesystem;

namespace
{
constexpr auto WAIT_SLICE = std::chrono::milliseconds(40);

int64_t modifiedTicks(const fs::path& p_Path)
{
	std::error_code l_Error;
	const auto l_Time = fs::last_write_time(p_Path, l_Error);
	return l_Error ? 0 : static_cast<int64_t>(l_Time.time_since_epoch().count());
}

std::string randomName()
{
	static std::atomic<uint64_t> s_Counter{ 0 };
	std::random_device l_Device;
	const uint64_t l_Random = (static_cast<uint64_t>(l_Device()) << 32) ^ l_Device();
	char l_Text[48];
	std::snprintf(l_Text, sizeof(l_Text), "dl-%016llx-%llu.part", static_cast<unsigned long long>(l_Random), static_cast<unsigned long long>(s_Counter.fetch_add(1)));
	return l_Text;
}
} // namespace

// ------------------------------------------------------------------------------------------------ native file access

struct FileSource::Native
{
#ifdef _WIN32
	HANDLE handle = INVALID_HANDLE_VALUE;
	bool valid() const { return handle != INVALID_HANDLE_VALUE; }
	void close()
	{
		if (valid())
			CloseHandle(handle);
		handle = INVALID_HANDLE_VALUE;
	}

	// Reads at an absolute offset; the position of the handle does not matter, so threads do not disturb each other
	size_t readAt(const uint64_t p_Offset, void* p_Target, const size_t p_Count) const
	{
		size_t l_Done = 0;
		while (l_Done < p_Count)
		{
			OVERLAPPED l_Where{};
			const uint64_t l_At = p_Offset + l_Done;
			l_Where.Offset = static_cast<DWORD>(l_At & 0xFFFFFFFFu);
			l_Where.OffsetHigh = static_cast<DWORD>(l_At >> 32);
			DWORD l_Got = 0;
			const DWORD l_Want = static_cast<DWORD>(std::min<size_t>(p_Count - l_Done, size_t{ 1 } << 28));
			if (!ReadFile(handle, static_cast<uint8_t*>(p_Target) + l_Done, l_Want, &l_Got, &l_Where) && GetLastError() != ERROR_HANDLE_EOF)
				break;
			if (l_Got == 0)
				break;
			l_Done += l_Got;
		}
		return l_Done;
	}

	bool writeAt(const uint64_t p_Offset, const void* p_Data, const size_t p_Count) const
	{
		size_t l_Done = 0;
		while (l_Done < p_Count)
		{
			OVERLAPPED l_Where{};
			const uint64_t l_At = p_Offset + l_Done;
			l_Where.Offset = static_cast<DWORD>(l_At & 0xFFFFFFFFu);
			l_Where.OffsetHigh = static_cast<DWORD>(l_At >> 32);
			DWORD l_Put = 0;
			const DWORD l_Want = static_cast<DWORD>(std::min<size_t>(p_Count - l_Done, size_t{ 1 } << 28));
			if (!WriteFile(handle, static_cast<const uint8_t*>(p_Data) + l_Done, l_Want, &l_Put, &l_Where) || l_Put == 0)
				return false;
			l_Done += l_Put;
		}
		return true;
	}

	bool openRead(const fs::path& p_Path)
	{
		handle = CreateFileW(p_Path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
		return valid();
	}

	// Gone when the last handle closes, even if the program dies first
	bool createTemp(const fs::path& p_Path)
	{
		handle = CreateFileW(p_Path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_RANDOM_ACCESS, nullptr);
		return valid();
	}

	bool fileSize(uint64_t& p_Size) const
	{
		LARGE_INTEGER l_Size{};
		if (!GetFileSizeEx(handle, &l_Size))
			return false;
		p_Size = static_cast<uint64_t>(l_Size.QuadPart);
		return true;
	}
#else
	int fd = -1;
	bool valid() const { return fd >= 0; }
	void close()
	{
		if (valid())
			::close(fd);
		fd = -1;
	}

	size_t readAt(const uint64_t p_Offset, void* p_Target, const size_t p_Count) const
	{
		size_t l_Done = 0;
		while (l_Done < p_Count)
		{
			const ssize_t l_Got = ::pread(fd, static_cast<uint8_t*>(p_Target) + l_Done, p_Count - l_Done, static_cast<off_t>(p_Offset + l_Done));
			if (l_Got < 0 && errno == EINTR)
				continue;
			if (l_Got <= 0)
				break;
			l_Done += static_cast<size_t>(l_Got);
		}
		return l_Done;
	}

	bool writeAt(const uint64_t p_Offset, const void* p_Data, const size_t p_Count) const
	{
		size_t l_Done = 0;
		while (l_Done < p_Count)
		{
			const ssize_t l_Put = ::pwrite(fd, static_cast<const uint8_t*>(p_Data) + l_Done, p_Count - l_Done, static_cast<off_t>(p_Offset + l_Done));
			if (l_Put < 0 && errno == EINTR)
				continue;
			if (l_Put <= 0)
				return false;
			l_Done += static_cast<size_t>(l_Put);
		}
		return true;
	}

	bool openRead(const fs::path& p_Path)
	{
		fd = ::open(p_Path.c_str(), O_RDONLY | O_CLOEXEC);
		return valid();
	}

	// Unlinked at once: the file lives as long as the descriptor does
	bool createTemp(const fs::path& p_Path)
	{
		fd = ::open(p_Path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (!valid())
			return false;
		::unlink(p_Path.c_str());
		return true;
	}

	bool fileSize(uint64_t& p_Size) const
	{
		struct stat l_Stat{};
		if (::fstat(fd, &l_Stat) != 0)
			return false;
		p_Size = static_cast<uint64_t>(l_Stat.st_size);
		return true;
	}
#endif
};

// ------------------------------------------------------------------------------------------------ memory

size_t MemorySource::read(const uint64_t p_Offset, void* p_Target, const size_t p_Count, const std::atomic<bool>*) const
{
	if (p_Offset >= m_Bytes->size())
		return 0;
	const size_t l_Count = static_cast<size_t>(std::min<uint64_t>(p_Count, m_Bytes->size() - p_Offset));
	std::memcpy(p_Target, m_Bytes->data() + p_Offset, l_Count);
	return l_Count;
}

// ------------------------------------------------------------------------------------------------ files

FileSource::FileSource() : m_Native(std::make_unique<Native>())
{
}

FileSource::~FileSource()
{
	m_Native->close();
}

std::shared_ptr<FileSource> FileSource::open(const fs::path& p_Path, std::string& p_Error)
{
	std::shared_ptr<FileSource> l_Source(new FileSource);
	if (!l_Source->m_Native->openRead(p_Path))
	{
		p_Error = tr("io.cannotOpen", { pathToUtf8(p_Path) });
		return nullptr;
	}
	uint64_t l_Size = 0;
	if (!l_Source->m_Native->fileSize(l_Size))
	{
		p_Error = tr("io.cannotRead", { pathToUtf8(p_Path) });
		return nullptr;
	}
	l_Source->m_Path = p_Path;
	l_Source->m_Size = l_Size;
	l_Source->m_Available.store(l_Size);
	l_Source->m_Modified = modifiedTicks(p_Path);
	return l_Source;
}

std::shared_ptr<FileSource> FileSource::createTemporary(const fs::path& p_Directory, const uint64_t p_Total, std::string& p_Error)
{
	std::error_code l_Error;
	fs::create_directories(p_Directory, l_Error);
	const fs::space_info l_Space = fs::space(p_Directory, l_Error);
	// Leave the machine some room to breathe
	constexpr uint64_t RESERVE = uint64_t{ 512 } << 20;
	if (!l_Error && l_Space.available != static_cast<uintmax_t>(-1) && l_Space.available < p_Total + RESERVE)
	{
		p_Error = tr("io.diskSpace", { std::to_string(p_Total >> 20) });
		return nullptr;
	}
	std::shared_ptr<FileSource> l_Source(new FileSource);
	const fs::path l_Path = p_Directory / randomName();
	if (!l_Source->m_Native->createTemp(l_Path))
	{
		p_Error = tr("io.cannotCreateTemp", { pathToUtf8(p_Directory) });
		return nullptr;
	}
	l_Source->m_Path = l_Path;
	l_Source->m_Size = p_Total;
	l_Source->m_Temporary = true;
	return l_Source;
}

size_t FileSource::read(const uint64_t p_Offset, void* p_Target, size_t p_Count, const std::atomic<bool>* p_Cancel) const
{
	if (p_Offset >= m_Size || p_Count == 0)
		return 0;
	p_Count = static_cast<size_t>(std::min<uint64_t>(p_Count, m_Size - p_Offset));
	const uint64_t l_End = p_Offset + p_Count;
	if (m_Available.load(std::memory_order_acquire) < l_End)
	{
		std::unique_lock l_Lock(m_Mutex);
		while (m_Available.load(std::memory_order_acquire) < l_End && !m_Failed.load() && !(p_Cancel != nullptr && p_Cancel->load()))
			m_Arrived.wait_for(l_Lock, WAIT_SLICE);
		const uint64_t l_Have = m_Available.load(std::memory_order_acquire);
		if (l_Have < l_End)
			p_Count = l_Have > p_Offset ? static_cast<size_t>(l_Have - p_Offset) : 0;
	}
	if (p_Count == 0)
		return 0;
	const size_t l_Got = m_Native->readAt(p_Offset, p_Target, p_Count);
	if (l_Got < p_Count)
		m_Failed.store(true); // the disk lost bytes it had promised
	return l_Got;
}

bool FileSource::append(const std::span<const uint8_t> p_Bytes, std::string& p_Error)
{
	const uint64_t l_At = m_Available.load();
	if (!m_Temporary || l_At + p_Bytes.size() > m_Size)
	{
		p_Error = tr("io.moreBytes");
		return false;
	}
	if (!m_Native->writeAt(l_At, p_Bytes.data(), p_Bytes.size()))
	{
		p_Error = tr("io.diskWrite");
		m_Failed.store(true);
		m_Arrived.notify_all();
		return false;
	}
	{
		std::lock_guard l_Lock(m_Mutex);
		m_Available.store(l_At + p_Bytes.size(), std::memory_order_release);
	}
	m_Arrived.notify_all();
	return true;
}

void FileSource::abandon()
{
	if (complete())
		return;
	{
		std::lock_guard l_Lock(m_Mutex);
		m_Failed.store(true);
	}
	m_Arrived.notify_all();
}

// ------------------------------------------------------------------------------------------------ range

size_t RangeSource::read(const uint64_t p_Offset, void* p_Target, size_t p_Count, const std::atomic<bool>* p_Cancel) const
{
	if (p_Offset >= m_Size)
		return 0;
	p_Count = static_cast<size_t>(std::min<uint64_t>(p_Count, m_Size - p_Offset));
	return m_Base->read(m_Offset + p_Offset, p_Target, p_Count, p_Cancel);
}

uint64_t RangeSource::available() const
{
	const uint64_t l_Base = m_Base->available();
	return l_Base <= m_Offset ? 0 : std::min(m_Size, l_Base - m_Offset);
}

// ------------------------------------------------------------------------------------------------ helpers

bool streamBytes(const ByteSource& p_Source, uint64_t p_Offset, uint64_t p_Count, const ByteSink& p_Sink, const size_t p_Block, const std::atomic<bool>* p_Cancel)
{
	std::vector<uint8_t> l_Buffer(static_cast<size_t>(std::min<uint64_t>(std::max<size_t>(p_Block, 4096), std::max<uint64_t>(p_Count, 1))));
	while (p_Count > 0)
	{
		const size_t l_Want = static_cast<size_t>(std::min<uint64_t>(p_Count, l_Buffer.size()));
		if (p_Source.read(p_Offset, l_Buffer.data(), l_Want, p_Cancel) != l_Want)
			return false;
		if (!p_Sink(l_Buffer.data(), l_Want))
			return false;
		p_Offset += l_Want;
		p_Count -= l_Want;
	}
	return true;
}

std::vector<uint8_t> readAllBytes(const ByteSource& p_Source, const uint64_t p_Max)
{
	std::vector<uint8_t> l_Bytes;
	if (p_Source.size() > p_Max)
		return l_Bytes;
	try
	{
		l_Bytes.resize(static_cast<size_t>(p_Source.size()));
	}
	catch (const std::bad_alloc&)
	{
		return {};
	}
	if (p_Source.read(0, l_Bytes.data(), l_Bytes.size()) != l_Bytes.size())
		l_Bytes.clear();
	return l_Bytes;
}

IoResult copySourceToFile(const ByteSource& p_Source, const fs::path& p_Path)
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
		const bool l_Streamed = streamBytes(p_Source, 0, p_Source.size(), [&](const uint8_t* p_Data, const size_t p_Size)
		{
			l_File.write(reinterpret_cast<const char*>(p_Data), static_cast<std::streamsize>(p_Size));
			return static_cast<bool>(l_File);
		});
		l_File.flush();
		if (!l_Streamed || !l_File)
		{
			l_File.close();
			fs::remove(l_Temp, l_Error);
			return IoResult::failure(tr("io.cannotWriteGone", { pathToUtf8(p_Path) }));
		}
	}
	return replaceFile(l_Temp, p_Path);
}

uint64_t fingerprint(const ByteSource& p_Source)
{
	uint64_t l_Hash = 1469598103934665603ull;
	const auto l_Mix = [&](const uint8_t* p_Data, const size_t p_Size)
	{
		for (size_t i = 0; i < p_Size; ++i)
		{
			l_Hash ^= p_Data[i];
			l_Hash *= 1099511628211ull;
		}
	};
	const uint64_t l_Size = p_Source.size();
	l_Mix(reinterpret_cast<const uint8_t*>(&l_Size), sizeof(l_Size));
	constexpr size_t SAMPLE = 64u << 10;
	std::vector<uint8_t> l_Buffer(SAMPLE);
	const uint64_t l_Starts[3] = { 0, l_Size / 2 > SAMPLE / 2 ? l_Size / 2 - SAMPLE / 2 : 0, l_Size > SAMPLE ? l_Size - SAMPLE : 0 };
	for (const uint64_t l_Start : l_Starts)
	{
		const size_t l_Got = p_Source.read(l_Start, l_Buffer.data(), SAMPLE);
		l_Mix(l_Buffer.data(), l_Got);
	}
	return l_Hash;
}

bool sameBytes(const ByteSource& p_A, const ByteSource& p_B)
{
	if (&p_A == &p_B)
		return true;
	if (p_A.size() != p_B.size())
		return false;
	if (!p_A.path().empty() && p_A.path() == p_B.path() && p_A.fileOffset() == p_B.fileOffset() && p_A.modified() == p_B.modified())
		return true;
	constexpr size_t BLOCK = size_t{ 1 } << 20;
	std::vector<uint8_t> l_A(BLOCK);
	std::vector<uint8_t> l_B(BLOCK);
	for (uint64_t l_At = 0; l_At < p_A.size(); l_At += BLOCK)
	{
		const size_t l_Count = static_cast<size_t>(std::min<uint64_t>(BLOCK, p_A.size() - l_At));
		if (p_A.read(l_At, l_A.data(), l_Count) != l_Count || p_B.read(l_At, l_B.data(), l_Count) != l_Count || std::memcmp(l_A.data(), l_B.data(), l_Count) != 0)
			return false;
	}
	return true;
}
} // namespace wb
