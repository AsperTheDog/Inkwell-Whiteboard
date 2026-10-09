// Reading PDF files with PDFium: every page is drawn to a picture. PDFium is not thread safe, so use one document
// from one thread at a time (a worker thread doing the whole conversion is fine).
module;
#include <cstdint>
#include <string>
#include <vector>

export module wb.platform.pdf;

import wb.math;

export namespace wb::platform
{
// One page drawn to a PNG
struct PdfPagePicture
{
	std::vector<uint8_t> png;
	uint32_t width = 0;  // pixels
	uint32_t height = 0;
	Vec2 size{ 0.f };    // the page on the board, in world units (1/96 inch)
};

struct PdfImport
{
	std::vector<PdfPagePicture> pages;
	uint32_t totalPages = 0; // in the file; `pages` may hold fewer when the file is longer than the limit
	std::string error;       // set when nothing could be read
};

inline constexpr uint32_t MAX_PDF_PAGES = 120;

// Draws up to MAX_PDF_PAGES pages. The resolution drops for long documents to keep the board light.
[[nodiscard]] PdfImport importPdf(const std::vector<uint8_t>& p_Bytes);
} // namespace wb::platform
