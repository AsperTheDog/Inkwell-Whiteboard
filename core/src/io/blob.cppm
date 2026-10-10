// Read-only byte sources: memory, a file on disk (or a stretch of one) and a file that is still being filled.
//
// Big pictures and videos are never loaded into memory whole. The board refers to them through a ByteSource, and the
// decoder, the saver, the network and the "save this file" command read from it in pieces, from any thread. A source
// that is a file keeps the file open for as long as anybody holds it, so the file can be moved or deleted by the user
// (or replaced by a newer save of the board) without the pictures that came from it going away.
module;
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

export module wb.io.blob;

import wb.io.file;

export namespace wb
{
class ByteSource
{
public:
	virtual ~ByteSource() = default;

	[[nodiscard]] virtual uint64_t size() const = 0;
	// Copies up to p_Count bytes that start at p_Offset. Fewer come back only at the end of the source or when it fails.
	// A source that is still being filled waits for the bytes (until p_Cancel turns true). Safe from any thread.
	virtual size_t read(uint64_t p_Offset, void* p_Target, size_t p_Count, const std::atomic<bool>* p_Cancel = nullptr) const = 0;
	// How many bytes from the start can be read without waiting
	[[nodiscard]] virtual uint64_t available() const { return size(); }
	// The disk gave up on a read, or the bytes will never arrive
	[[nodiscard]] virtual bool failed() const { return false; }
	// The file the bytes live in (empty for memory). Offsets into it are given by fileOffset().
	[[nodiscard]] virtual std::filesystem::path path() const { return {}; }
	[[nodiscard]] virtual uint64_t fileOffset() const { return 0; }
	// The file is deleted when it is closed: it holds a copy that exists nowhere else (a download)
	[[nodiscard]] virtual bool temporary() const { return false; }
	// When the file was last written (opaque ticks), taken when it was opened; 0 for memory
	[[nodiscard]] virtual int64_t modified() const { return 0; }
};

class MemorySource final : public ByteSource
{
public:
	explicit MemorySource(std::shared_ptr<const std::vector<uint8_t>> p_Bytes) : m_Bytes(std::move(p_Bytes)) {}
	explicit MemorySource(std::vector<uint8_t> p_Bytes) : m_Bytes(std::make_shared<const std::vector<uint8_t>>(std::move(p_Bytes))) {}

	[[nodiscard]] uint64_t size() const override { return m_Bytes->size(); }
	size_t read(uint64_t p_Offset, void* p_Target, size_t p_Count, const std::atomic<bool>* p_Cancel = nullptr) const override;

private:
	std::shared_ptr<const std::vector<uint8_t>> m_Bytes;
};

// A whole file, kept open. One of these may also be filled by append() (a download): readers then wait for what has
// not arrived yet.
class FileSource final : public ByteSource
{
public:
	~FileSource() override;
	FileSource(const FileSource&) = delete;
	FileSource& operator=(const FileSource&) = delete;

	// An existing file as it is now (a file that grows afterwards is not followed)
	[[nodiscard]] static std::shared_ptr<FileSource> open(const std::filesystem::path& p_Path, std::string& p_Error);
	// A new nameless-in-effect file in p_Directory that disappears when the last holder lets go (or the program ends),
	// meant to receive p_Total bytes through append(). Fails when the disk has less room than that.
	[[nodiscard]] static std::shared_ptr<FileSource> createTemporary(const std::filesystem::path& p_Directory, uint64_t p_Total, std::string& p_Error);

	[[nodiscard]] uint64_t size() const override { return m_Size; }
	size_t read(uint64_t p_Offset, void* p_Target, size_t p_Count, const std::atomic<bool>* p_Cancel = nullptr) const override;
	[[nodiscard]] uint64_t available() const override { return m_Available.load(std::memory_order_acquire); }
	[[nodiscard]] bool failed() const override { return m_Failed.load(std::memory_order_acquire); }
	[[nodiscard]] std::filesystem::path path() const override { return m_Path; }
	[[nodiscard]] bool temporary() const override { return m_Temporary; }
	[[nodiscard]] int64_t modified() const override { return m_Modified; }

	// Filling (one writer thread): adds the bytes at the end
	[[nodiscard]] bool append(std::span<const uint8_t> p_Bytes, std::string& p_Error);
	// No more bytes will come, and what is missing never will: waiting readers are let go
	void abandon();
	[[nodiscard]] bool complete() const { return available() >= m_Size; }

private:
	struct Native;
	FileSource();

	std::unique_ptr<Native> m_Native;
	std::filesystem::path m_Path;
	uint64_t m_Size = 0;
	int64_t m_Modified = 0;
	bool m_Temporary = false;
	std::atomic<uint64_t> m_Available{ 0 };
	mutable std::atomic<bool> m_Failed{ false };
	mutable std::mutex m_Mutex;
	mutable std::condition_variable m_Arrived;
};

// A stretch of another source, e.g. a picture that sits inside a board file
class RangeSource final : public ByteSource
{
public:
	RangeSource(std::shared_ptr<const ByteSource> p_Base, uint64_t p_Offset, uint64_t p_Size) : m_Base(std::move(p_Base)), m_Offset(p_Offset), m_Size(p_Size) {}

	[[nodiscard]] uint64_t size() const override { return m_Size; }
	size_t read(uint64_t p_Offset, void* p_Target, size_t p_Count, const std::atomic<bool>* p_Cancel = nullptr) const override;
	[[nodiscard]] uint64_t available() const override;
	[[nodiscard]] bool failed() const override { return m_Base->failed(); }
	[[nodiscard]] std::filesystem::path path() const override { return m_Base->path(); }
	[[nodiscard]] uint64_t fileOffset() const override { return m_Base->fileOffset() + m_Offset; }
	[[nodiscard]] bool temporary() const override { return m_Base->temporary(); }
	[[nodiscard]] int64_t modified() const override { return m_Base->modified(); }
	[[nodiscard]] const std::shared_ptr<const ByteSource>& base() const { return m_Base; }

private:
	std::shared_ptr<const ByteSource> m_Base;
	uint64_t m_Offset;
	uint64_t m_Size;
};

// ---- helpers

// Hands p_Count bytes from p_Offset to p_Sink in pieces of about p_Block; false when the sink refuses or a read falls short
using ByteSink = std::function<bool(const uint8_t* p_Data, size_t p_Size)>;
[[nodiscard]] bool streamBytes(const ByteSource& p_Source, uint64_t p_Offset, uint64_t p_Count, const ByteSink& p_Sink, size_t p_Block = size_t{ 1 } << 20, const std::atomic<bool>* p_Cancel = nullptr);
// The whole source in memory; empty when it is bigger than p_Max or unreadable
[[nodiscard]] std::vector<uint8_t> readAllBytes(const ByteSource& p_Source, uint64_t p_Max = uint64_t{ 1 } << 30);
// Writes the source to p_Path through a temporary file and a rename
[[nodiscard]] IoResult copySourceToFile(const ByteSource& p_Source, const std::filesystem::path& p_Path);
// A cheap fingerprint of the content (size, start, middle and end) for finding duplicates
[[nodiscard]] uint64_t fingerprint(const ByteSource& p_Source);
[[nodiscard]] bool sameBytes(const ByteSource& p_A, const ByteSource& p_B);
} // namespace wb
