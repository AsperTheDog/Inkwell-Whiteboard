// App: getting pictures onto the board (dialog, paste, drop), recompressing them and controlling animations.
module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
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
import wb.io.file;
import wb.math;
import wb.platform.clipboard;
import wb.platform.input;
import wb.render.canvas_renderer;
import wb.render.image_store;
import wb.session;
import wb.tools.tool;
import wb.view.camera;

namespace wb
{
namespace
{
constexpr double CASCADE_POINTS = 28.0; // offset between several pictures inserted together
constexpr uint64_t MAX_PICTURE_BYTES = 512ull << 20;

std::string formatSize(const size_t p_Bytes)
{
	char l_Buffer[32];
	if (p_Bytes >= (1u << 20))
		std::snprintf(l_Buffer, sizeof(l_Buffer), "%.1f MB", static_cast<double>(p_Bytes) / (1024.0 * 1024.0));
	else
		std::snprintf(l_Buffer, sizeof(l_Buffer), "%.0f KB", static_cast<double>(p_Bytes) / 1024.0);
	return l_Buffer;
}
} // namespace

void App::importPictures(std::vector<platform::ClipboardPicture> p_Pictures, const std::optional<Vec2> p_ScreenPosition)
{
	if (m_Editor.isBusy() || p_Pictures.empty())
		return;
	const Camera& l_Camera = m_Editor.camera();
	const Vec2 l_Screen = p_ScreenPosition.value_or(Vec2{ m_Ui.viewport() * 0.5f });
	const DVec2 l_Center = l_Camera.screenToWorld(DVec2{ l_Screen });

	std::vector<ObjectId> l_Inserted;
	int l_Rejected = 0;
	for (platform::ClipboardPicture& l_Picture : p_Pictures)
	{
		const std::optional<image::ImageInfo> l_Info = image::probe(l_Picture.bytes);
		if (!l_Info)
		{
			++l_Rejected;
			continue;
		}
		ImageAsset l_Asset;
		l_Asset.width = l_Info->width;
		l_Asset.height = l_Info->height;
		l_Asset.name = std::move(l_Picture.name);
		l_Asset.bytes = std::move(l_Picture.bytes);
		const double l_Shift = CASCADE_POINTS * static_cast<double>(l_Inserted.size()) / l_Camera.zoom();
		const ObjectId l_Id = m_Editor.insertPicture(std::move(l_Asset), l_Center + DVec2{ l_Shift });
		if (l_Id != INVALID_OBJECT_ID)
			l_Inserted.push_back(l_Id);
	}

	if (!l_Inserted.empty())
	{
		m_Editor.selection().set(l_Inserted);
		m_Editor.setTool(tools::ToolKind::Select); // ready to move and resize it
		if (l_Rejected > 0)
			showToast(std::to_string(l_Rejected) + (l_Rejected == 1 ? " file was not a picture" : " files were not pictures"));
	}
	else if (l_Rejected > 0)
	{
		showToast("That is not a picture Whiteboard can read");
	}
	requestRedraw();
}

bool App::pasteFromSystemClipboard()
{
	if (m_Editor.isBusy() || !platform::clipboardMayHavePictures())
		return false;
	std::vector<platform::ClipboardPicture> l_Pictures = platform::readClipboardPictures(MAX_PICTURE_BYTES);
	std::erase_if(l_Pictures, [](const platform::ClipboardPicture& p_Picture) { return !image::probe(p_Picture.bytes); });
	if (l_Pictures.empty())
		return false;
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
	static const SDL_DialogFileFilter s_Filters[] = { { "Pictures", "png;jpg;jpeg;gif;bmp;tga;psd" }, { "All files", "*" } };
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
			showToast("Save this board first, then open the other one with Ctrl+O");
		else
			openPath(l_Path);
		return;
	}

	platform::ClipboardPicture l_Picture;
	if (const IoResult l_Read = readFile(l_Path, l_Picture.bytes, MAX_PICTURE_BYTES); !l_Read.ok)
	{
		showToast("That file could not be read");
		return;
	}
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
	Document& l_Document = m_Editor.document();
	std::unordered_map<AssetId, AssetId> l_Replacements;
	size_t l_Before = 0;
	size_t l_After = 0;
	std::string l_Reason;
	for (const ObjectId l_Id : m_Editor.selection().ids())
	{
		const Object* l_Object = l_Document.find(l_Id);
		const ImageData* l_Image = l_Object != nullptr ? l_Object->image() : nullptr;
		if (l_Image == nullptr || l_Replacements.contains(l_Image->asset))
			continue;
		const ImageAsset* l_Asset = l_Document.findAsset(l_Image->asset);
		if (l_Asset == nullptr)
			continue;
		std::string l_Error;
		std::optional<image::Recompressed> l_Result = image::recompress(l_Asset->bytes, 4096, 85, l_Error);
		if (!l_Result)
		{
			l_Reason = l_Error;
			continue;
		}
		ImageAsset l_New;
		l_New.width = l_Result->width;
		l_New.height = l_Result->height;
		l_New.name = pathToUtf8(pathFromUtf8(l_Asset->name).stem()) + "." + l_Result->extension;
		l_Before += l_Asset->bytes.size();
		l_After += l_Result->bytes.size();
		l_New.bytes = std::move(l_Result->bytes);
		l_Replacements[l_Image->asset] = l_Document.addAsset(std::move(l_New));
	}

	if (l_Replacements.empty())
	{
		showToast(l_Reason.empty() ? "Nothing to reduce in the selection" : l_Reason);
		return;
	}
	m_Editor.editSelectedImages("Reduce picture size", [&](ImageData& p_Image)
	{
		const auto l_It = l_Replacements.find(p_Image.asset);
		if (l_It == l_Replacements.end())
			return false;
		p_Image.asset = l_It->second;
		return true;
	});
	showToast("Reduced from " + formatSize(l_Before) + " to " + formatSize(l_After));
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
