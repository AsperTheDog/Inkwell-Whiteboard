// App: collaboration. Hosting and joining dialogs, the people list, and feeding the session each loop iteration.
module;
#include <algorithm>
#include <array>
#include <cstdint>
#include <chrono>
#include <cmath>
#include <deque>
#include <optional>
#include <thread>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <spdlog/spdlog.h>
#include <volk.h>

module wb.app;

import wb.doc.document;
import wb.doc.object;
import wb.editor;
import wb.image.codec;
import wb.io.blob;
import wb.io.file;
import wb.math;
import wb.net.collab;
import wb.net.protocol;
import wb.net.socket;
import wb.session;
import wb.ui.context;
import wb.ui.draw;
import wb.ui.font;
import wb.ui.theme;
import wb.video.media;

namespace wb
{
namespace
{
using ui::Rect2;

constexpr uint16_t DEFAULT_PORT = 5000;

bool parsePortText(const std::string& p_Text, uint16_t& p_Port)
{
	if (p_Text.empty() || p_Text.size() > 5)
		return false;
	unsigned l_Value = 0;
	for (const char l_Char : p_Text)
	{
		if (l_Char < '0' || l_Char > '9')
			return false;
		l_Value = l_Value * 10 + static_cast<unsigned>(l_Char - '0');
	}
	if (l_Value == 0 || l_Value > 65535)
		return false;
	p_Port = static_cast<uint16_t>(l_Value);
	return true;
}

// A dimmed board with a centred card. Returns the card; the caller draws into it and must reset the opacity.
Rect2 openCard(ui::Context& p_Ui, const std::string_view p_Key, const float p_Height, float& p_Fade)
{
	ui::DrawList& l_Draw = p_Ui.draw();
	const ui::Theme& l_Theme = p_Ui.theme();
	const Vec2 l_Viewport = p_Ui.viewport();
	p_Fade = p_Ui.animFrom(ui::Context::id(p_Key, 1), 0.f, 1.f, 20.f);
	const Rect2 l_Screen{ Vec2{ 0.f }, l_Viewport };
	p_Ui.beginPanel(l_Screen);
	l_Draw.setOpacity(p_Fade);
	l_Draw.rect(l_Screen, 0.f, l_Theme.scrim);
	const float l_Width = std::min(p_Ui.px(440.f), l_Viewport.x - p_Ui.px(32.f));
	const float l_Lift = (1.f - p_Fade) * p_Ui.px(10.f);
	const Rect2 l_Card = Rect2::fromPosSize(Vec2{ (l_Viewport.x - l_Width) * 0.5f, (l_Viewport.y - p_Height) * 0.5f + l_Lift }, Vec2{ l_Width, p_Height });
	p_Ui.panel(l_Card, p_Ui.px(22.f));
	return l_Card;
}

// Cuts p_Text with an ellipsis so it fits p_Width
std::string fitText(ui::Context& p_Ui, std::string p_Text, const int p_Px, const float p_Width)
{
	if (p_Ui.font().measure(p_Text, p_Px) <= p_Width)
		return p_Text;
	while (p_Text.size() > 1 && p_Ui.font().measure(p_Text + "...", p_Px) > p_Width)
		p_Text.pop_back();
	return p_Text + "...";
}

constexpr size_t LOG_LINES = 7;

// A thumbnail of a picture, the first frame of a GIF or of a video: at most PREVIEW_SIDE pixels on a side. Runs on the
// session's worker thread.
constexpr uint32_t PREVIEW_SIDE = 64;

std::optional<AssetPreview> makeAssetPreview(const std::shared_ptr<const ByteSource>& p_Source)
{
	std::vector<uint8_t> l_Pixels;
	uint32_t l_Width = 0;
	uint32_t l_Height = 0;
	std::string l_Error;
	image::DecodeLimits l_Limits;
	l_Limits.maxDimension = 8192;
	l_Limits.maxBytes = 256ull * 1024 * 1024;
	// A big file is most likely a video: it is not read into memory just to find out that it is no picture
	const bool l_BigVideo = p_Source->size() > LARGE_ASSET_BYTES && video::probe(*p_Source).has_value();
	std::optional<image::Decoded> l_Image;
	if (!l_BigVideo)
	{
		const std::vector<uint8_t> l_Bytes = readAllBytes(*p_Source, l_Limits.maxBytes);
		if (!l_Bytes.empty())
			l_Image = image::decode(l_Bytes, l_Limits, l_Error);
	}
	if (l_Image && !l_Image->frames.empty())
	{
		l_Width = l_Image->width;
		l_Height = l_Image->height;
		l_Pixels = std::move(l_Image->frames.front());
	}
	else
	{
		video::Player::Options l_Options;
		l_Options.maxDimension = 2 * PREVIEW_SIDE;
		std::unique_ptr<video::Player> l_Player = video::Player::open(p_Source, l_Options, l_Error);
		if (!l_Player)
			return std::nullopt;
		std::shared_ptr<const video::Frame> l_Frame;
		for (int i = 0; i < 150 && !l_Frame; ++i)
		{
			l_Frame = l_Player->currentFrame();
			if (!l_Frame)
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		if (!l_Frame)
			return std::nullopt;
		l_Width = l_Player->info().width;
		l_Height = l_Player->info().height;
		l_Pixels = l_Frame->pixels;
	}
	if (l_Width == 0 || l_Height == 0 || l_Pixels.size() < static_cast<size_t>(l_Width) * l_Height * 4)
		return std::nullopt;

	// Box filter down to the thumbnail size
	const double l_Factor = std::max(1.0, static_cast<double>(std::max(l_Width, l_Height)) / PREVIEW_SIDE);
	AssetPreview l_Preview;
	l_Preview.width = std::max(1u, static_cast<uint32_t>(std::lround(l_Width / l_Factor)));
	l_Preview.height = std::max(1u, static_cast<uint32_t>(std::lround(l_Height / l_Factor)));
	l_Preview.rgba.resize(static_cast<size_t>(l_Preview.width) * l_Preview.height * 4);
	for (uint32_t y = 0; y < l_Preview.height; ++y)
	{
		const uint32_t y0 = static_cast<uint32_t>(y * l_Factor);
		const uint32_t y1 = std::min(l_Height, std::max(y0 + 1, static_cast<uint32_t>((y + 1) * l_Factor)));
		for (uint32_t x = 0; x < l_Preview.width; ++x)
		{
			const uint32_t x0 = static_cast<uint32_t>(x * l_Factor);
			const uint32_t x1 = std::min(l_Width, std::max(x0 + 1, static_cast<uint32_t>((x + 1) * l_Factor)));
			uint32_t l_Sum[4]{};
			for (uint32_t sy = y0; sy < y1; ++sy)
			{
				for (uint32_t sx = x0; sx < x1; ++sx)
				{
					const uint8_t* l_Source = &l_Pixels[(static_cast<size_t>(sy) * l_Width + sx) * 4];
					for (int c = 0; c < 4; ++c)
						l_Sum[c] += l_Source[c];
				}
			}
			const uint32_t l_Count = (y1 - y0) * (x1 - x0);
			for (int c = 0; c < 4; ++c)
				l_Preview.rgba[(static_cast<size_t>(y) * l_Preview.width + x) * 4 + static_cast<size_t>(c)] = static_cast<uint8_t>(l_Sum[c] / l_Count);
		}
	}
	return l_Preview;
}

std::string defaultUserName()
{
	SDL_Environment* l_Environment = SDL_GetEnvironment();
	for (const char* l_Variable : { "USERNAME", "USER", "LOGNAME" })
	{
		const char* l_Value = l_Environment != nullptr ? SDL_GetEnvironmentVariable(l_Environment, l_Variable) : nullptr;
		if (l_Value != nullptr && *l_Value != '\0')
			return net::sanitizeName(l_Value);
	}
	return "Guest";
}
} // namespace

// ------------------------------------------------------------------------------------------------ session plumbing

void App::initCollab()
{
	m_CollabEvent = SDL_RegisterEvents(1);
	m_CollabForm.name = defaultUserName();

	net::CollabHooks l_Hooks;
	l_Hooks.makeSnapshot = [this] { return m_Editor.saveBoardShell(net::SNAPSHOT_INLINE_ASSET_BYTES); };
	l_Hooks.loadSnapshot = [this](const std::span<const uint8_t> p_Bytes)
	{
		const IoResult l_Result = m_Session.adoptBoard(p_Bytes);
		if (!l_Result.ok)
			spdlog::warn("Could not load the shared board: {}", l_Result.error);
		return l_Result.ok;
	};
	l_Hooks.makePreview = makeAssetPreview;
	l_Hooks.assetLost = [this](const AssetId p_Asset) { m_Editor.history().invalidateAsset(p_Asset); };
	l_Hooks.remoteChange = [this](const std::span<const ObjectId> p_Ids, const bool p_Removed) { m_Editor.history().invalidate(p_Ids, p_Removed); };
	l_Hooks.message = [this](const std::string& p_Text, const bool p_Error)
	{
		if (p_Error)
		{
			// Failures come with the log of what led to them, so "it does not connect" can be diagnosed
			std::string l_Details;
			const std::deque<std::string>& l_Log = m_Collab->diagnostics();
			for (size_t i = l_Log.size() > 10 ? l_Log.size() - 10 : 0; i < l_Log.size(); ++i)
				l_Details += '\n' + l_Log[i];
			showMessage(l_Details.empty() ? p_Text : p_Text + "\n\nConnection log:" + l_Details);
		}
		else
			showToast(p_Text);
	};
	l_Hooks.log = [](const std::string& p_Line) { spdlog::info("collab: {}", p_Line); };
	l_Hooks.wake = [this]
	{
		SDL_Event l_Event{};
		l_Event.type = m_CollabEvent;
		SDL_PushEvent(&l_Event);
	};
	m_Collab = std::make_unique<net::CollabSession>(m_Editor.document(), std::move(l_Hooks));
	if (!m_PrefDir.empty())
		m_Collab->setCacheDirectory(m_PrefDir / "cache"); // big files that arrive are kept here while they are played
}

void App::pollCollab()
{
	if (!m_Collab || !m_Collab->active())
	{
		if (m_Collab && m_Collab->usersRevision() != m_CollabUsersSeen)
		{
			m_CollabUsersSeen = m_Collab->usersRevision();
			requestRedraw();
		}
		return;
	}
	if (m_Collab->online())
	{
		// Where my pointer is on the board, for the others. It stays in sync over the toolbar and panels too; it is only
		// hidden when it leaves the window.
		const bool l_InWindow = m_PointerInWindow && m_LastPointer.has_value();
		m_Collab->setLocalCursor(l_InWindow ? m_Editor.camera().screenToWorld(DVec2{ m_LastPointer->position }) : DVec2{ 0.0 }, l_InWindow);
	}
	if (m_Collab->online())
	{
		// The line being drawn, so the others watch it grow
		if (const std::optional<render::LiveStrokeView> l_Line = m_Editor.liveStroke())
		{
			const net::CollabSession::LocalStroke l_Local{ .origin = l_Line->origin, .color = l_Line->color, .stable = l_Line->committed, .tail = l_Line->tail };
			m_Collab->setLocalStroke(&l_Local);
		}
		else
		{
			m_Collab->setLocalStroke(nullptr);
		}
	}
	const uint64_t l_Revision = m_Editor.document().revision();
	m_Collab->tick();
	for (const net::CollabSession::VideoEvent& l_Event : m_Collab->takeVideoEvents())
	{
		m_Canvas.videos().applyRemote(l_Event.id, l_Event.playing, l_Event.position);
		requestRedraw();
	}
	if (m_Collab->cursorPending() || m_Collab->strokePending())
		requestRedraw(2); // the next update is not due yet; come back for it
	if (m_Collab->strokesRevision() != m_CollabStrokesSeen)
	{
		m_CollabStrokesSeen = m_Collab->strokesRevision();
		requestRedraw();
	}
	if (m_Collab->cursorsRevision() != m_CollabCursorsSeen)
	{
		m_CollabCursorsSeen = m_Collab->cursorsRevision();
		requestRedraw(2);
	}
	if (m_Editor.document().revision() != l_Revision)
	{
		m_Editor.abortTextEditingIfGone();
		// What arrived is not in the undo history, but it is unsaved work
		m_Editor.history().markExternalChange();
		requestRedraw();
	}
	if (m_Collab->usersRevision() != m_CollabUsersSeen)
	{
		m_CollabUsersSeen = m_Collab->usersRevision();
		requestRedraw();
	}
	if (m_Collab->assetsRevision() != m_CollabAssetsSeen)
	{
		// A picture or video arrived (or the transfer moved on): placeholders turn into the real thing
		m_CollabAssetsSeen = m_Collab->assetsRevision();
		requestRedraw();
		const uint32_t l_Pending = m_Collab->assetProgress().files;
		if (m_CollabFilesPending > 0 && l_Pending == 0 && m_Collab->online())
			showToast("All pictures and videos received");
		m_CollabFilesPending = l_Pending;
	}
	if (collabBusy())
		requestRedraw(1); // the spinner-less "connecting" card still has to notice the end of the attempt
}

void App::openCollabDialog(const CollabDialog p_Kind)
{
	m_CollabDialog = p_Kind;
	requestRedraw();
}

void App::startHosting()
{
	uint16_t l_Port = 0;
	if (!parsePortText(m_CollabForm.port, l_Port))
		return;
	std::string l_Error;
	if (!m_Collab->host(l_Port, m_CollabForm.password, m_CollabForm.name, l_Error))
	{
		showMessage("Could not start hosting on port " + std::to_string(l_Port) + ".\n\n" + l_Error);
		return;
	}
	m_CollabDialog = CollabDialog::None;
	showToast("Hosting on port " + std::to_string(m_Collab->listenPort()));
}

void App::beginJoin()
{
	std::string l_Host;
	uint16_t l_Port = 0;
	if (!net::parseEndpoint(m_CollabForm.address, DEFAULT_PORT, l_Host, l_Port))
	{
		showMessage("That is not a valid address.");
		return;
	}
	m_Collab->join(l_Host, l_Port, m_CollabForm.password, m_CollabForm.name);
	requestRedraw();
}

void App::submitCollabDialog()
{
	std::string l_Host;
	uint16_t l_Port = 0;
	switch (m_CollabDialog)
	{
	case CollabDialog::Host:
		if (parsePortText(m_CollabForm.port, l_Port))
			startHosting();
		break;
	case CollabDialog::Join:
		if (net::parseEndpoint(m_CollabForm.address, DEFAULT_PORT, l_Host, l_Port))
		{
			m_CollabDialog = CollabDialog::None;
			requestAction(Action::JoinSession); // asks about unsaved changes first: joining replaces the board
		}
		break;
	default:
		break;
	}
}

// ------------------------------------------------------------------------------------------------ dialogs

void App::buildCollabBusy()
{
	ui::DrawList& l_Draw = m_Ui.draw();
	const ui::Theme& l_Theme = m_Ui.theme();
	const float l_Pad = m_Ui.px(26.f);
	float l_Fade = 1.f;
	const std::deque<std::string>& l_Log = m_Collab->diagnostics();
	const size_t l_LogShown = std::min<size_t>(l_Log.size(), 4);
	const float l_LogLine = m_Ui.px(17.f);
	const float l_InnerWidth = std::min(m_Ui.px(440.f), m_Ui.viewport().x - m_Ui.px(32.f)) - l_Pad * 2.f;
	const Rect2 l_Card = openCard(m_Ui, "dialog.collab", l_Pad * 2.f + m_Ui.px(26.f) + m_Ui.px(14.f) + m_Ui.px(22.f) + (l_LogShown > 0 ? m_Ui.px(10.f) + l_LogLine * static_cast<float>(l_LogShown) : 0.f) + m_Ui.px(24.f) + m_Ui.px(40.f), l_Fade);
	float l_Y = l_Card.min.y + l_Pad;
	m_Ui.label(Vec2{ l_Card.min.x + l_Pad, l_Y + m_Ui.px(13.f) }, "Joining session", 18.f, l_Theme.text);
	l_Y += m_Ui.px(26.f) + m_Ui.px(14.f);
	const char* l_Text = m_Collab->state() == net::LinkState::Connecting ? "Connecting to the host..." : "Receiving the board...";
	l_Draw.text(Vec2{ l_Card.min.x + l_Pad, l_Y + m_Ui.px(11.f) }, l_Text, m_Ui.fontPx(14.f), l_Theme.textMuted);
	l_Y += m_Ui.px(22.f);
	if (l_LogShown > 0)
		l_Y += m_Ui.px(10.f);
	for (size_t i = l_Log.size() - l_LogShown; i < l_Log.size(); ++i)
	{
		l_Draw.text(Vec2{ l_Card.min.x + l_Pad, l_Y + l_LogLine * 0.5f }, fitText(m_Ui, l_Log[i], m_Ui.fontPx(11.5f), l_InnerWidth), m_Ui.fontPx(11.5f), l_Theme.textMuted);
		l_Y += l_LogLine;
	}
	l_Y += m_Ui.px(24.f);
	const float l_ButtonWidth = m_Ui.px(110.f);
	if (m_Ui.button("collab.cancelJoin", Rect2::fromPosSize(Vec2{ l_Card.max.x - l_Pad - l_ButtonWidth, l_Y }, Vec2{ l_ButtonWidth, m_Ui.px(40.f) }), "Cancel", ui::ButtonStyle::Secondary))
		m_Collab->leave();
	l_Draw.setOpacity(1.f);
}

void App::buildCollabDialog()
{
	ui::DrawList& l_Draw = m_Ui.draw();
	const ui::Theme& l_Theme = m_Ui.theme();
	const CollabDialog l_Kind = m_CollabDialog;
	const float l_Pad = m_Ui.px(26.f);
	const float l_TitleBlock = m_Ui.px(26.f) + m_Ui.px(14.f);
	const float l_FieldBlock = m_Ui.px(22.f) + m_Ui.px(38.f) + m_Ui.px(14.f);
	const float l_ButtonHeight = m_Ui.px(40.f);
	const float l_NoteLine = m_Ui.px(19.f);
	const float l_InnerWidth = std::min(m_Ui.px(440.f), m_Ui.viewport().x - m_Ui.px(32.f)) - l_Pad * 2.f;
	const int l_NotePx = m_Ui.fontPx(12.5f);

	if (l_Kind == CollabDialog::Session)
	{
		const std::vector<net::UserInfo>& l_Users = m_Collab->users();
		const float l_RowHeight = m_Ui.px(34.f);
		const std::deque<std::string>& l_Log = m_Collab->diagnostics();
		const size_t l_LogShown = std::min(l_Log.size(), LOG_LINES);
		const float l_LogLine = m_Ui.px(17.f);
		const float l_LogBlock = l_LogShown > 0 ? m_Ui.px(30.f) + l_LogLine * static_cast<float>(l_LogShown) : 0.f;
		const float l_Height = l_Pad + l_TitleBlock + m_Ui.px(22.f) + m_Ui.px(8.f) + l_RowHeight * static_cast<float>(std::max<size_t>(l_Users.size(), 1)) + l_LogBlock + m_Ui.px(20.f) + l_ButtonHeight + l_Pad;
		float l_Fade = 1.f;
		const Rect2 l_Card = openCard(m_Ui, "dialog.collab", l_Height, l_Fade);
		const float l_Left = l_Card.min.x + l_Pad;
		float l_Y = l_Card.min.y + l_Pad;
		m_Ui.label(Vec2{ l_Left, l_Y + m_Ui.px(13.f) }, "Session", 18.f, l_Theme.text);
		l_Y += l_TitleBlock;
		const bool l_Hosting = m_Collab->role() == net::Role::Host;
		std::string l_Where = !m_Collab->active() ? "Not in a session" : l_Hosting ? "Hosting on port " + std::to_string(m_Collab->listenPort()) : "Connected to the host";
		if (const net::CollabSession::AssetProgress l_Assets = m_Collab->assetProgress(); l_Assets.files > 0)
		{
			l_Where += "  -  receiving " + std::to_string(l_Assets.files) + (l_Assets.files == 1 ? " picture/video" : " pictures/videos");
			if (l_Assets.expected > 0)
				l_Where += " (" + std::to_string(l_Assets.received >> 20) + " of " + std::to_string(l_Assets.expected >> 20) + " MiB)";
		}
		l_Draw.text(Vec2{ l_Left, l_Y + m_Ui.px(11.f) }, l_Where, m_Ui.fontPx(13.f), l_Theme.textMuted);
		l_Y += m_Ui.px(22.f) + m_Ui.px(8.f);
		for (const net::UserInfo& l_User : l_Users)
		{
			const float l_CenterY = l_Y + l_RowHeight * 0.5f;
			l_Draw.circle(Vec2{ l_Left + m_Ui.px(8.f), l_CenterY }, m_Ui.px(7.f), Color::fromRgba8(l_User.color));
			std::string l_Tags;
			if (l_User.host)
				l_Tags += "host";
			if (l_User.slot == m_Collab->localSlot())
				l_Tags += l_Tags.empty() ? "you" : ", you";
			const float l_NameWidth = m_Ui.font().measure(l_User.name, m_Ui.fontPx(14.f));
			l_Draw.text(Vec2{ l_Left + m_Ui.px(26.f), l_CenterY }, l_User.name, m_Ui.fontPx(14.f), l_Theme.text);
			if (!l_Tags.empty())
				l_Draw.text(Vec2{ l_Left + m_Ui.px(26.f) + l_NameWidth + m_Ui.px(10.f), l_CenterY }, l_Tags, m_Ui.fontPx(12.5f), l_Theme.textMuted);
			l_Y += l_RowHeight;
		}
		if (l_Users.empty())
			l_Y += l_RowHeight;
		if (l_LogShown > 0)
		{
			l_Y += m_Ui.px(10.f);
			l_Draw.text(Vec2{ l_Left, l_Y + m_Ui.px(8.f) }, "Connection log", m_Ui.fontPx(12.5f), l_Theme.textMuted);
			l_Y += m_Ui.px(20.f);
			for (size_t i = l_Log.size() - l_LogShown; i < l_Log.size(); ++i)
			{
				l_Draw.text(Vec2{ l_Left, l_Y + l_LogLine * 0.5f }, fitText(m_Ui, l_Log[i], m_Ui.fontPx(11.5f), l_InnerWidth), m_Ui.fontPx(11.5f), l_Theme.textMuted);
				l_Y += l_LogLine;
			}
		}
		l_Y += m_Ui.px(20.f);
		const float l_ButtonWidth = m_Ui.px(140.f);
		const Rect2 l_Close = Rect2::fromPosSize(Vec2{ l_Card.max.x - l_Pad - m_Ui.px(110.f), l_Y }, Vec2{ m_Ui.px(110.f), l_ButtonHeight });
		const Rect2 l_Leave = Rect2::fromPosSize(Vec2{ l_Close.min.x - m_Ui.px(10.f) - l_ButtonWidth, l_Y }, Vec2{ l_ButtonWidth, l_ButtonHeight });
		if (m_Ui.button("collab.leave", l_Leave, l_Hosting ? "Stop hosting" : "Leave session", ui::ButtonStyle::Secondary))
		{
			m_Collab->leave();
			m_CollabDialog = CollabDialog::None;
			showToast("Left the session");
		}
		if (m_Ui.button("collab.close", l_Close, "Close", ui::ButtonStyle::Primary))
			m_CollabDialog = CollabDialog::None;
		l_Draw.setOpacity(1.f);
		return;
	}

	const bool l_Host = l_Kind == CollabDialog::Host;
	const std::string l_Note = l_Host ? "Share your address and this port with the people who should join. Nothing is encrypted: use a trusted network or a VPN. Everyone with the password can edit this board."
	                                  : "Joining replaces the board you have open with the host's.";
	const std::vector<std::string> l_NoteLines = m_Ui.font().wrap(l_Note, l_NotePx, l_InnerWidth);
	const float l_Height = l_Pad + l_TitleBlock + 3.f * l_FieldBlock + l_NoteLine * static_cast<float>(l_NoteLines.size()) + m_Ui.px(16.f) + l_ButtonHeight + l_Pad;
	float l_Fade = 1.f;
	const Rect2 l_Card = openCard(m_Ui, "dialog.collab", l_Height, l_Fade);
	const float l_Left = l_Card.min.x + l_Pad;
	float l_Y = l_Card.min.y + l_Pad;
	m_Ui.label(Vec2{ l_Left, l_Y + m_Ui.px(13.f) }, l_Host ? "Host a session" : "Join a session", 18.f, l_Theme.text);
	l_Y += l_TitleBlock;

	const auto l_Field = [&](const char* p_Key, const char* p_Title, std::string& p_Text, const char* p_Placeholder, const std::string_view p_Allowed, const size_t p_Max)
	{
		l_Draw.text(Vec2{ l_Left, l_Y + m_Ui.px(11.f) }, p_Title, m_Ui.fontPx(13.f), l_Theme.textMuted);
		l_Y += m_Ui.px(22.f);
		m_Ui.textField(p_Key, Rect2::fromPosSize(Vec2{ l_Left, l_Y }, Vec2{ l_InnerWidth, m_Ui.px(38.f) }), p_Text, p_Placeholder, p_Allowed, p_Max);
		l_Y += m_Ui.px(38.f) + m_Ui.px(14.f);
	};
	l_Field("collab.name", "Your name", m_CollabForm.name, "Shown next to your cursor", {}, net::MAX_NAME_BYTES);
	if (l_Host)
		l_Field("collab.port", "Port", m_CollabForm.port, "5000", "0123456789", 5);
	else
		l_Field("collab.address", "Host address", m_CollabForm.address, "192.168.1.20:5000", {}, 255);
	l_Field("collab.password", "Password", m_CollabForm.password, l_Host ? "Leave empty for an open session" : "Ask the host", {}, 64);

	for (const std::string& l_Line : l_NoteLines)
	{
		l_Draw.text(Vec2{ l_Left, l_Y + l_NoteLine * 0.5f }, l_Line, l_NotePx, l_Theme.textMuted);
		l_Y += l_NoteLine;
	}
	l_Y += m_Ui.px(16.f);

	std::string l_ParsedHost;
	uint16_t l_ParsedPort = 0;
	const bool l_Valid = l_Host ? parsePortText(m_CollabForm.port, l_ParsedPort) : net::parseEndpoint(m_CollabForm.address, DEFAULT_PORT, l_ParsedHost, l_ParsedPort);
	const float l_ButtonWidth = m_Ui.px(120.f);
	const Rect2 l_Go = Rect2::fromPosSize(Vec2{ l_Card.max.x - l_Pad - l_ButtonWidth, l_Y }, Vec2{ l_ButtonWidth, l_ButtonHeight });
	const Rect2 l_Cancel = Rect2::fromPosSize(Vec2{ l_Go.min.x - m_Ui.px(10.f) - l_ButtonWidth, l_Y }, Vec2{ l_ButtonWidth, l_ButtonHeight });
	if (m_Ui.button("collab.cancel", l_Cancel, "Cancel", ui::ButtonStyle::Secondary))
		m_CollabDialog = CollabDialog::None;
	if (m_Ui.button("collab.go", l_Go, l_Host ? "Start hosting" : "Join", ui::ButtonStyle::Primary, l_Valid))
		submitCollabDialog();
	l_Draw.setOpacity(1.f);
}
} // namespace wb
