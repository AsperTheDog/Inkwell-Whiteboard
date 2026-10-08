// App: the viewer that appears on a video while the pointer is over it (or it is selected): play/pause, a progress bar
// that can be dragged, loop and sound. Paused videos carry a small play badge so they read as videos at a glance.
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
#include <volk.h>

module wb.app;

import wb.doc.document;
import wb.doc.object;
import wb.editor;
import wb.math;
import wb.render.canvas_renderer;
import wb.render.video_store;
import wb.tools.tool;
import wb.ui.context;
import wb.ui.draw;
import wb.ui.font;
import wb.ui.theme;
import wb.view.camera;

namespace wb
{
namespace
{
using ui::Rect2;

constexpr float BAR_HEIGHT = 40.f;        // points
constexpr float BAR_MARGIN = 10.f;        // between the bar and the video's edge
constexpr float BUTTON_SIZE = 32.f;
constexpr float MIN_VIEWER_WIDTH = 150.f; // narrower than this on screen (points): only the badge
constexpr double SEEK_SLOP_SECONDS = 0.02;

std::string formatTime(const double p_Seconds)
{
	const int l_Total = static_cast<int>(std::max(p_Seconds, 0.0));
	char l_Buffer[32];
	if (l_Total >= 3600)
		std::snprintf(l_Buffer, sizeof(l_Buffer), "%d:%02d:%02d", l_Total / 3600, (l_Total / 60) % 60, l_Total % 60);
	else
		std::snprintf(l_Buffer, sizeof(l_Buffer), "%d:%02d", l_Total / 60, l_Total % 60);
	return l_Buffer;
}

// "12:34" -> "00:00": the widest text of the same shape, so a label made from it never jitters
std::string widestLike(std::string p_Text)
{
	for (char& l_Char : p_Text)
	{
		if (l_Char >= '0' && l_Char <= '9')
			l_Char = '0';
	}
	return p_Text;
}

// Where the video is on screen: the rectangle around its (possibly turned) quad, and the quad's corners
struct VideoOnScreen
{
	Rect2 box{};
	std::array<Vec2, 4> corners{};
	Affine2 localToScreen{};
};

std::optional<VideoOnScreen> videoOnScreen(const Object& p_Object, const VideoData& p_Video, const Camera& p_Camera)
{
	VideoOnScreen l_Result;
	l_Result.localToScreen = p_Camera.worldToScreenTransform() * p_Object.transform;
	const Rect l_Local = p_Video.localBounds();
	const DVec2 l_Points[4]{ l_Local.min, DVec2{ l_Local.max.x, l_Local.min.y }, l_Local.max, DVec2{ l_Local.min.x, l_Local.max.y } };
	Vec2 l_Min{ 1e30f };
	Vec2 l_Max{ -1e30f };
	for (int i = 0; i < 4; ++i)
	{
		const Vec2 l_Screen{ l_Result.localToScreen.apply(l_Points[i]) };
		if (!std::isfinite(l_Screen.x) || !std::isfinite(l_Screen.y))
			return std::nullopt;
		l_Result.corners[static_cast<size_t>(i)] = l_Screen;
		l_Min = glm::min(l_Min, l_Screen);
		l_Max = glm::max(l_Max, l_Screen);
	}
	l_Result.box = Rect2{ l_Min, l_Max };
	return l_Result;
}

bool pointInVideo(const VideoOnScreen& p_Screen, const Rect& p_Local, const Vec2 p_Point)
{
	if (!p_Screen.localToScreen.isInvertible())
		return false;
	return p_Local.contains(p_Screen.localToScreen.inverse().apply(DVec2{ p_Point }));
}
} // namespace

void App::buildVideoViewer()
{
	ui::DrawList& l_Draw = m_Ui.draw();
	const ui::Theme& l_Theme = m_Ui.theme();
	const Vec2 l_Viewport = m_Ui.viewport();
	const Camera& l_Camera = m_Editor.camera();
	render::VideoStore& l_Videos = m_Canvas.videos();
	const std::span<const ObjectId> l_Visible = m_Canvas.visibleVideos();

	// ---- which video gets the viewer
	const bool l_Quiet = m_Editor.isBusy() || m_Editor.textEditing() || modalOpen();
	const bool l_HavePointer = m_PointerInWindow && m_LastPointer.has_value();
	const Vec2 l_Pointer = l_HavePointer ? m_LastPointer->position : Vec2{ -1000.f };
	ObjectId l_Hot = INVALID_OBJECT_ID;
	if (m_VideoScrub != INVALID_OBJECT_ID)
	{
		l_Hot = m_VideoScrub;
	}
	else if (!l_Quiet)
	{
		if (m_VideoHot != INVALID_OBJECT_ID && l_HavePointer && m_VideoBarRect.contains(l_Pointer))
		{
			l_Hot = m_VideoHot; // on the bar itself
		}
		else if (l_HavePointer && !m_CursorOverUi)
		{
			for (auto l_It = l_Visible.rbegin(); l_It != l_Visible.rend() && l_Hot == INVALID_OBJECT_ID; ++l_It)
			{
				const Object* l_Object = m_Editor.document().find(*l_It);
				const VideoData* l_Video = l_Object != nullptr ? l_Object->video() : nullptr;
				if (l_Video == nullptr)
					continue;
				if (const std::optional<VideoOnScreen> l_Screen = videoOnScreen(*l_Object, *l_Video, l_Camera); l_Screen && l_Screen->box.contains(l_Pointer) && pointInVideo(*l_Screen, l_Video->localBounds(), l_Pointer))
					l_Hot = *l_It;
			}
		}
		// A selected video keeps its viewer (this is how it works with touch, which has no hover)
		if (l_Hot == INVALID_OBJECT_ID && m_Editor.selection().size() == 1)
		{
			const ObjectId l_Selected = m_Editor.selection().orderedIds().front();
			if (const Object* l_Object = m_Editor.document().find(l_Selected); l_Object != nullptr && l_Object->video() != nullptr)
				l_Hot = l_Selected;
		}
	}

	if (l_Hot != m_VideoHot)
	{
		m_VideoHot = l_Hot;
		m_Ui.setAnim(ui::Context::id("video.show"), 0.f);
		m_VideoBarRect = Rect2{};
	}
	if (m_VideoScrub != INVALID_OBJECT_ID && l_Hot != m_VideoScrub)
		m_VideoScrub = INVALID_OBJECT_ID;

	// ---- paused videos wear a badge; videos that cannot play say so
	for (const ObjectId l_Id : l_Visible)
	{
		if (l_Id == l_Hot)
			continue;
		const Object* l_Object = m_Editor.document().find(l_Id);
		const VideoData* l_Video = l_Object != nullptr ? l_Object->video() : nullptr;
		if (l_Video == nullptr)
			continue;
		const render::VideoStatus l_Status = l_Videos.status(l_Id);
		const std::optional<VideoOnScreen> l_Screen = videoOnScreen(*l_Object, *l_Video, l_Camera);
		if (!l_Screen || std::min(l_Screen->box.width(), l_Screen->box.height()) < m_Ui.px(48.f))
			continue;
		if (l_Status.failed)
		{
			const std::string l_Text = l_Status.error.empty() ? "This video cannot be played" : l_Status.error;
			const int l_Size = m_Ui.fontPx(13.f);
			const float l_Width = m_Ui.font().measure(l_Text, l_Size) + m_Ui.px(24.f);
			if (l_Width < l_Screen->box.width())
			{
				const Rect2 l_Chip = Rect2::fromCenter(l_Screen->box.center(), Vec2{ l_Width, m_Ui.px(30.f) });
				l_Draw.rect(l_Chip, m_Ui.px(15.f), ui::withAlpha(l_Theme.danger, 0.9f));
				l_Draw.text(l_Chip.center(), l_Text, l_Size, Color{ 1.f, 1.f, 1.f, 1.f }, ui::TextAlign::Center);
			}
		}
		else if (l_Status.ready && !l_Status.playing)
		{
			const float l_Radius = std::clamp(std::min(l_Screen->box.width(), l_Screen->box.height()) * 0.12f, m_Ui.px(14.f), m_Ui.px(26.f));
			const Vec2 l_Center = l_Screen->box.center();
			l_Draw.circle(l_Center, l_Radius, Color{ 0.f, 0.f, 0.f, 0.42f });
			l_Draw.ring(l_Center, l_Radius, Color{ 1.f, 1.f, 1.f, 0.55f }, m_Ui.px(1.5f));
			l_Draw.icon(ui::Icon::Play, l_Center + Vec2{ l_Radius * 0.07f, 0.f }, static_cast<int>(l_Radius * 1.05f), Color{ 1.f, 1.f, 1.f, 0.95f });
		}
	}

	// ---- the viewer
	if (l_Hot == INVALID_OBJECT_ID)
	{
		m_VideoBarRect = Rect2{};
		return;
	}
	const Object* l_Object = m_Editor.document().find(l_Hot);
	const VideoData* l_Video = l_Object != nullptr ? l_Object->video() : nullptr;
	if (l_Video == nullptr)
	{
		m_VideoHot = INVALID_OBJECT_ID;
		m_VideoScrub = INVALID_OBJECT_ID;
		m_VideoBarRect = Rect2{};
		return;
	}
	const render::VideoStatus l_Status = l_Videos.status(l_Hot);
	const std::optional<VideoOnScreen> l_Screen = videoOnScreen(*l_Object, *l_Video, l_Camera);
	if (!l_Screen || !l_Status.ready)
	{
		m_VideoBarRect = Rect2{};
		return;
	}

	// The part of the video that is in the window
	Rect2 l_Area{ glm::max(l_Screen->box.min, Vec2{ 0.f }), glm::min(l_Screen->box.max, l_Viewport) };
	const float l_Margin = m_Ui.px(BAR_MARGIN);
	const float l_Available = l_Area.width() - 2.f * l_Margin;
	if (l_Available < m_Ui.px(MIN_VIEWER_WIDTH) || l_Area.height() < m_Ui.px(BAR_HEIGHT) + 2.f * l_Margin)
	{
		m_VideoBarRect = Rect2{};
		return;
	}

	const float l_Button = m_Ui.px(BUTTON_SIZE);
	const float l_Pad = m_Ui.px(4.f);
	const float l_Gap = m_Ui.px(2.f);
	const bool l_ShowLoop = l_Available >= m_Ui.px(230.f);
	const bool l_ShowSound = l_Status.hasAudio && l_Available >= m_Ui.px(270.f);
	const bool l_ShowTime = l_Status.duration > 0.0 && l_Available >= m_Ui.px(330.f);
	const int l_TimePx = m_Ui.fontPx(12.5f);
	const std::string l_DurationText = formatTime(l_Status.duration);
	const std::string l_TimeShape = widestLike(l_DurationText) + " / " + widestLike(l_DurationText);
	const float l_TimeWidth = l_ShowTime ? m_Ui.font().measure(l_TimeShape, l_TimePx) + m_Ui.px(12.f) : 0.f;
	const float l_TrackGap = m_Ui.px(8.f);

	const float l_Width = std::min(l_Available, m_Ui.px(560.f));
	const float l_Height = m_Ui.px(BAR_HEIGHT);
	const float l_Slide = (1.f - m_Ui.anim(ui::Context::id("video.show"), 1.f, 16.f)) * m_Ui.px(8.f);
	Rect2 l_Bar = Rect2::fromPosSize(Vec2{ l_Area.center().x - l_Width * 0.5f, l_Area.max.y - l_Margin - l_Height + l_Slide }, Vec2{ l_Width, l_Height });
	// Out of the way of the tool bar and the zoom pill, which sit at the bottom of the window
	if (m_ToolbarRect.max.x > m_ToolbarRect.min.x && l_Bar.max.x > m_ToolbarRect.min.x && l_Bar.min.x < m_ToolbarRect.max.x && l_Bar.max.y > m_ToolbarRect.min.y - m_Ui.px(8.f))
		l_Bar = l_Bar.translated(Vec2{ 0.f, m_ToolbarRect.min.y - m_Ui.px(8.f) - l_Bar.max.y });
	m_VideoBarRect = l_Bar;
	m_Ui.panel(l_Bar, l_Height * 0.5f);

	float l_Cursor = l_Bar.min.x + l_Pad;
	const float l_ButtonY = l_Bar.center().y - l_Button * 0.5f;
	const auto l_NextButton = [&]
	{
		const Rect2 l_Rect = Rect2::fromPosSize(Vec2{ l_Cursor, l_ButtonY }, Vec2{ l_Button });
		l_Cursor += l_Button + l_Gap;
		return l_Rect;
	};

	if (m_Ui.iconButton("video.play", l_NextButton(), l_Status.playing ? ui::Icon::Pause : ui::Icon::Play, false, true, l_Status.playing ? "Pause" : "Play"))
		l_Videos.setPlaying(l_Hot, !l_Status.playing);

	// Everything right of the track is laid out first so the track takes what is left
	float l_RightEdge = l_Bar.max.x - l_Pad;
	const Rect2 l_SoundRect = Rect2::fromPosSize(Vec2{ l_RightEdge - l_Button, l_ButtonY }, Vec2{ l_Button });
	if (l_ShowSound)
		l_RightEdge -= l_Button + l_Gap;
	const Rect2 l_LoopRect = Rect2::fromPosSize(Vec2{ l_RightEdge - l_Button, l_ButtonY }, Vec2{ l_Button });
	if (l_ShowLoop)
		l_RightEdge -= l_Button + l_Gap;

	if (l_ShowTime)
	{
		const std::string l_TimeText = formatTime(l_Status.position) + " / " + l_DurationText;
		l_Draw.text(Vec2{ l_Cursor + m_Ui.px(4.f), l_Bar.center().y }, l_TimeText, l_TimePx, l_Theme.textMuted);
		l_Cursor += l_TimeWidth;
	}

	// ---- progress bar
	const Rect2 l_Track{ Vec2{ l_Cursor + l_TrackGap, l_Bar.center().y }, Vec2{ l_RightEdge - l_TrackGap, l_Bar.center().y } };
	if (l_Track.width() > m_Ui.px(24.f))
	{
		const uint32_t l_SeekId = ui::Context::id("video.seek");
		const Rect2 l_HitRect{ Vec2{ l_Track.min.x - m_Ui.px(8.f), l_Bar.min.y + m_Ui.px(4.f) }, Vec2{ l_Track.max.x + m_Ui.px(8.f), l_Bar.max.y - m_Ui.px(4.f) } };
		const ui::Interaction l_State = m_Ui.interact(l_SeekId, l_HitRect);
		double l_Shown = l_Status.position;
		if ((l_State.held || l_State.clicked) && l_Status.duration > 0.0)
		{
			const double l_T = std::clamp(static_cast<double>((m_Ui.pointerPosition().x - l_Track.min.x) / std::max(l_Track.width(), 1.f)), 0.0, 1.0) * l_Status.duration;
			if (l_State.held && m_VideoScrub == INVALID_OBJECT_ID)
			{
				m_VideoScrub = l_Hot;
				m_VideoScrubResume = l_Status.playing;
				m_VideoScrubLastSeek = -1.0;
				if (l_Status.playing)
					l_Videos.setPlaying(l_Hot, false);
			}
			if (m_VideoScrubLastSeek < 0.0 || std::abs(l_T - m_VideoScrubLastSeek) > SEEK_SLOP_SECONDS || l_State.clicked)
			{
				l_Videos.seek(l_Hot, l_T);
				m_VideoScrubLastSeek = l_T;
			}
			l_Shown = l_T;
		}
		if (m_VideoScrub == l_Hot && !l_State.held)
		{
			if (m_VideoScrubResume)
				l_Videos.setPlaying(l_Hot, true);
			m_VideoScrub = INVALID_OBJECT_ID;
		}

		const float l_Active = m_Ui.anim(l_SeekId + 1, (l_State.hovered || l_State.held) ? 1.f : 0.f, 22.f);
		const float l_Fraction = l_Status.duration > 0.0 ? static_cast<float>(std::clamp(l_Shown / l_Status.duration, 0.0, 1.0)) : 0.f;
		const float l_Half = m_Ui.px(2.f + 1.2f * l_Active);
		const float l_Y = l_Track.min.y;
		const float l_FillX = l_Track.min.x + l_Track.width() * l_Fraction;
		l_Draw.rect(Rect2{ Vec2{ l_Track.min.x, l_Y - l_Half }, Vec2{ l_Track.max.x, l_Y + l_Half } }, l_Half, l_Theme.track);
		l_Draw.rect(Rect2{ Vec2{ l_Track.min.x, l_Y - l_Half }, Vec2{ std::max(l_FillX, l_Track.min.x + l_Half * 2.f), l_Y + l_Half } }, l_Half, l_Theme.accent);
		const float l_Knob = m_Ui.px(5.f + 2.f * l_Active);
		l_Draw.shadow(Rect2::fromCenter(Vec2{ l_FillX, l_Y }, Vec2{ l_Knob * 2.f }), l_Knob, m_Ui.px(4.f), Vec2{ 0.f, m_Ui.px(1.f) }, ui::withAlpha(l_Theme.shadow, 0.5f));
		l_Draw.circle(Vec2{ l_FillX, l_Y }, l_Knob, Color{ 1.f, 1.f, 1.f, 1.f });
		l_Draw.ring(Vec2{ l_FillX, l_Y }, l_Knob, l_Theme.accent, m_Ui.px(1.75f));
	}

	if (l_ShowLoop && m_Ui.iconButton("video.loop", l_LoopRect, ui::Icon::Repeat, l_Video->loop, true, l_Video->loop ? "Loop is on" : "Loop"))
		m_Editor.editVideo(l_Hot, l_Video->loop ? "Stop looping" : "Loop video", [](VideoData& p_Data) { p_Data.loop = !p_Data.loop; return true; });
	if (l_ShowSound)
	{
		const char* l_Tip = !l_Status.audioWorks ? "No sound device" : (l_Video->muted ? "Sound is off" : "Sound is on");
		if (m_Ui.iconButton("video.sound", l_SoundRect, l_Video->muted ? ui::Icon::VolumeOff : ui::Icon::Volume, !l_Video->muted, l_Status.audioWorks, l_Tip))
			m_Editor.editVideo(l_Hot, l_Video->muted ? "Sound on" : "Sound off", [](VideoData& p_Data) { p_Data.muted = !p_Data.muted; return true; });
	}
}
} // namespace wb
