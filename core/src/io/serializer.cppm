// The native .wbrd board format.
//
//   file   = magic(8) version(u32) chunk*
//   chunk  = tag(4 chars) length(u64) crc32-of-payload(u32) payload
//   chunks = META (id counter, object count, origin path), VIEW (camera), OBJS (objects), END (must be last)
//
// Everything is little endian. Readers skip chunks they do not know and objects of types they do not know, so newer
// files with extra data still open. Every read is bounds checked: corrupt or truncated input is reported as an
// error and leaves the target document untouched.
module;
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

export module wb.io.serializer;

import wb.math;
import wb.doc.document;
import wb.doc.object;
import wb.io.file;

export namespace wb
{
inline constexpr uint32_t BOARD_FORMAT_VERSION = 1;

// Per-file state that is not part of the document itself
struct BoardMeta
{
	DVec2 viewCenter{ 0.0 };
	double viewZoom = 1.0;
	// Set in autosave files: the file the board was opened from (empty for an untitled board)
	std::string sourcePath;
};

struct LoadResult
{
	bool ok = false;
	std::string error; // human readable, set when !ok
	uint32_t skippedObjects = 0; // objects of types this version does not know
};

[[nodiscard]] uint32_t crc32(std::span<const uint8_t> p_Bytes);

// A board prepared for a background write. Files too big to copy cheaply (videos, large pictures) are not copied into
// `head`: `big` points at the document's own bytes, which are immutable, so a worker thread can write them while the
// board keeps changing. The document must outlive the write and must not be cleared until it has finished.
struct BigAssetRef
{
	AssetId id = INVALID_ASSET_ID;
	uint32_t width = 0;
	uint32_t height = 0;
	std::string name;
	const uint8_t* data = nullptr;
	size_t size = 0;
};

struct SplitBoard
{
	std::vector<uint8_t> head; // everything but the big assets and the closing chunk
	std::vector<uint8_t> tail; // the closing chunk
	std::vector<BigAssetRef> big;
};

// Assets of at least p_BigAssetBytes go to `big`
[[nodiscard]] SplitBoard serializeBoardSplit(const Document& p_Document, const BoardMeta& p_Meta, size_t p_BigAssetBytes);
// Writes the parts as one file, through a temporary file and a rename like writeFileAtomic. Safe to call off-thread.
[[nodiscard]] IoResult writeSplitBoardAtomic(const std::filesystem::path& p_Path, const SplitBoard& p_Board);

[[nodiscard]] std::vector<uint8_t> serializeBoard(const Document& p_Document, const BoardMeta& p_Meta);
// Replaces the document content (listeners get onDocumentCleared + onObjectAdded). On failure nothing is touched.
[[nodiscard]] LoadResult deserializeBoard(std::span<const uint8_t> p_Bytes, Document& p_Document, BoardMeta& p_Meta);
} // namespace wb
