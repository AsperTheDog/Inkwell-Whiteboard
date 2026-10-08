module;
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <spdlog/spdlog.h>
#include "gfx/vk_check.hpp"
#include "gfx/vma.hpp"

module wb.render.video_store;

import wb.doc.object;
import wb.doc.document;
import wb.gfx.buffer;
import wb.gfx.commands;
import wb.gfx.context;
import wb.gfx.frames;
import wb.gfx.image;
import wb.render.image_store;
import wb.video.media;

namespace wb::render
{
namespace
{
constexpr VkFormat TEXTURE_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;
constexpr auto OFFSCREEN_DROP = std::chrono::seconds(4); // a paused video this long out of sight gives its memory back
constexpr uint32_t DEFAULT_MAX_DIMENSION = 1920;

uint32_t mipCountFor(const uint32_t p_Width, const uint32_t p_Height)
{
	uint32_t l_Count = 1;
	for (uint32_t l_Size = std::max(p_Width, p_Height); l_Size > 1; l_Size >>= 1)
		++l_Count;
	return l_Count;
}
} // namespace

void VideoStore::init(const gfx::GraphicsContext& p_Context, ImageStore& p_Images)
{
	m_Images = &p_Images;
	m_MaxDimension = std::min<uint32_t>(p_Context.info().limits.maxImageDimension2D, DEFAULT_MAX_DIMENSION);
}

void VideoStore::destroy(const gfx::GraphicsContext& p_Context)
{
	// Open jobs keep running to their end; they only touch their own data
	for (int l_Wait = 0; l_Wait < 20000 && m_ActiveThreads->load() > 0; ++l_Wait)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));

	const auto l_Free = [&](Entry& p_Entry)
	{
		p_Entry.player.reset();
		p_Entry.job.reset();
		for (Texture& l_Texture : p_Entry.textures)
			gfx::destroyImage(p_Context, l_Texture.image);
		for (gfx::Buffer& l_Buffer : p_Entry.staging)
			gfx::destroyBuffer(p_Context, l_Buffer);
	};
	for (auto& [l_Id, l_Entry] : m_Entries)
		l_Free(l_Entry);
	m_Entries.clear();
	for (Entry& l_Entry : m_Retired)
		l_Free(l_Entry);
	m_Retired.clear();
	m_FreeSets->clear();
	m_Images = nullptr;
}

// ---------------------------------------------------------------------------------------------------- opening

void VideoStore::startOpen(Entry& p_Entry, const Document& p_Document)
{
	const ImageAsset* l_Asset = p_Document.findAsset(p_Entry.asset);
	if (l_Asset == nullptr)
	{
		p_Entry.state = Entry::State::Failed;
		p_Entry.error = "The video is missing from the board.";
		return;
	}
	// Videos of one asset (and a video that scrolls out of view and back) share one copy of the file
	video::Blob l_Blob = m_Blobs[p_Entry.asset].lock();
	if (!l_Blob)
	{
		l_Blob = std::make_shared<const std::vector<uint8_t>>(l_Asset->bytes);
		m_Blobs[p_Entry.asset] = l_Blob;
	}
	p_Entry.job = std::make_shared<OpenJob>();
	m_ActiveThreads->fetch_add(1);
	video::Player::Options l_Options;
	l_Options.maxDimension = m_MaxDimension;
	std::thread([l_Job = p_Entry.job, l_Active = m_ActiveThreads, l_Blob, l_Options]
	{
		std::string l_Error;
		l_Job->player = video::Player::open(l_Blob, l_Options, l_Error);
		l_Job->error = std::move(l_Error);
		l_Job->done.store(true, std::memory_order_release);
		l_Active->fetch_sub(1);
	}).detach();
}

bool VideoStore::createTextures(const gfx::GraphicsContext& p_Context, Entry& p_Entry)
{
	const uint32_t l_Width = p_Entry.info.width;
	const uint32_t l_Height = p_Entry.info.height;
	if (l_Width == 0 || l_Height == 0 || l_Width > m_MaxDimension || l_Height > m_MaxDimension)
		return false;
	const uint32_t l_Mips = mipCountFor(l_Width, l_Height);
	for (Texture& l_Texture : p_Entry.textures)
	{
		l_Texture.image = gfx::createImage2D(p_Context, VkExtent2D{ l_Width, l_Height }, TEXTURE_FORMAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "video", l_Mips, 1, true);
		if (!m_FreeSets->empty())
		{
			l_Texture.set = m_FreeSets->back();
			m_FreeSets->pop_back();
			m_Images->writeSet(p_Context, l_Texture.set, l_Texture.image.view);
		}
		else
		{
			l_Texture.set = m_Images->allocateSet(p_Context, l_Texture.image.view);
		}
	}
	const VkDeviceSize l_Bytes = static_cast<VkDeviceSize>(l_Width) * l_Height * 4;
	for (gfx::Buffer& l_Buffer : p_Entry.staging)
		l_Buffer = gfx::createBuffer(p_Context, l_Bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, gfx::MemoryKind::Upload, "video upload");
	return true;
}

// ---------------------------------------------------------------------------------------------------- frames

void VideoStore::upload(const gfx::GraphicsContext& p_Context, gfx::FrameScheduler& p_Frames, const VkCommandBuffer p_Cmd, Entry& p_Entry, const video::Frame& p_Frame)
{
	(void)p_Context;
	Texture& l_Texture = p_Entry.textures[p_Entry.next];
	const uint32_t l_Width = p_Entry.info.width;
	const uint32_t l_Height = p_Entry.info.height;
	const size_t l_Bytes = static_cast<size_t>(l_Width) * l_Height * 4;
	if (p_Frame.pixels.size() < l_Bytes)
		return;
	gfx::Buffer& l_Staging = p_Entry.staging[p_Frames.currentSlot()];
	std::memcpy(l_Staging.mapped, p_Frame.pixels.data(), l_Bytes);

	const uint32_t l_Mips = l_Texture.image.mipLevels;
	const VkImage l_Image = l_Texture.image.handle;
	gfx::transitionImage(p_Cmd, l_Image, {
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.srcStage = VK_PIPELINE_STAGE_2_NONE,
		.srcAccess = VK_ACCESS_2_NONE,
		.dstStage = VK_PIPELINE_STAGE_2_COPY_BIT,
		.dstAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
	});
	const VkBufferImageCopy l_Copy{
		.bufferOffset = 0,
		.imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 },
		.imageExtent = { l_Width, l_Height, 1 },
	};
	vkCmdCopyBufferToImage(p_Cmd, l_Staging.handle, l_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &l_Copy);

	for (uint32_t l_Level = 1; l_Level < l_Mips; ++l_Level)
	{
		gfx::transitionImage(p_Cmd, l_Image, {
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.srcStage = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
			.srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
			.dstStage = VK_PIPELINE_STAGE_2_BLIT_BIT,
			.dstAccess = VK_ACCESS_2_TRANSFER_READ_BIT,
			.baseMip = l_Level - 1,
			.mipCount = 1,
		});
		const auto l_Extent = [&](const uint32_t p_Level) { return VkOffset3D{ static_cast<int32_t>(std::max(1u, l_Width >> p_Level)), static_cast<int32_t>(std::max(1u, l_Height >> p_Level)), 1 }; };
		const VkImageBlit l_Blit{
			.srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = l_Level - 1, .baseArrayLayer = 0, .layerCount = 1 },
			.srcOffsets = { VkOffset3D{ 0, 0, 0 }, l_Extent(l_Level - 1) },
			.dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = l_Level, .baseArrayLayer = 0, .layerCount = 1 },
			.dstOffsets = { VkOffset3D{ 0, 0, 0 }, l_Extent(l_Level) },
		};
		vkCmdBlitImage(p_Cmd, l_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, l_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &l_Blit, VK_FILTER_LINEAR);
	}

	if (l_Mips > 1)
	{
		gfx::transitionImage(p_Cmd, l_Image, {
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.srcStage = VK_PIPELINE_STAGE_2_BLIT_BIT,
			.srcAccess = VK_ACCESS_2_TRANSFER_READ_BIT,
			.dstStage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
			.dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			.baseMip = 0,
			.mipCount = l_Mips - 1,
		});
	}
	gfx::transitionImage(p_Cmd, l_Image, {
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		.srcStage = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
		.srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
		.dstStage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
		.dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
		.baseMip = l_Mips - 1,
		.mipCount = 1,
	});

	p_Entry.shown = static_cast<int>(p_Entry.next);
	p_Entry.next = (p_Entry.next + 1) % RING;
	p_Entry.uploadedSerial = p_Frame.serial;
}

void VideoStore::release(const gfx::GraphicsContext& p_Context, gfx::FrameScheduler& p_Frames, Entry& p_Entry)
{
	p_Entry.player.reset();
	for (Texture& l_Texture : p_Entry.textures)
	{
		if (!l_Texture.image.isValid())
			continue;
		p_Frames.deferDestroy([l_Allocator = p_Context.allocator(), l_Device = p_Context.device(), l_Handle = l_Texture.image.handle, l_View = l_Texture.image.view, l_Allocation = l_Texture.image.allocation, l_Set = l_Texture.set, l_Free = m_FreeSets]
		{
			vkDestroyImageView(l_Device, l_View, nullptr);
			vmaDestroyImage(l_Allocator, l_Handle, l_Allocation);
			l_Free->push_back(l_Set); // no frame in flight uses the set any more: it can point at another texture
		});
		l_Texture = Texture{};
	}
	for (gfx::Buffer& l_Buffer : p_Entry.staging)
	{
		if (l_Buffer.isValid())
			gfx::destroyBufferDeferred(p_Context, p_Frames, l_Buffer);
	}
}

void VideoStore::update(const gfx::GraphicsContext& p_Context, gfx::FrameScheduler& p_Frames, const VkCommandBuffer p_Cmd)
{
	++m_Tick;
	m_Animating = false;
	m_PlayingInBackground = false;
	for (Entry& l_Entry : m_Retired)
		release(p_Context, p_Frames, l_Entry);
	m_Retired.clear();

	const auto l_Now = std::chrono::steady_clock::now();
	for (auto l_It = m_Entries.begin(); l_It != m_Entries.end();)
	{
		Entry& l_Entry = l_It->second;
		const bool l_Visible = l_Entry.seenTick + 1 >= m_Tick;

		if (l_Entry.state == Entry::State::Opening && l_Entry.job->done.load(std::memory_order_acquire))
		{
			const std::shared_ptr<OpenJob> l_Job = std::move(l_Entry.job);
			l_Entry.player = std::move(l_Job->player);
			if (!l_Entry.player)
			{
				l_Entry.state = Entry::State::Failed;
				l_Entry.error = l_Job->error;
				spdlog::warn("Video {} could not be opened: {}", l_It->first, l_Job->error);
			}
			else
			{
				l_Entry.info = l_Entry.player->info();
				if (!createTextures(p_Context, l_Entry))
				{
					l_Entry.player.reset();
					l_Entry.state = Entry::State::Failed;
					l_Entry.error = "The video is too big for this graphics card.";
				}
				else
				{
					l_Entry.state = Entry::State::Ready;
					l_Entry.player->setLoop(l_Entry.loop);
					l_Entry.player->setMuted(l_Entry.muted);
					if (const auto l_Resume = m_Resume.find(l_It->first); l_Resume != m_Resume.end())
					{
						l_Entry.player->seek(l_Resume->second);
						m_Resume.erase(l_Resume);
					}
					if (l_Entry.wantSeek)
						l_Entry.player->seek(*l_Entry.wantSeek);
					l_Entry.wantSeek.reset();
					if (l_Entry.wantPlay)
						l_Entry.player->play();
				}
			}
		}

		if (l_Entry.state == Entry::State::Ready)
		{
			l_Entry.player->tick();
			const bool l_Playing = l_Entry.player->playing();
			if (l_Visible)
			{
				if (const std::shared_ptr<const video::Frame> l_Frame = l_Entry.player->currentFrame(); l_Frame && l_Frame->serial != l_Entry.uploadedSerial)
					upload(p_Context, p_Frames, p_Cmd, l_Entry, *l_Frame);
				// A paused video needs no frames once its first picture is up
				if (l_Playing || l_Entry.uploadedSerial == 0)
					m_Animating = true;
			}
			else if (l_Playing)
			{
				m_PlayingInBackground = true;
			}
			else if (l_Now - l_Entry.seenTime > OFFSCREEN_DROP)
			{
				m_Resume[l_It->first] = l_Entry.player->position();
				release(p_Context, p_Frames, l_Entry);
				l_It = m_Entries.erase(l_It);
				continue;
			}
		}
		else if (l_Entry.state == Entry::State::Opening)
		{
			m_Animating = m_Animating || l_Visible;
		}
		else if (!l_Visible && l_Now - l_Entry.seenTime > OFFSCREEN_DROP)
		{
			l_It = m_Entries.erase(l_It);
			continue;
		}
		++l_It;
	}
}

// ---------------------------------------------------------------------------------------------------- queries

VideoLookup VideoStore::lookup(const Document& p_Document, const ObjectId p_Id, const VideoData& p_Video)
{
	VideoLookup l_Result;
	l_Result.set = m_Images->placeholderSet();
	auto l_It = m_Entries.find(p_Id);
	if (l_It != m_Entries.end() && l_It->second.asset != p_Video.asset)
	{
		m_Retired.push_back(std::move(l_It->second));
		m_Entries.erase(l_It);
		l_It = m_Entries.end();
	}
	if (l_It == m_Entries.end())
	{
		Entry l_Entry;
		l_Entry.asset = p_Video.asset;
		l_It = m_Entries.emplace(p_Id, std::move(l_Entry)).first;
		startOpen(l_It->second, p_Document);
	}
	Entry& l_Entry = l_It->second;
	l_Entry.seenTick = m_Tick;
	l_Entry.seenTime = std::chrono::steady_clock::now();
	l_Entry.loop = p_Video.loop;
	l_Entry.muted = p_Video.muted;
	if (l_Entry.state == Entry::State::Ready)
	{
		l_Entry.player->setLoop(p_Video.loop);
		l_Entry.player->setMuted(p_Video.muted);
		if (l_Entry.shown >= 0)
		{
			l_Result.set = l_Entry.textures[static_cast<size_t>(l_Entry.shown)].set;
			l_Result.layer = 0.f;
		}
	}
	return l_Result;
}

VideoStatus VideoStore::status(const ObjectId p_Id) const
{
	VideoStatus l_Status;
	const auto l_It = m_Entries.find(p_Id);
	if (l_It == m_Entries.end())
		return l_Status;
	const Entry& l_Entry = l_It->second;
	l_Status.known = true;
	l_Status.failed = l_Entry.state == Entry::State::Failed;
	l_Status.error = l_Entry.error;
	l_Status.playing = l_Entry.wantPlay;
	if (l_Entry.state == Entry::State::Ready)
	{
		l_Status.ready = true;
		l_Status.playing = l_Entry.player->playing();
		l_Status.ended = l_Entry.player->ended();
		l_Status.position = l_Entry.player->position();
		l_Status.duration = l_Entry.player->duration();
		l_Status.hasAudio = l_Entry.info.hasAudio;
		l_Status.audioWorks = l_Entry.player->audioWorks();
	}
	return l_Status;
}

void VideoStore::setPlaying(const ObjectId p_Id, const bool p_Playing)
{
	const auto l_It = m_Entries.find(p_Id);
	if (l_It == m_Entries.end())
		return;
	Entry& l_Entry = l_It->second;
	l_Entry.wantPlay = p_Playing;
	if (l_Entry.state != Entry::State::Ready)
		return;
	if (!p_Playing)
	{
		l_Entry.player->pause();
		return;
	}
	if (l_Entry.player->ended() || (l_Entry.player->duration() > 0.0 && l_Entry.player->position() >= l_Entry.player->duration() - 0.05))
		l_Entry.player->seek(0.0);
	l_Entry.player->play();
}

void VideoStore::seek(const ObjectId p_Id, const double p_Seconds)
{
	const auto l_It = m_Entries.find(p_Id);
	if (l_It == m_Entries.end())
		return;
	Entry& l_Entry = l_It->second;
	if (l_Entry.state == Entry::State::Ready)
		l_Entry.player->seek(p_Seconds);
	else
		l_Entry.wantSeek = p_Seconds;
}

void VideoStore::forget(const ObjectId p_Id)
{
	m_Resume.erase(p_Id);
	const auto l_It = m_Entries.find(p_Id);
	if (l_It == m_Entries.end())
		return;
	l_It->second.player.reset(); // silence at once
	m_Retired.push_back(std::move(l_It->second));
	m_Entries.erase(l_It);
}

void VideoStore::clear()
{
	for (auto& [l_Id, l_Entry] : m_Entries)
	{
		l_Entry.player.reset();
		m_Retired.push_back(std::move(l_Entry));
	}
	m_Entries.clear();
	m_Resume.clear();
}
} // namespace wb::render
