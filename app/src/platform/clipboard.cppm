// Pictures on the system clipboard: copied image data (PNG, bitmaps) and copied image files.
module;
#include <cstdint>
#include <string>
#include <vector>

export module wb.platform.clipboard;

export namespace wb::platform
{
struct ClipboardPicture
{
	std::vector<uint8_t> bytes; // a complete image file (PNG, JPEG, GIF, BMP...)
	std::string name;
};

// Cheap check: the clipboard holds something readPictures() may turn into pictures
[[nodiscard]] bool clipboardMayHavePictures();

// Everything picture-like on the clipboard. Files are read from disk (up to p_MaxBytes each). Files that are not
// pictures are returned too: the caller probes them.
[[nodiscard]] std::vector<ClipboardPicture> readClipboardPictures(uint64_t p_MaxBytes = 512ull * 1024 * 1024);
} // namespace wb::platform
