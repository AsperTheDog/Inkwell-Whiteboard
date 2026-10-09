// App: PDF import. The file is drawn page by page on a worker thread; the pages then land on the board as locked pictures.
module;
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <volk.h>

module wb.app;

import wb.doc.document;
import wb.editor;
import wb.io.file;
import wb.math;
import wb.platform.pdf;
import wb.tools.tool;

namespace wb
{
namespace
{
constexpr uint64_t MAX_PDF_BYTES = 512ull << 20;
} // namespace

void App::startPdfImport(const std::filesystem::path& p_Path)
{
	if (m_PdfJob.valid())
	{
		showToast("Another PDF is still being drawn");
		return;
	}
	std::vector<uint8_t> l_Bytes;
	if (const IoResult l_Read = readFile(p_Path, l_Bytes, MAX_PDF_BYTES); !l_Read.ok)
	{
		showMessage("The PDF could not be read.\n\n" + l_Read.error);
		return;
	}
	m_PdfName = pathToUtf8(p_Path.filename());
	m_PdfJob = std::async(std::launch::async, [l_Bytes = std::move(l_Bytes)] { return platform::importPdf(l_Bytes); });
	showToast("Drawing " + m_PdfName + "...");
}

// Once per loop: puts a finished PDF on the board
void App::pollPdfImport()
{
	if (!m_PdfJob.valid())
		return;
	if (m_PdfJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
	{
		requestRedraw(1); // keeps the loop awake so the result is noticed
		return;
	}
	platform::PdfImport l_Import = m_PdfJob.get();
	if (m_Editor.isBusy())
	{
		// Something is being drawn right now: land it a moment later instead of dropping it
		m_PdfJob = std::async(std::launch::deferred, [l_Import = std::move(l_Import)]() mutable { return std::move(l_Import); });
		requestRedraw(1);
		return;
	}
	if (!l_Import.error.empty())
	{
		showMessage(l_Import.error);
		return;
	}

	std::vector<Editor::PageImage> l_Pages;
	l_Pages.reserve(l_Import.pages.size());
	for (size_t i = 0; i < l_Import.pages.size(); ++i)
	{
		platform::PdfPagePicture& l_Page = l_Import.pages[i];
		Editor::PageImage l_Image;
		l_Image.asset.width = l_Page.width;
		l_Image.asset.height = l_Page.height;
		l_Image.asset.name = m_PdfName + " page " + std::to_string(i + 1);
		l_Image.asset.bytes = std::move(l_Page.png);
		l_Image.size = l_Page.size;
		l_Pages.push_back(std::move(l_Image));
	}
	const size_t l_Count = l_Pages.size();
	m_Editor.insertPages(std::move(l_Pages));
	if (l_Import.totalPages > l_Count)
		showToast("Added the first " + std::to_string(l_Count) + " of " + std::to_string(l_Import.totalPages) + " pages. They are locked: Ctrl+L unlocks");
	else
		showToast("Added " + std::to_string(l_Count) + (l_Count == 1 ? " page" : " pages") + ". They are locked: select one and press Ctrl+L to unlock");
	requestRedraw();
}
} // namespace wb
