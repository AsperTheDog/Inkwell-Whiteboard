module;
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <fpdfview.h>
#include <stb_image_write.h>

module wb.platform.pdf;

import wb.math;

namespace wb::platform
{
namespace
{
constexpr double UNITS_PER_POINT = 96.0 / 72.0; // a PDF point is 1/72 inch, a world unit 1/96 inch
constexpr double MAX_PAGE_PIXELS_LONG_SIDE = 3000.0;

void appendPng(void* p_Context, void* p_Data, const int p_Size)
{
	auto* l_Out = static_cast<std::vector<uint8_t>*>(p_Context);
	const auto* l_Bytes = static_cast<const uint8_t*>(p_Data);
	l_Out->insert(l_Out->end(), l_Bytes, l_Bytes + p_Size);
}

void ensureLibrary()
{
	static std::once_flag s_Once;
	std::call_once(s_Once, [] { FPDF_InitLibrary(); });
}

std::string describeError(const unsigned long p_Code)
{
	switch (p_Code)
	{
	case FPDF_ERR_PASSWORD:
		return "This PDF is protected with a password.";
	case FPDF_ERR_FORMAT:
		return "This file is not a PDF Whiteboard can read.";
	case FPDF_ERR_FILE:
		return "The PDF could not be opened.";
	default:
		return "The PDF could not be read.";
	}
}
} // namespace

PdfImport importPdf(const std::vector<uint8_t>& p_Bytes)
{
	PdfImport l_Result;
	ensureLibrary();
	FPDF_DOCUMENT l_Document = FPDF_LoadMemDocument(p_Bytes.data(), static_cast<int>(p_Bytes.size()), nullptr);
	if (l_Document == nullptr)
	{
		l_Result.error = describeError(FPDF_GetLastError());
		return l_Result;
	}

	const int l_Count = FPDF_GetPageCount(l_Document);
	l_Result.totalPages = static_cast<uint32_t>(std::max(l_Count, 0));
	const int l_Used = std::min<int>(l_Count, static_cast<int>(MAX_PDF_PAGES));
	// Pixels per PDF point: sharp enough to read at 200%, lighter for long documents
	const double l_Resolution = l_Used <= 12 ? 2.0 : (l_Used <= 40 ? 1.5 : 1.2);

	for (int i = 0; i < l_Used; ++i)
	{
		FPDF_PAGE l_Page = FPDF_LoadPage(l_Document, i);
		if (l_Page == nullptr)
			continue;
		const double l_WidthPoints = static_cast<double>(FPDF_GetPageWidthF(l_Page));
		const double l_HeightPoints = static_cast<double>(FPDF_GetPageHeightF(l_Page));
		if (l_WidthPoints > 0.0 && l_HeightPoints > 0.0)
		{
			double l_Scale = l_Resolution;
			l_Scale = std::min(l_Scale, MAX_PAGE_PIXELS_LONG_SIDE / std::max(l_WidthPoints, l_HeightPoints));
			const int l_Width = std::max(1, static_cast<int>(std::lround(l_WidthPoints * l_Scale)));
			const int l_Height = std::max(1, static_cast<int>(std::lround(l_HeightPoints * l_Scale)));

			FPDF_BITMAP l_Bitmap = FPDFBitmap_Create(l_Width, l_Height, 0);
			if (l_Bitmap != nullptr)
			{
				FPDFBitmap_FillRect(l_Bitmap, 0, 0, l_Width, l_Height, 0xFFFFFFFFu);
				FPDF_RenderPageBitmap(l_Bitmap, l_Page, 0, 0, l_Width, l_Height, 0, FPDF_ANNOT);

				// BGRx to RGB
				const auto* l_Source = static_cast<const uint8_t*>(FPDFBitmap_GetBuffer(l_Bitmap));
				const int l_Stride = FPDFBitmap_GetStride(l_Bitmap);
				std::vector<uint8_t> l_Rgb(static_cast<size_t>(l_Width) * static_cast<size_t>(l_Height) * 3);
				for (int y = 0; y < l_Height; ++y)
				{
					const uint8_t* l_Row = l_Source + static_cast<size_t>(y) * static_cast<size_t>(l_Stride);
					uint8_t* l_Out = l_Rgb.data() + static_cast<size_t>(y) * static_cast<size_t>(l_Width) * 3;
					for (int x = 0; x < l_Width; ++x)
					{
						l_Out[x * 3 + 0] = l_Row[x * 4 + 2];
						l_Out[x * 3 + 1] = l_Row[x * 4 + 1];
						l_Out[x * 3 + 2] = l_Row[x * 4 + 0];
					}
				}
				FPDFBitmap_Destroy(l_Bitmap);

				PdfPagePicture l_Picture;
				l_Picture.width = static_cast<uint32_t>(l_Width);
				l_Picture.height = static_cast<uint32_t>(l_Height);
				l_Picture.size = Vec2{ static_cast<float>(l_WidthPoints * UNITS_PER_POINT), static_cast<float>(l_HeightPoints * UNITS_PER_POINT) };
				if (stbi_write_png_to_func(&appendPng, &l_Picture.png, l_Width, l_Height, 3, l_Rgb.data(), l_Width * 3) != 0)
					l_Result.pages.push_back(std::move(l_Picture));
			}
		}
		FPDF_ClosePage(l_Page);
	}
	FPDF_CloseDocument(l_Document);
	if (l_Result.pages.empty())
		l_Result.error = "No page of this PDF could be drawn.";
	return l_Result;
}
} // namespace wb::platform
