// App: getting pictures onto the board (dialog, paste, drop), recompressing them and controlling animations.
module;
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
#include <imgui.h>
#include <spdlog/spdlog.h>
#include <volk.h>

module wb.app;

import wb.doc.document;
import wb.doc.object;
import wb.editor;
import wb.image.codec;
import wb.io.blob;
import wb.io.file;
import wb.io.locale;
import wb.math;
import wb.platform.clipboard;
import wb.platform.input;
import wb.render.canvas_renderer;
import wb.render.image_store;
import wb.session;
import wb.text.utf8;
import wb.tools.tool;
import wb.video.media;
import wb.view.camera;

namespace wb
{
namespace
{
constexpr double CASCADE_POINTS = 28.0; // offset between several pictures inserted together
constexpr size_t MAX_TEXT_BYTES = 2u << 20; // a text file becomes one text block, so it has to stay editable
constexpr uint64_t MAX_PICTURE_BYTES = 512ull << 20; // pictures are decoded in memory; videos are played from their file and have no limit

std::string formatSize(const size_t p_Bytes)
{
	char l_Buffer[32];
	if (p_Bytes >= (1u << 20))
		std::snprintf(l_Buffer, sizeof(l_Buffer), "%.1f MB", static_cast<double>(p_Bytes) / (1024.0 * 1024.0));
	else
		std::snprintf(l_Buffer, sizeof(l_Buffer), "%.0f KB", static_cast<double>(p_Bytes) / 1024.0);
	return l_Buffer;
}

// The text in a file made of text, or nothing when it is binary. UTF-8 (with or without a BOM) and UTF-16 are read as they are;
// anything else without NUL bytes is taken to be an old 8-bit encoding and each byte is read as its Latin-1 character.
std::optional<std::string> decodeTextFile(const std::vector<uint8_t>& p_Bytes)
{
	const size_t l_Size = p_Bytes.size();
	if (l_Size >= 2 && ((p_Bytes[0] == 0xFF && p_Bytes[1] == 0xFE) || (p_Bytes[0] == 0xFE && p_Bytes[1] == 0xFF)))
	{
		const bool l_Little = p_Bytes[0] == 0xFF;
		const auto l_Unit = [&](const size_t p_At) -> uint32_t { return l_Little ? (p_Bytes[p_At] | (p_Bytes[p_At + 1] << 8)) : ((p_Bytes[p_At] << 8) | p_Bytes[p_At + 1]); };
		std::string l_Out;
		for (size_t i = 2; i + 1 < l_Size; i += 2)
		{
			uint32_t l_Code = l_Unit(i);
			if (l_Code >= 0xD800 && l_Code < 0xDC00 && i + 3 < l_Size)
			{
				const uint32_t l_Low = l_Unit(i + 2);
				if (l_Low >= 0xDC00 && l_Low < 0xE000)
				{
					l_Code = 0x10000 + ((l_Code - 0xD800) << 10) + (l_Low - 0xDC00);
					i += 2;
				}
			}
			text::appendCodepoint(l_Out, l_Code);
		}
		return l_Out;
	}

	const size_t l_Skip = l_Size >= 3 && p_Bytes[0] == 0xEF && p_Bytes[1] == 0xBB && p_Bytes[2] == 0xBF ? 3 : 0;
	if (std::find(p_Bytes.begin() + static_cast<std::ptrdiff_t>(l_Skip), p_Bytes.end(), uint8_t{ 0 }) != p_Bytes.end())
		return std::nullopt;

	bool l_Valid = true;
	for (size_t i = l_Skip; i < l_Size && l_Valid;)
	{
		const uint8_t l_Lead = p_Bytes[i];
		const size_t l_Extra = l_Lead >= 0xF0 ? 3 : l_Lead >= 0xE0 ? 2 : l_Lead >= 0xC0 ? 1 : 0;
		if ((l_Lead >= 0x80 && l_Lead < 0xC2) || l_Lead >= 0xF8 || i + l_Extra >= l_Size + (l_Extra == 0 ? 1 : 0))
		{
			l_Valid = false;
			break;
		}
		for (size_t k = 1; k <= l_Extra; ++k)
			l_Valid = l_Valid && (p_Bytes[i + k] & 0xC0) == 0x80;
		i += l_Extra + 1;
	}
	if (l_Valid)
		return std::string(p_Bytes.begin() + static_cast<std::ptrdiff_t>(l_Skip), p_Bytes.end());
	std::string l_Out;
	for (size_t i = l_Skip; i < l_Size; ++i)
		text::appendCodepoint(l_Out, p_Bytes[i]);
	return l_Out;
}
} // namespace

namespace
{
constexpr uint64_t BACKGROUND_IMPORT_BYTES = 8ull << 20; // files from this size up are read on a worker so the window stays alive
} // namespace

// Opens and reads one picture, video or text file. Touches no shared state, so it runs on a worker too.
App::PreparedPicture App::preparePicture(platform::ClipboardPicture p_Picture)
{
	PreparedPicture l_Result;
	// A file on disk is opened, not read: a big one stays where it is and a video is played straight from it
	if (p_Picture.bytes.empty() && !p_Picture.path.empty())
	{
		std::string l_Error;
		const std::shared_ptr<FileSource> l_File = FileSource::open(p_Picture.path, l_Error);
		if (!l_File || l_File->size() == 0)
		{
			l_Result.kind = PreparedPicture::Kind::Unreadable;
			return l_Result;
		}
		if (p_Picture.name.empty())
			p_Picture.name = pathToUtf8(p_Picture.path.filename());
		if (l_File->size() > LARGE_ASSET_BYTES)
		{
			if (const std::optional<video::VideoInfo> l_Video = video::probe(*l_File))
			{
				l_Result.kind = PreparedPicture::Kind::Video;
				l_Result.asset.width = l_Video->width;
				l_Result.asset.height = l_Video->height;
				l_Result.asset.name = std::move(p_Picture.name);
				l_Result.asset.stream = l_File;
				return l_Result;
			}
			if (l_File->size() > MAX_PICTURE_BYTES)
			{
				l_Result.kind = PreparedPicture::Kind::Rejected;
				return l_Result;
			}
		}
		p_Picture.bytes = readAllBytes(*l_File, MAX_PICTURE_BYTES);
		if (p_Picture.bytes.empty())
		{
			l_Result.kind = PreparedPicture::Kind::Unreadable;
			return l_Result;
		}
	}
	if (const std::optional<image::ImageInfo> l_Info = image::probe(p_Picture.bytes))
	{
		l_Result.kind = PreparedPicture::Kind::Picture;
		l_Result.asset.width = l_Info->width;
		l_Result.asset.height = l_Info->height;
		l_Result.asset.name = std::move(p_Picture.name);
		l_Result.asset.bytes = std::move(p_Picture.bytes);
		return l_Result;
	}
	// Not a picture: perhaps a video
	if (const std::optional<video::VideoInfo> l_Video = video::probe(p_Picture.bytes))
	{
		l_Result.kind = PreparedPicture::Kind::Video;
		l_Result.asset.width = l_Video->width;
		l_Result.asset.height = l_Video->height;
		l_Result.asset.name = std::move(p_Picture.name);
		l_Result.asset.bytes = std::move(p_Picture.bytes);
		return l_Result;
	}
	// Or a file made of text (.txt, .md, .cpp...), which becomes a block of text
	if (p_Picture.bytes.size() <= MAX_TEXT_BYTES)
	{
		if (std::optional<std::string> l_Content = decodeTextFile(p_Picture.bytes))
		{
			l_Result.kind = PreparedPicture::Kind::Text;
			l_Result.text = std::move(*l_Content);
			return l_Result;
		}
	}
	l_Result.kind = PreparedPicture::Kind::Rejected;
	return l_Result;
}

void App::importPictures(std::vector<platform::ClipboardPicture> p_Pictures, const std::optional<Vec2> p_ScreenPosition)
{
	if (m_Editor.isBusy() || p_Pictures.empty())
		return;
	const Camera& l_Camera = m_Editor.camera();
	const Vec2 l_Screen = p_ScreenPosition.value_or(Vec2{ m_Ui.viewport() * 0.5f });
	const DVec2 l_Center = l_Camera.screenToWorld(DVec2{ l_Screen });

	// Big files are read on a worker; whatever comes after them waits its turn so the order is kept
	bool l_Background = !m_PictureJobs.empty();
	for (const platform::ClipboardPicture& l_Picture : p_Pictures)
	{
		std::error_code l_Error;
		if (l_Picture.bytes.empty() && !l_Picture.path.empty() && std::filesystem::file_size(l_Picture.path, l_Error) >= BACKGROUND_IMPORT_BYTES)
			l_Background = true;
	}
	if (l_Background)
	{
		PictureBatch l_Batch;
		l_Batch.center = l_Center;
		l_Batch.zoom = l_Camera.zoom();
		l_Batch.label = p_Pictures.size() == 1 && !p_Pictures.front().path.empty() ? pathToUtf8(p_Pictures.front().path.filename()) : tr("images.loadingCount", { std::to_string(p_Pictures.size()) });
		l_Batch.work = std::async(std::launch::async, [l_Pictures = std::move(p_Pictures)]() mutable
		{
			std::vector<PreparedPicture> l_Prepared;
			l_Prepared.reserve(l_Pictures.size());
			for (platform::ClipboardPicture& l_Picture : l_Pictures)
				l_Prepared.push_back(preparePicture(std::move(l_Picture)));
			return l_Prepared;
		});
		m_PictureJobs.push_back(std::move(l_Batch));
		requestRedraw();
		return;
	}

	std::vector<PreparedPicture> l_Prepared;
	l_Prepared.reserve(p_Pictures.size());
	for (platform::ClipboardPicture& l_Picture : p_Pictures)
		l_Prepared.push_back(preparePicture(std::move(l_Picture)));
	finishPictureImport(std::move(l_Prepared), l_Center, l_Camera.zoom());
}

// Once per loop: puts the pictures a worker has finished reading on the board
void App::pollPictureImport()
{
	while (!m_PictureJobs.empty())
	{
		PictureBatch& l_Front = m_PictureJobs.front();
		if (l_Front.work.wait_for(std::chrono::seconds(0)) != std::future_status::ready || m_Editor.isBusy())
		{
			requestRedraw(1); // keeps the loop awake (and the spinner turning) so the result is noticed
			return;
		}
		std::vector<PreparedPicture> l_Prepared = l_Front.work.get();
		const DVec2 l_Center = l_Front.center;
		const double l_Zoom = l_Front.zoom;
		m_PictureJobs.pop_front();
		finishPictureImport(std::move(l_Prepared), l_Center, l_Zoom);
	}
}

// Puts what was read on the board (on the UI thread) and tells how it went
void App::finishPictureImport(std::vector<PreparedPicture> p_Pictures, const DVec2 p_Center, const double p_Zoom)
{
	std::vector<ObjectId> l_Inserted;
	int l_Rejected = 0;
	int l_Unreadable = 0;
	for (PreparedPicture& l_Picture : p_Pictures)
	{
		if (l_Picture.kind == PreparedPicture::Kind::Unreadable)
		{
			++l_Unreadable;
			continue;
		}
		const double l_Shift = CASCADE_POINTS * static_cast<double>(l_Inserted.size()) / p_Zoom;
		const DVec2 l_At = p_Center + DVec2{ l_Shift };
		ObjectId l_Id = INVALID_OBJECT_ID;
		switch (l_Picture.kind)
		{
		case PreparedPicture::Kind::Picture:
			l_Id = m_Editor.insertPicture(std::move(l_Picture.asset), l_At);
			break;
		case PreparedPicture::Kind::Video:
			l_Id = m_Editor.insertVideo(std::move(l_Picture.asset), l_At);
			break;
		case PreparedPicture::Kind::Text:
			l_Id = m_Editor.insertText(std::move(l_Picture.text), l_At);
			break;
		default:
			break;
		}
		if (l_Id != INVALID_OBJECT_ID)
			l_Inserted.push_back(l_Id);
		else
			++l_Rejected;
	}

	if (!l_Inserted.empty())
	{
		m_Editor.selection().set(l_Inserted);
		m_Editor.setTool(tools::ToolKind::Select); // ready to move and resize it
		if (l_Rejected > 0)
			showToast(tr(l_Rejected == 1 ? "images.rejected.one" : "images.rejected.many", { std::to_string(l_Rejected) }), true);
		else if (l_Unreadable > 0)
			showToast(tr(l_Unreadable == 1 ? "images.unreadable.one" : "images.unreadable.many", { std::to_string(l_Unreadable) }), true);
	}
	else if (l_Unreadable > 0)
	{
		showToast(tr(l_Unreadable == 1 ? "images.thatUnreadable" : "images.thoseUnreadable"), true);
	}
	else if (l_Rejected > 0)
	{
		showToast(tr("images.unsupported"), true);
	}
	requestRedraw();
}

bool App::pasteFromSystemClipboard()
{
	if (m_Editor.isBusy() || m_Editor.textEditing())
		return false;
	std::vector<platform::ClipboardPicture> l_Pictures;
	if (platform::clipboardMayHavePictures())
	{
		l_Pictures = platform::readClipboardPictures(MAX_PICTURE_BYTES);
		// Pictures and videos stay (a big file is only looked at when it is inserted)
		std::erase_if(l_Pictures, [](const platform::ClipboardPicture& p_Picture) { return p_Picture.bytes.empty() ? p_Picture.path.empty() : (!image::probe(p_Picture.bytes) && !video::probe(p_Picture.bytes)); });
	}
	if (l_Pictures.empty())
	{
		// Plain text from another program becomes a text box
		if (!SDL_HasClipboardText())
			return false;
		char* l_Text = SDL_GetClipboardText();
		if (l_Text == nullptr)
			return false;
		const std::string l_Pasted(l_Text);
		SDL_free(l_Text);
		const DVec2 l_Viewport = m_Editor.camera().viewport();
		const bool l_Over = m_PointerInWindow && !m_CursorOverUi && m_LastPointer;
		const DVec2 l_Screen = l_Over ? DVec2{ m_LastPointer->position } : l_Viewport * 0.5;
		return m_Editor.insertText(l_Pasted, m_Editor.camera().screenToWorld(l_Screen)) != INVALID_OBJECT_ID;
	}
	// Where the pointer is, unless it is over the interface or outside the window
	std::optional<Vec2> l_Position;
	if (m_PointerInWindow && !m_CursorOverUi && m_LastPointer)
		l_Position = m_LastPointer->position;
	importPictures(std::move(l_Pictures), l_Position);
	return true;
}

void App::showInsertPictureDialog()
{
	if (m_DialogKind != DialogKind::None || m_Editor.isBusy())
		return;
	static SDL_DialogFileFilter s_Filters[2]; // read while the dialog is open, so it has to outlive this call
	s_Filters[0] = { tr("images.files").c_str(), "png;jpg;jpeg;gif;webp;bmp;tga;psd;pdf;mp4;m4v;mov;webm;mkv;avi;ogv;mpg;mpeg;ts" };
	s_Filters[1] = { tr("common.allFiles").c_str(), "*" };
	m_DialogKind = DialogKind::Pictures;
	SDL_ShowOpenFileDialog(&App::dialogCallback, this, m_Window.handle(), s_Filters, 2, m_LastDirectory.empty() ? nullptr : m_LastDirectory.c_str(), true);
}

void App::handleDroppedFile(const SDL_DropEvent& p_Event)
{
	if (p_Event.data == nullptr || m_Options.smokeTestFrames > 0)
		return;
	const std::filesystem::path l_Path = pathFromUtf8(p_Event.data);
	if (l_Path.extension() == ".wbrd")
	{
		if (m_Session.dirty())
			showToast(tr("images.saveFirst"));
		else
			openPath(l_Path);
		return;
	}

	if (const std::string l_Extension = l_Path.extension().string(); l_Extension == ".ttf" || l_Extension == ".otf" || l_Extension == ".ttc" || l_Extension == ".TTF" || l_Extension == ".OTF")
	{
		importFontFiles({ std::string(p_Event.data) });
		return;
	}

	if (const std::string l_Extension = l_Path.extension().string(); l_Extension == ".pdf" || l_Extension == ".PDF")
	{
		startPdfImport(l_Path);
		return;
	}

	if (std::string l_Extension = pathToUtf8(l_Path.extension()); std::ranges::any_of(std::array<const char*, 8>{ ".srt", ".SRT", ".vtt", ".VTT", ".ass", ".ASS", ".ssa", ".SSA" }, [&](const char* p_Known) { return l_Extension == p_Known; }))
	{
		const float l_Scale = m_Window.pixelDensity();
		if (!dropSubtitleFile(l_Path, Vec2{ p_Event.x, p_Event.y } * l_Scale))
			showToast(tr("images.dropSubtitles"));
		return;
	}

	platform::ClipboardPicture l_Picture;
	l_Picture.path = l_Path; // opened when it is inserted
	l_Picture.name = pathToUtf8(l_Path.filename());

	const float l_Density = m_Window.pixelDensity();
	Vec2 l_Position = Vec2{ p_Event.x, p_Event.y } * l_Density;
	l_Position += Vec2{ static_cast<float>(CASCADE_POINTS * static_cast<double>(m_DropCount++)) * m_Window.displayScale() };
	std::vector<platform::ClipboardPicture> l_Pictures;
	l_Pictures.push_back(std::move(l_Picture));
	importPictures(std::move(l_Pictures), l_Position);
}

void App::compressSelectedPictures()
{
	if (m_Editor.isBusy())
		return;
	if (m_ReduceJob.valid())
	{
		showToast(tr("images.shrinkBusy"));
		return;
	}
	Document& l_Document = m_Editor.document();
	// Each file once, whatever number of objects show it. The worker gets its own copy of the bytes (or the stream, for a big file).
	std::vector<std::pair<AssetId, std::shared_ptr<const ByteSource>>> l_Files;
	std::unordered_map<AssetId, std::string> l_Names;
	std::vector<ObjectId> l_Objects;
	for (const ObjectId l_Id : m_Editor.selection().ids())
	{
		const Object* l_Object = l_Document.find(l_Id);
		const ImageData* l_Image = l_Object != nullptr ? l_Object->image() : nullptr;
		if (l_Image == nullptr || m_Canvas.images().animated(l_Image->asset))
			continue;
		const ImageAsset* l_Asset = l_Document.findAsset(l_Image->asset);
		if (l_Asset == nullptr || !l_Asset->complete())
			continue;
		l_Objects.push_back(l_Id);
		if (l_Names.emplace(l_Image->asset, l_Asset->name).second)
			l_Files.emplace_back(l_Image->asset, l_Asset->source());
	}
	if (l_Files.empty())
	{
		showToast(tr("images.nothingToReduce"));
		return;
	}

	m_ReduceJob = std::async(std::launch::async, [l_Files = std::move(l_Files), l_Names = std::move(l_Names), l_Objects = std::move(l_Objects)]() mutable
	{
		ReduceResult l_Out;
		l_Out.objects = std::move(l_Objects);
		for (const auto& [l_AssetId, l_Source] : l_Files)
		{
			std::string l_Error;
			const std::vector<uint8_t> l_Original = readAllBytes(*l_Source, MAX_PICTURE_BYTES);
			std::optional<image::Recompressed> l_Result = l_Original.empty() ? std::nullopt : image::recompress(l_Original, 4096, 85, l_Error);
			if (!l_Result)
			{
				l_Out.reason = l_Error;
				continue;
			}
			ReducedPicture l_Picture;
			l_Picture.from = l_AssetId;
			l_Picture.width = l_Result->width;
			l_Picture.height = l_Result->height;
			l_Picture.name = pathToUtf8(pathFromUtf8(l_Names[l_AssetId]).stem()) + "." + l_Result->extension;
			l_Picture.beforeBytes = l_Original.size();
			l_Picture.bytes = std::move(l_Result->bytes);
			l_Out.pictures.push_back(std::move(l_Picture));
		}
		return l_Out;
	});
	showToast(tr("images.reducing"));
}

// Once per loop: puts the reduced files on the board. Editing the objects is a normal edit, so everybody in a shared board gets
// the new file the way they get any other change of a picture, and undo brings the old one back.
void App::pollReducedPictures()
{
	if (!m_ReduceJob.valid())
		return;
	if (m_ReduceJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
	{
		requestRedraw(1); // keeps the loop awake so the result is noticed
		return;
	}
	if (m_Editor.isBusy())
	{
		requestRedraw(1); // something is being drawn right now: land it a moment later
		return;
	}
	ReduceResult l_Result = m_ReduceJob.get();

	// Only the files that are still shown by one of the pictures (they may have been deleted or edited meanwhile)
	Document& l_Document = m_Editor.document();
	std::unordered_set<AssetId> l_Shown;
	for (const ObjectId l_Id : l_Result.objects)
	{
		if (const Object* l_Object = l_Document.find(l_Id); l_Object != nullptr && l_Object->image() != nullptr)
			l_Shown.insert(l_Object->image()->asset);
	}
	std::unordered_map<AssetId, AssetId> l_Replacements;
	size_t l_Before = 0;
	size_t l_After = 0;
	for (ReducedPicture& l_Picture : l_Result.pictures)
	{
		if (!l_Shown.contains(l_Picture.from))
			continue;
		ImageAsset l_New;
		l_New.width = l_Picture.width;
		l_New.height = l_Picture.height;
		l_New.name = std::move(l_Picture.name);
		l_Before += l_Picture.beforeBytes;
		l_After += l_Picture.bytes.size();
		l_New.bytes = std::move(l_Picture.bytes);
		l_Replacements[l_Picture.from] = l_Document.addAsset(std::move(l_New));
	}

	if (l_Replacements.empty())
	{
		showToast(l_Result.reason.empty() ? tr("images.nothingToReduce") : l_Result.reason);
		return;
	}
	m_Editor.editImages(l_Result.objects, "Reduce picture size", [&](ImageData& p_Image)
	{
		const auto l_It = l_Replacements.find(p_Image.asset);
		if (l_It == l_Replacements.end())
			return false;
		p_Image.asset = l_It->second;
		return true;
	});
	showToast(tr("images.reduced", { formatSize(l_Before), formatSize(l_After) }));
	requestRedraw();
}

void App::toggleSelectedPlayback()
{
	const render::ImageStore& l_Images = m_Canvas.images();
	const render::ImageClock l_Clock = imageClock();
	const Document& l_Document = m_Editor.document();

	bool l_AllPlaying = true;
	bool l_AnyAnimated = false;
	for (const ObjectId l_Id : m_Editor.selection().ids())
	{
		const Object* l_Object = l_Document.find(l_Id);
		const ImageData* l_Image = l_Object != nullptr ? l_Object->image() : nullptr;
		if (l_Image == nullptr || !l_Images.animated(l_Image->asset))
			continue;
		l_AnyAnimated = true;
		l_AllPlaying = l_AllPlaying && l_Image->playing;
	}
	if (!l_AnyAnimated)
		return;

	// Pausing freezes each animation on the frame it shows right now
	m_Editor.editSelectedImages(l_AllPlaying ? "Pause animation" : "Play animation", [&](ImageData& p_Image)
	{
		if (!l_Images.animated(p_Image.asset))
			return false;
		if (l_AllPlaying)
		{
			p_Image.frame = l_Images.frameAt(p_Image.asset, l_Clock);
			p_Image.playing = false;
		}
		else
		{
			p_Image.playing = true;
		}
		return true;
	});
	if (!l_AllPlaying)
		m_PlayAnimations = true; // pressing play must show something moving
	requestRedraw();
}
} // namespace wb
