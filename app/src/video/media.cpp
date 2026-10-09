module;
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>
#include "video/ffmpeg.hpp"

module wb.video.media;

namespace wb::video
{
namespace
{
using Clock = std::chrono::steady_clock;

constexpr size_t MAX_QUEUED_FRAMES = 5;
constexpr int AUDIO_RATE = 48000;
constexpr int AUDIO_CHANNELS = 2;
constexpr double AUDIO_BYTES_PER_SECOND = static_cast<double>(AUDIO_RATE) * AUDIO_CHANNELS * sizeof(float);
constexpr double AUDIO_QUEUE_PLAYING = 0.45; // seconds of sound kept ready on the device
constexpr double AUDIO_QUEUE_PAUSED = 0.12;
constexpr double LATE_FRAME_SECONDS = 0.12;  // pictures this far behind the clock are not worth converting
constexpr auto WAIT_SLICE = std::chrono::milliseconds(15);
constexpr const char* FORMAT_WHITELIST = "mov,mp4,m4a,3gp,3g2,mj2,matroska,webm,avi,flv,mpegts,mpeg,ogg,asf,wav,mp3,aac,ogv,m4v";

// ------------------------------------------------------------------------------------------- memory input

// A demuxer's view of the shared file bytes
struct Input
{
	Blob keep; // holds the bytes alive when they belong to the input
	std::span<const uint8_t> data;
	int64_t position = 0;
	AVFormatContext* format = nullptr;
	AVIOContext* io = nullptr;

	Input() = default;
	Input(const Input&) = delete;
	Input& operator=(const Input&) = delete;
	~Input()
	{
		close();
	}

	static int readPacket(void* p_Opaque, uint8_t* p_Buffer, const int p_Size)
	{
		Input* l_Self = static_cast<Input*>(p_Opaque);
		const int64_t l_Remaining = static_cast<int64_t>(l_Self->data.size()) - l_Self->position;
		if (l_Remaining <= 0)
			return AVERROR_EOF;
		const int l_Count = static_cast<int>(std::min<int64_t>(l_Remaining, p_Size));
		std::memcpy(p_Buffer, l_Self->data.data() + l_Self->position, static_cast<size_t>(l_Count));
		l_Self->position += l_Count;
		return l_Count;
	}

	static int64_t seekPacket(void* p_Opaque, int64_t p_Offset, int p_Whence)
	{
		Input* l_Self = static_cast<Input*>(p_Opaque);
		const int64_t l_Size = static_cast<int64_t>(l_Self->data.size());
		p_Whence &= ~AVSEEK_FORCE;
		if (p_Whence == AVSEEK_SIZE)
			return l_Size;
		int64_t l_Target = 0;
		if (p_Whence == SEEK_SET)
			l_Target = p_Offset;
		else if (p_Whence == SEEK_CUR)
			l_Target = l_Self->position + p_Offset;
		else if (p_Whence == SEEK_END)
			l_Target = l_Size + p_Offset;
		else
			return -1;
		if (l_Target < 0 || l_Target > l_Size)
			return -1;
		l_Self->position = l_Target;
		return l_Target;
	}

	bool open(Blob p_Blob, std::string& p_Error)
	{
		keep = std::move(p_Blob);
		return open(std::span<const uint8_t>(*keep), p_Error);
	}

	bool open(const std::span<const uint8_t> p_Data, std::string& p_Error)
	{
		data = p_Data;
		position = 0;
		constexpr int BUFFER_BYTES = 1 << 16;
		format = avformat_alloc_context();
		uint8_t* l_Buffer = static_cast<uint8_t*>(av_malloc(BUFFER_BYTES));
		if (format == nullptr || l_Buffer == nullptr)
		{
			av_free(l_Buffer);
			p_Error = "Out of memory.";
			return false;
		}
		io = avio_alloc_context(l_Buffer, BUFFER_BYTES, 0, this, &Input::readPacket, nullptr, &Input::seekPacket);
		if (io == nullptr)
		{
			av_free(l_Buffer);
			p_Error = "Out of memory.";
			return false;
		}
		format->pb = io;
		format->flags |= AVFMT_FLAG_CUSTOM_IO;

		AVDictionary* l_Options = nullptr;
		av_dict_set(&l_Options, "format_whitelist", FORMAT_WHITELIST, 0);
		const int l_Result = avformat_open_input(&format, nullptr, nullptr, &l_Options);
		av_dict_free(&l_Options);
		if (l_Result < 0)
		{
			p_Error = "This is not a video file Inkwell can read.";
			return false; // avformat_open_input freed (and nulled) the context
		}
		if (avformat_find_stream_info(format, nullptr) < 0)
		{
			p_Error = "The video file is damaged.";
			return false;
		}
		return true;
	}

	void close()
	{
		if (format != nullptr)
			avformat_close_input(&format);
		if (io != nullptr)
		{
			av_freep(&io->buffer);
			avio_context_free(&io);
		}
	}
};

int pickVideoStream(const AVFormatContext* p_Format)
{
	const int l_Index = av_find_best_stream(const_cast<AVFormatContext*>(p_Format), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (l_Index < 0 || (p_Format->streams[l_Index]->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0)
		return -1;
	const AVCodecParameters* l_Par = p_Format->streams[l_Index]->codecpar;
	return l_Par->width > 0 && l_Par->height > 0 ? l_Index : -1;
}

int pickAudioStream(const AVFormatContext* p_Format, const int p_VideoIndex)
{
	return av_find_best_stream(const_cast<AVFormatContext*>(p_Format), AVMEDIA_TYPE_AUDIO, -1, p_VideoIndex, nullptr, 0);
}

struct Geometry
{
	int outWidth = 0;  // size of the scaled picture before it is turned upright
	int outHeight = 0;
	int quarterTurns = 0; // clockwise
	uint32_t width = 0;   // size as shown
	uint32_t height = 0;
};

Geometry computeGeometry(AVFormatContext* p_Format, const AVStream* p_Stream, const uint32_t p_MaxDimension)
{
	const AVCodecParameters* l_Par = p_Stream->codecpar;
	double l_Width = l_Par->width;
	const double l_Height = l_Par->height;
	const AVRational l_Aspect = av_guess_sample_aspect_ratio(p_Format, const_cast<AVStream*>(p_Stream), nullptr);
	if (l_Aspect.num > 0 && l_Aspect.den > 0)
		l_Width *= static_cast<double>(l_Aspect.num) / static_cast<double>(l_Aspect.den);

	Geometry l_Result;
	if (const AVPacketSideData* l_Data = av_packet_side_data_get(l_Par->coded_side_data, l_Par->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX); l_Data != nullptr && l_Data->size >= 9 * sizeof(int32_t))
	{
		const double l_Angle = av_display_rotation_get(reinterpret_cast<const int32_t*>(l_Data->data));
		if (!std::isnan(l_Angle))
		{
			const int l_Clockwise = ((static_cast<int>(std::lround(-l_Angle)) % 360) + 360) % 360;
			l_Result.quarterTurns = ((l_Clockwise + 45) / 90) % 4;
		}
	}
	const double l_Scale = std::min(1.0, static_cast<double>(p_MaxDimension) / std::max(l_Width, l_Height));
	l_Result.outWidth = std::max(1, static_cast<int>(std::lround(l_Width * l_Scale)));
	l_Result.outHeight = std::max(1, static_cast<int>(std::lround(l_Height * l_Scale)));
	const bool l_Sideways = (l_Result.quarterTurns & 1) != 0;
	l_Result.width = static_cast<uint32_t>(l_Sideways ? l_Result.outHeight : l_Result.outWidth);
	l_Result.height = static_cast<uint32_t>(l_Sideways ? l_Result.outWidth : l_Result.outHeight);
	return l_Result;
}

double containerDuration(const AVFormatContext* p_Format)
{
	return p_Format->duration > 0 ? static_cast<double>(p_Format->duration) / AV_TIME_BASE : 0.0;
}

double containerStart(const AVFormatContext* p_Format)
{
	return p_Format->start_time != AV_NOPTS_VALUE ? static_cast<double>(p_Format->start_time) / AV_TIME_BASE : 0.0;
}

// Turns an RGBA picture by quarter turns clockwise
void rotatePixels(const uint8_t* p_Source, const int p_Width, const int p_Height, const int p_Turns, uint8_t* p_Target)
{
	const uint32_t* l_Src = reinterpret_cast<const uint32_t*>(p_Source);
	uint32_t* l_Dst = reinterpret_cast<uint32_t*>(p_Target);
	switch (p_Turns & 3)
	{
	case 1: // target is p_Height wide
		for (int y = 0; y < p_Height; ++y)
			for (int x = 0; x < p_Width; ++x)
				l_Dst[static_cast<size_t>(x) * static_cast<size_t>(p_Height) + static_cast<size_t>(p_Height - 1 - y)] = l_Src[static_cast<size_t>(y) * static_cast<size_t>(p_Width) + static_cast<size_t>(x)];
		break;
	case 2:
		for (size_t i = 0, n = static_cast<size_t>(p_Width) * static_cast<size_t>(p_Height); i < n; ++i)
			l_Dst[n - 1 - i] = l_Src[i];
		break;
	case 3:
		for (int y = 0; y < p_Height; ++y)
			for (int x = 0; x < p_Width; ++x)
				l_Dst[static_cast<size_t>(p_Width - 1 - x) * static_cast<size_t>(p_Height) + static_cast<size_t>(y)] = l_Src[static_cast<size_t>(y) * static_cast<size_t>(p_Width) + static_cast<size_t>(x)];
		break;
	default:
		std::memcpy(p_Target, p_Source, static_cast<size_t>(p_Width) * static_cast<size_t>(p_Height) * 4);
		break;
	}
}

int colorspaceCode(const AVFrame* p_Frame)
{
	switch (p_Frame->colorspace)
	{
	case AVCOL_SPC_BT709:
		return SWS_CS_ITU709;
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M:
		return SWS_CS_ITU601;
	case AVCOL_SPC_SMPTE240M:
		return SWS_CS_SMPTE240M;
	case AVCOL_SPC_BT2020_NCL:
	case AVCOL_SPC_BT2020_CL:
		return SWS_CS_BT2020;
	default:
		return p_Frame->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
	}
}

bool openDecoder(const AVStream* p_Stream, const bool p_Threaded, AVCodecContext*& p_Context)
{
	const AVCodec* l_Codec = avcodec_find_decoder(p_Stream->codecpar->codec_id);
	if (l_Codec == nullptr)
		return false;
	p_Context = avcodec_alloc_context3(l_Codec);
	if (p_Context == nullptr)
		return false;
	if (avcodec_parameters_to_context(p_Context, p_Stream->codecpar) < 0)
	{
		avcodec_free_context(&p_Context);
		return false;
	}
	p_Context->pkt_timebase = p_Stream->time_base;
	p_Context->thread_count = p_Threaded ? 0 : 1;
	if (avcodec_open2(p_Context, l_Codec, nullptr) < 0)
	{
		avcodec_free_context(&p_Context);
		return false;
	}
	return true;
}

void discardOtherStreams(AVFormatContext* p_Format, const int p_Keep)
{
	for (unsigned i = 0; i < p_Format->nb_streams; ++i)
		p_Format->streams[i]->discard = static_cast<int>(i) == p_Keep ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
}

struct FramePool
{
	std::mutex mutex;
	std::vector<std::vector<uint8_t>> spare;
};
} // namespace

// ------------------------------------------------------------------------------------------------ probe

std::optional<VideoInfo> probe(const std::span<const uint8_t> p_Bytes)
{
	if (p_Bytes.size() < 16)
		return std::nullopt;
	Input l_Input;
	std::string l_Error;
	if (!l_Input.open(p_Bytes, l_Error))
		return std::nullopt;
	const int l_Video = pickVideoStream(l_Input.format);
	if (l_Video < 0)
		return std::nullopt;
	const AVStream* l_Stream = l_Input.format->streams[l_Video];
	const Geometry l_Geometry = computeGeometry(l_Input.format, l_Stream, 1u << 14);
	VideoInfo l_Info;
	l_Info.width = l_Geometry.width;
	l_Info.height = l_Geometry.height;
	l_Info.duration = containerDuration(l_Input.format);
	const AVRational l_Rate = av_guess_frame_rate(l_Input.format, const_cast<AVStream*>(l_Stream), nullptr);
	l_Info.fps = l_Rate.den > 0 ? av_q2d(l_Rate) : 0.0;
	l_Info.hasAudio = pickAudioStream(l_Input.format, l_Video) >= 0;
	return l_Info;
}

// ------------------------------------------------------------------------------------------------ player

struct Player::Impl
{
	Blob blob;
	Options options;
	VideoInfo info;

	std::shared_ptr<FramePool> pool = std::make_shared<FramePool>();

	// Work shared with the decoder threads (m): the latest seek request and the decoded pictures
	std::mutex m;
	std::condition_variable cv;
	bool quit = false;
	uint64_t seekSerial = 0;
	double seekTarget = 0.0;
	std::deque<std::shared_ptr<Frame>> queue;
	bool videoEof = false;
	std::shared_ptr<Frame> current;
	uint64_t frameSerial = 0;

	// The clock (clockMutex)
	mutable std::mutex clockMutex;
	bool isPlaying = false;
	double basePosition = 0.0; // where the wall clock started (or where playback is paused)
	Clock::time_point wallBase{};
	bool stoppedAtEnd = false;

	// Sound
	SDL_AudioStream* audioStream = nullptr;
	bool hasAudioStream = false;
	std::atomic<bool> audioActive{ false };   // the sound device is the clock
	std::atomic<bool> audioFlushing{ false }; // a seek is waiting for the audio thread to empty the device queue
	std::atomic<bool> audioEof{ false };
	std::atomic<double> audioEnd{ 0.0 };      // time of the end of the last sound handed to the device
	std::atomic<bool> looping{ false };

	// Video decoder state handed to its thread
	std::unique_ptr<Input> videoInput;
	AVCodecContext* videoCodec = nullptr;
	int videoIndex = -1;
	Geometry geometry;
	double startTime = 0.0;
	double totalDuration = 0.0;

	std::thread videoThread;
	std::thread audioThread;

	~Impl();

	// ---- clock
	double positionLocked() const;
	[[nodiscard]] double position() const
	{
		std::lock_guard l_Lock(clockMutex);
		return positionLocked();
	}
	void restartClockAt(double p_Seconds);

	// ---- threads
	void videoLoop();
	void audioLoop();
	std::shared_ptr<Frame> makeFrame();
	void pushFrame(std::shared_ptr<Frame> p_Frame, uint64_t p_Epoch);
};

Player::Impl::~Impl()
{
	{
		std::lock_guard l_Lock(m);
		quit = true;
	}
	cv.notify_all();
	if (videoThread.joinable())
		videoThread.join();
	if (audioThread.joinable())
		audioThread.join();
	if (audioStream != nullptr)
		SDL_DestroyAudioStream(audioStream);
	if (videoCodec != nullptr)
		avcodec_free_context(&videoCodec);
}

double Player::Impl::positionLocked() const
{
	double l_Position = basePosition;
	if (audioActive.load() && audioStream != nullptr)
	{
		if (!audioFlushing.load())
		{
			const double l_Queued = static_cast<double>(SDL_GetAudioStreamQueued(audioStream)) / AUDIO_BYTES_PER_SECOND;
			l_Position = std::max(audioEnd.load() - l_Queued, 0.0);
		}
	}
	else if (isPlaying)
	{
		l_Position += std::chrono::duration<double>(Clock::now() - wallBase).count();
	}
	if (totalDuration > 0.0)
		l_Position = std::min(l_Position, totalDuration);
	return std::max(l_Position, 0.0);
}

void Player::Impl::restartClockAt(const double p_Seconds)
{
	basePosition = p_Seconds;
	wallBase = Clock::now();
}

std::shared_ptr<Frame> Player::Impl::makeFrame()
{
	auto l_Frame = std::shared_ptr<Frame>(new Frame, [l_Pool = pool](Frame* p_Frame)
	{
		{
			std::lock_guard l_Lock(l_Pool->mutex);
			if (l_Pool->spare.size() < 4)
				l_Pool->spare.push_back(std::move(p_Frame->pixels));
		}
		delete p_Frame;
	});
	const size_t l_Bytes = static_cast<size_t>(info.width) * info.height * 4;
	{
		std::lock_guard l_Lock(pool->mutex);
		if (!pool->spare.empty())
		{
			l_Frame->pixels = std::move(pool->spare.back());
			pool->spare.pop_back();
		}
	}
	l_Frame->pixels.resize(l_Bytes);
	return l_Frame;
}

void Player::Impl::pushFrame(std::shared_ptr<Frame> p_Frame, const uint64_t p_Epoch)
{
	std::lock_guard l_Lock(m);
	if (p_Epoch != seekSerial)
		return; // made before a seek: stale
	p_Frame->epoch = p_Epoch;
	p_Frame->serial = ++frameSerial;
	queue.push_back(std::move(p_Frame));
}

// ------------------------------------------------------------------------------------------ video thread

void Player::Impl::videoLoop()
{
	AVFormatContext* l_Format = videoInput->format;
	const AVStream* l_Stream = l_Format->streams[videoIndex];
	const double l_TimeBase = av_q2d(l_Stream->time_base);
	const AVRational l_Rate = av_guess_frame_rate(l_Format, const_cast<AVStream*>(l_Stream), nullptr);
	const double l_FrameDuration = l_Rate.num > 0 && l_Rate.den > 0 ? av_q2d(av_inv_q(l_Rate)) : 1.0 / 30.0;

	AVPacket* l_Packet = av_packet_alloc();
	AVFrame* l_Frame = av_frame_alloc();
	AVFrame* l_Candidate = av_frame_alloc(); // the last picture before the seek target: what is shown at the target
	SwsContext* l_Sws = nullptr;
	std::vector<uint8_t> l_Scaled; // when the picture is turned afterwards
	bool l_HaveCandidate = false;
	double l_CandidatePts = 0.0;
	bool l_Draining = false;
	bool l_Eof = false;
	uint64_t l_Handled = 0;
	uint64_t l_Epoch = 0;
	double l_DiscardBefore = -1.0;
	double l_LastPts = -l_FrameDuration;
	Clock::time_point l_LastEmit = Clock::now();

	const auto l_Emit = [&](const AVFrame* p_Source, const double p_Pts, const bool p_Force)
	{
		if (!p_Force && l_DiscardBefore < 0.0)
		{
			// A picture that is already late is skipped while the clock runs (decoding catches up), but a picture
			// still appears every half second so a slow machine shows something
			bool l_Playing;
			{
				std::lock_guard l_Lock(clockMutex);
				l_Playing = isPlaying;
			}
			if (l_Playing && p_Pts < position() - LATE_FRAME_SECONDS && Clock::now() - l_LastEmit < std::chrono::milliseconds(500))
				return;
		}
		l_Sws = sws_getCachedContext(l_Sws, p_Source->width, p_Source->height, static_cast<AVPixelFormat>(p_Source->format), geometry.outWidth, geometry.outHeight, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (l_Sws == nullptr)
			return;
		const int* l_Coefficients = sws_getCoefficients(colorspaceCode(p_Source));
		sws_setColorspaceDetails(l_Sws, l_Coefficients, p_Source->color_range == AVCOL_RANGE_JPEG ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);

		std::shared_ptr<Frame> l_Out = makeFrame();
		const bool l_Turn = geometry.quarterTurns != 0;
		const size_t l_ScaledBytes = static_cast<size_t>(geometry.outWidth) * static_cast<size_t>(geometry.outHeight) * 4;
		uint8_t* l_Target = l_Out->pixels.data();
		if (l_Turn)
		{
			l_Scaled.resize(l_ScaledBytes);
			l_Target = l_Scaled.data();
		}
		uint8_t* const l_Planes[4] = { l_Target, nullptr, nullptr, nullptr };
		const int l_Strides[4] = { geometry.outWidth * 4, 0, 0, 0 };
		sws_scale(l_Sws, p_Source->data, p_Source->linesize, 0, p_Source->height, l_Planes, l_Strides);
		if (l_Turn)
			rotatePixels(l_Scaled.data(), geometry.outWidth, geometry.outHeight, geometry.quarterTurns, l_Out->pixels.data());
		l_Out->pts = p_Pts;
		pushFrame(std::move(l_Out), l_Epoch);
		l_LastEmit = Clock::now();
	};

	const auto l_Handle = [&](AVFrame* p_Decoded)
	{
		double l_Pts = l_LastPts + l_FrameDuration;
		if (p_Decoded->best_effort_timestamp != AV_NOPTS_VALUE)
			l_Pts = static_cast<double>(p_Decoded->best_effort_timestamp) * l_TimeBase - startTime;
		l_LastPts = l_Pts;
		if (l_DiscardBefore >= 0.0 && l_Pts < l_DiscardBefore - 1e-4)
		{
			// Before the seek target: remember the latest, show it only if nothing later turns up
			av_frame_unref(l_Candidate);
			av_frame_ref(l_Candidate, p_Decoded);
			l_CandidatePts = l_Pts;
			l_HaveCandidate = true;
			return;
		}
		if (l_HaveCandidate)
		{
			l_Emit(l_Candidate, l_CandidatePts, true);
			av_frame_unref(l_Candidate);
			l_HaveCandidate = false;
		}
		const bool l_First = l_DiscardBefore >= 0.0;
		l_DiscardBefore = -1.0;
		l_Emit(p_Decoded, l_Pts, l_First);
	};

	while (true)
	{
		double l_SeekTarget = 0.0;
		bool l_Seek = false;
		{
			std::unique_lock l_Lock(m);
			cv.wait_for(l_Lock, WAIT_SLICE, [&] { return quit || seekSerial != l_Handled || (!l_Eof && queue.size() < MAX_QUEUED_FRAMES); });
			if (quit)
				break;
			if (seekSerial != l_Handled)
			{
				l_Handled = seekSerial;
				l_Epoch = seekSerial;
				l_SeekTarget = seekTarget;
				queue.clear();
				videoEof = false;
				l_Seek = true;
			}
			else if (l_Eof || queue.size() >= MAX_QUEUED_FRAMES)
			{
				continue;
			}
		}

		if (l_Seek)
		{
			const int64_t l_Ts = av_rescale_q(static_cast<int64_t>(std::llround((l_SeekTarget + containerStart(l_Format)) * AV_TIME_BASE)), AVRational{ 1, AV_TIME_BASE }, l_Stream->time_base);
			if (av_seek_frame(l_Format, videoIndex, l_Ts, AVSEEK_FLAG_BACKWARD) < 0)
				av_seek_frame(l_Format, videoIndex, l_Ts, AVSEEK_FLAG_ANY);
			avcodec_flush_buffers(videoCodec);
			av_frame_unref(l_Candidate);
			l_HaveCandidate = false;
			l_Draining = false;
			l_Eof = false;
			l_DiscardBefore = std::max(l_SeekTarget, 0.0);
			l_LastPts = l_DiscardBefore - l_FrameDuration;
		}

		if (!l_Draining)
		{
			const int l_Read = av_read_frame(l_Format, l_Packet);
			if (l_Read < 0)
			{
				avcodec_send_packet(videoCodec, nullptr);
				l_Draining = true;
			}
			else
			{
				if (l_Packet->stream_index == videoIndex)
					avcodec_send_packet(videoCodec, l_Packet); // the output is always taken before the next packet
				av_packet_unref(l_Packet);
			}
		}

		while (true)
		{
			const int l_Got = avcodec_receive_frame(videoCodec, l_Frame);
			if (l_Got == AVERROR(EAGAIN))
				break;
			if (l_Got == AVERROR_EOF)
			{
				if (l_HaveCandidate)
				{
					l_Emit(l_Candidate, l_CandidatePts, true);
					av_frame_unref(l_Candidate);
					l_HaveCandidate = false;
				}
				l_Eof = true;
				std::lock_guard l_Lock(m);
				if (l_Epoch == seekSerial)
					videoEof = true;
				break;
			}
			if (l_Got < 0)
			{
				l_Eof = true; // a broken stream ends the video
				std::lock_guard l_Lock(m);
				if (l_Epoch == seekSerial)
					videoEof = true;
				break;
			}
			l_Handle(l_Frame);
			av_frame_unref(l_Frame);
			{
				std::lock_guard l_Lock(m);
				if (quit || seekSerial != l_Handled)
					break;
			}
		}
	}

	sws_freeContext(l_Sws);
	av_frame_free(&l_Candidate);
	av_frame_free(&l_Frame);
	av_packet_free(&l_Packet);
	videoInput.reset();
}

// ------------------------------------------------------------------------------------------ audio thread

void Player::Impl::audioLoop()
{
	Input l_Input;
	std::string l_Error;
	if (!l_Input.open(blob, l_Error))
	{
		audioActive.store(false);
		return;
	}
	const int l_Index = pickAudioStream(l_Input.format, -1);
	AVCodecContext* l_Codec = nullptr;
	if (l_Index < 0 || !openDecoder(l_Input.format->streams[l_Index], false, l_Codec))
	{
		audioActive.store(false);
		return;
	}
	discardOtherStreams(l_Input.format, l_Index);
	AVFormatContext* l_Format = l_Input.format;
	const AVStream* l_Stream = l_Format->streams[l_Index];
	const double l_TimeBase = av_q2d(l_Stream->time_base);

	AVPacket* l_Packet = av_packet_alloc();
	AVFrame* l_Frame = av_frame_alloc();
	SwrContext* l_Swr = nullptr;
	AVChannelLayout l_OutLayout{};
	av_channel_layout_default(&l_OutLayout, AUDIO_CHANNELS);
	std::vector<float> l_Samples;
	bool l_Draining = false;
	bool l_Eof = false;
	uint64_t l_Handled = 0;
	double l_Target = 0.0;
	bool l_FirstPush = true;
	int l_SwrRate = 0;
	int l_SwrFormat = -1;
	AVChannelLayout l_SwrLayout{};

	const auto l_Push = [&](const AVFrame* p_Source)
	{
		if (l_Swr == nullptr || l_SwrRate != p_Source->sample_rate || l_SwrFormat != p_Source->format || av_channel_layout_compare(&l_SwrLayout, &p_Source->ch_layout) != 0)
		{
			swr_free(&l_Swr);
			av_channel_layout_uninit(&l_SwrLayout);
			if (swr_alloc_set_opts2(&l_Swr, &l_OutLayout, AV_SAMPLE_FMT_FLT, AUDIO_RATE, &p_Source->ch_layout, static_cast<AVSampleFormat>(p_Source->format), p_Source->sample_rate, 0, nullptr) < 0 || swr_init(l_Swr) < 0)
			{
				swr_free(&l_Swr);
				return;
			}
			av_channel_layout_copy(&l_SwrLayout, &p_Source->ch_layout);
			l_SwrRate = p_Source->sample_rate;
			l_SwrFormat = p_Source->format;
		}
		const int l_Capacity = swr_get_out_samples(l_Swr, p_Source->nb_samples) + 64;
		l_Samples.resize(static_cast<size_t>(l_Capacity) * AUDIO_CHANNELS);
		uint8_t* l_Out[1] = { reinterpret_cast<uint8_t*>(l_Samples.data()) };
		int l_Count = swr_convert(l_Swr, l_Out, l_Capacity, const_cast<const uint8_t**>(p_Source->extended_data), p_Source->nb_samples);
		if (l_Count <= 0)
			return;

		double l_Start = l_Target;
		if (p_Source->best_effort_timestamp != AV_NOPTS_VALUE)
			l_Start = static_cast<double>(p_Source->best_effort_timestamp) * l_TimeBase - containerStart(l_Format);
		// Sound before the seek target is dropped
		int l_Skip = 0;
		if (l_Start < l_Target)
			l_Skip = static_cast<int>(std::lround((l_Target - l_Start) * AUDIO_RATE));
		if (l_Skip >= l_Count)
			return;
		l_Count -= l_Skip;
		const float* l_Data = l_Samples.data() + static_cast<size_t>(l_Skip) * AUDIO_CHANNELS;
		if (l_FirstPush)
		{
			audioEnd.store(std::max(l_Start, l_Target));
			l_FirstPush = false;
		}
		SDL_PutAudioStreamData(audioStream, l_Data, l_Count * AUDIO_CHANNELS * static_cast<int>(sizeof(float)));
		audioEnd.store(audioEnd.load() + static_cast<double>(l_Count) / AUDIO_RATE);
	};

	while (true)
	{
		bool l_Playing;
		{
			std::lock_guard l_Lock(clockMutex);
			l_Playing = isPlaying;
		}
		bool l_Seek = false;
		{
			std::unique_lock l_Lock(m);
			cv.wait_for(l_Lock, WAIT_SLICE, [&] { return quit || seekSerial != l_Handled; });
			if (quit)
				break;
			if (seekSerial != l_Handled)
			{
				l_Handled = seekSerial;
				l_Target = seekTarget;
				l_Seek = true;
			}
		}

		if (l_Seek)
		{
			const int64_t l_Ts = av_rescale_q(static_cast<int64_t>(std::llround((l_Target + containerStart(l_Format)) * AV_TIME_BASE)), AVRational{ 1, AV_TIME_BASE }, l_Stream->time_base);
			if (av_seek_frame(l_Format, l_Index, l_Ts, AVSEEK_FLAG_BACKWARD) < 0)
				av_seek_frame(l_Format, l_Index, l_Ts, AVSEEK_FLAG_ANY);
			avcodec_flush_buffers(l_Codec);
			swr_free(&l_Swr);
			SDL_ClearAudioStream(audioStream);
			l_Draining = false;
			l_Eof = false;
			l_FirstPush = true;
			audioEnd.store(l_Target);
			audioEof.store(false);
			audioFlushing.store(false);
		}
		if (l_Eof)
			continue;

		const double l_Limit = l_Playing ? AUDIO_QUEUE_PLAYING : AUDIO_QUEUE_PAUSED;
		// Decode until enough sound is waiting on the device
		while (true)
		{
			const double l_Queued = static_cast<double>(SDL_GetAudioStreamQueued(audioStream)) / AUDIO_BYTES_PER_SECOND;
			{
				std::lock_guard l_Lock(m);
				if (quit || seekSerial != l_Handled)
					break;
			}
			if (l_Queued >= l_Limit)
				break;

			if (!l_Draining)
			{
				const int l_Read = av_read_frame(l_Format, l_Packet);
				if (l_Read < 0)
				{
					avcodec_send_packet(l_Codec, nullptr);
					l_Draining = true;
				}
				else
				{
					if (l_Packet->stream_index == l_Index)
						avcodec_send_packet(l_Codec, l_Packet);
					av_packet_unref(l_Packet);
				}
			}
			bool l_Finished = false;
			while (true)
			{
				const int l_Got = avcodec_receive_frame(l_Codec, l_Frame);
				if (l_Got == AVERROR(EAGAIN))
					break;
				if (l_Got < 0)
				{
					l_Finished = true;
					break;
				}
				l_Push(l_Frame);
				av_frame_unref(l_Frame);
			}
			if (l_Finished)
			{
				l_Eof = true;
				audioEof.store(true);
				break;
			}
		}
	}

	swr_free(&l_Swr);
	av_channel_layout_uninit(&l_SwrLayout);
	av_channel_layout_uninit(&l_OutLayout);
	av_frame_free(&l_Frame);
	av_packet_free(&l_Packet);
	avcodec_free_context(&l_Codec);
}

// ------------------------------------------------------------------------------------------------ API

Player::Player() : m_Impl(std::make_unique<Impl>())
{
}

Player::~Player() = default;

std::unique_ptr<Player> Player::open(Blob p_Data, const Options& p_Options, std::string& p_Error)
{
	if (!p_Data || p_Data->empty())
	{
		p_Error = "The video is empty.";
		return nullptr;
	}
	std::unique_ptr<Player> l_Player(new Player);
	Impl& l_Impl = *l_Player->m_Impl;
	l_Impl.blob = std::move(p_Data);
	l_Impl.options = p_Options;
	l_Impl.looping.store(p_Options.loop);

	l_Impl.videoInput = std::make_unique<Input>();
	if (!l_Impl.videoInput->open(l_Impl.blob, p_Error))
		return nullptr;
	AVFormatContext* l_Format = l_Impl.videoInput->format;
	l_Impl.videoIndex = pickVideoStream(l_Format);
	if (l_Impl.videoIndex < 0)
	{
		p_Error = "The file has no picture.";
		return nullptr;
	}
	const AVStream* l_Stream = l_Format->streams[l_Impl.videoIndex];
	if (!openDecoder(l_Stream, true, l_Impl.videoCodec))
	{
		p_Error = "Inkwell cannot decode this kind of video.";
		return nullptr;
	}
	discardOtherStreams(l_Format, l_Impl.videoIndex);
	l_Impl.geometry = computeGeometry(l_Format, l_Stream, std::max<uint32_t>(p_Options.maxDimension, 16));
	l_Impl.startTime = containerStart(l_Format);
	l_Impl.totalDuration = containerDuration(l_Format);
	l_Impl.info.width = l_Impl.geometry.width;
	l_Impl.info.height = l_Impl.geometry.height;
	l_Impl.info.duration = l_Impl.totalDuration;
	const AVRational l_Rate = av_guess_frame_rate(l_Format, const_cast<AVStream*>(l_Stream), nullptr);
	l_Impl.info.fps = l_Rate.den > 0 ? av_q2d(l_Rate) : 0.0;
	l_Impl.hasAudioStream = pickAudioStream(l_Format, l_Impl.videoIndex) >= 0;
	l_Impl.info.hasAudio = l_Impl.hasAudioStream;

	if (l_Impl.hasAudioStream && SDL_InitSubSystem(SDL_INIT_AUDIO))
	{
		const SDL_AudioSpec l_Spec{ SDL_AUDIO_F32, AUDIO_CHANNELS, AUDIO_RATE };
		l_Impl.audioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &l_Spec, nullptr, nullptr);
		if (l_Impl.audioStream == nullptr)
			spdlog::info("No sound for this video: {}", SDL_GetError());
		else
			SDL_SetAudioStreamGain(l_Impl.audioStream, p_Options.volume);
	}
	l_Impl.audioActive.store(l_Impl.audioStream != nullptr);

	l_Impl.videoThread = std::thread([l_Self = &l_Impl] { l_Self->videoLoop(); });
	if (l_Impl.audioStream != nullptr)
		l_Impl.audioThread = std::thread([l_Self = &l_Impl] { l_Self->audioLoop(); });
	return l_Player;
}

const VideoInfo& Player::info() const
{
	return m_Impl->info;
}

void Player::play()
{
	Impl& l_Impl = *m_Impl;
	{
		std::lock_guard l_Lock(l_Impl.clockMutex);
		if (l_Impl.isPlaying)
			return;
		l_Impl.basePosition = l_Impl.positionLocked();
		l_Impl.isPlaying = true;
		l_Impl.stoppedAtEnd = false;
		l_Impl.wallBase = Clock::now();
	}
	if (l_Impl.audioStream != nullptr)
		SDL_ResumeAudioStreamDevice(l_Impl.audioStream);
	l_Impl.cv.notify_all();
}

void Player::pause()
{
	Impl& l_Impl = *m_Impl;
	{
		std::lock_guard l_Lock(l_Impl.clockMutex);
		if (!l_Impl.isPlaying)
			return;
		l_Impl.basePosition = l_Impl.positionLocked();
		l_Impl.isPlaying = false;
	}
	if (l_Impl.audioStream != nullptr)
		SDL_PauseAudioStreamDevice(l_Impl.audioStream);
}

void Player::seek(const double p_Seconds)
{
	Impl& l_Impl = *m_Impl;
	double l_Target = std::max(p_Seconds, 0.0);
	if (l_Impl.totalDuration > 0.0)
		l_Target = std::min(l_Target, l_Impl.totalDuration);
	{
		std::lock_guard l_Lock(l_Impl.clockMutex);
		l_Impl.basePosition = l_Target;
		l_Impl.wallBase = Clock::now();
		l_Impl.stoppedAtEnd = false;
		if (l_Impl.audioStream != nullptr)
		{
			l_Impl.audioActive.store(true);
			l_Impl.audioFlushing.store(true);
			l_Impl.audioEnd.store(l_Target);
			l_Impl.audioEof.store(false);
		}
	}
	{
		std::lock_guard l_Lock(l_Impl.m);
		l_Impl.seekTarget = l_Target;
		++l_Impl.seekSerial;
		l_Impl.queue.clear();
		l_Impl.videoEof = false;
	}
	l_Impl.cv.notify_all();
}

void Player::setLoop(const bool p_Loop)
{
	m_Impl->looping.store(p_Loop);
}

void Player::setVolume(const float p_Gain)
{
	if (m_Impl->audioStream != nullptr)
		SDL_SetAudioStreamGain(m_Impl->audioStream, std::clamp(p_Gain, 0.f, 1.f));
}

bool Player::playing() const
{
	std::lock_guard l_Lock(m_Impl->clockMutex);
	return m_Impl->isPlaying;
}

bool Player::ended() const
{
	std::lock_guard l_Lock(m_Impl->clockMutex);
	return m_Impl->stoppedAtEnd;
}

bool Player::looping() const
{
	return m_Impl->looping.load();
}

bool Player::audioWorks() const
{
	return m_Impl->audioStream != nullptr;
}

double Player::position() const
{
	return m_Impl->position();
}

double Player::duration() const
{
	return m_Impl->totalDuration;
}

void Player::tick()
{
	Impl& l_Impl = *m_Impl;
	double l_Position;
	bool l_Playing;
	{
		std::lock_guard l_Lock(l_Impl.clockMutex);
		l_Playing = l_Impl.isPlaying;
		if (!l_Playing)
			return;
		// The sound ran out before the picture does: carry on by the wall clock
		if (l_Impl.audioActive.load() && l_Impl.audioEof.load() && !l_Impl.audioFlushing.load() && l_Impl.audioStream != nullptr && SDL_GetAudioStreamQueued(l_Impl.audioStream) == 0)
		{
			l_Impl.audioActive.store(false);
			l_Impl.basePosition = l_Impl.audioEnd.load();
			l_Impl.wallBase = Clock::now();
		}
		l_Position = l_Impl.positionLocked();
	}
	bool l_AtEnd;
	if (l_Impl.totalDuration > 0.0)
	{
		l_AtEnd = l_Position >= l_Impl.totalDuration - 0.03;
	}
	else
	{
		std::lock_guard l_Lock(l_Impl.m);
		l_AtEnd = l_Impl.videoEof && l_Impl.queue.empty();
	}
	if (!l_AtEnd)
		return;
	if (l_Impl.looping.load())
	{
		seek(0.0);
		return;
	}
	pause();
	std::lock_guard l_Lock(l_Impl.clockMutex);
	l_Impl.stoppedAtEnd = true;
	if (l_Impl.totalDuration > 0.0)
		l_Impl.basePosition = l_Impl.totalDuration;
}

std::shared_ptr<const Frame> Player::currentFrame()
{
	Impl& l_Impl = *m_Impl;
	const double l_Position = l_Impl.position();
	const double l_Tolerance = l_Impl.info.fps > 0.0 ? 0.5 / l_Impl.info.fps : 0.02;
	std::lock_guard l_Lock(l_Impl.m);
	int l_Pick = -1;
	for (size_t i = 0; i < l_Impl.queue.size(); ++i)
	{
		if (l_Impl.queue[i]->pts <= l_Position + l_Tolerance)
			l_Pick = static_cast<int>(i);
	}
	if (l_Pick < 0 && (!l_Impl.current || l_Impl.current->epoch != l_Impl.seekSerial) && !l_Impl.queue.empty())
		l_Pick = 0; // nothing due yet but nothing shown either: the first picture will do
	if (l_Pick >= 0)
	{
		l_Impl.current = std::move(l_Impl.queue[static_cast<size_t>(l_Pick)]);
		l_Impl.queue.erase(l_Impl.queue.begin(), l_Impl.queue.begin() + l_Pick + 1);
		l_Impl.cv.notify_all();
	}
	return l_Impl.current;
}
} // namespace wb::video
