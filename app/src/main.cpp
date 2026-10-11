#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>
#include <algorithm>
#include <exception>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <spdlog/spdlog.h>

import wb.app;
import wb.gfx.context;
import wb.io.blob;
import wb.io.file;
import wb.net.connection;
import wb.net.stream;
import wb.platform.input;
import wb.steam;
import wb.text.subtitles;
import wb.video.media;

namespace
{
// --video-selftest <file>: decodes a video for a few seconds without any window and reports what the player did
int videoSelfTest(const std::string& p_Path)
{
	std::string l_OpenError;
	const std::shared_ptr<const wb::ByteSource> l_Blob = wb::FileSource::open(wb::pathFromUtf8(p_Path), l_OpenError); // played from the file, like on a board
	if (!l_Blob)
	{
		spdlog::error("Cannot read {}: {}", p_Path, l_OpenError);
		return 1;
	}
	const auto l_Info = wb::video::probe(*l_Blob);
	if (!l_Info)
	{
		spdlog::error("Not a video");
		return 1;
	}
	spdlog::info("probe: {}x{}, {:.2f}s, {:.2f} fps, audio {}", l_Info->width, l_Info->height, l_Info->duration, l_Info->fps, l_Info->hasAudio);
	std::string l_Error;
	auto l_Player = wb::video::Player::open(l_Blob, {}, l_Error);
	if (!l_Player)
	{
		spdlog::error("open failed: {}", l_Error);
		return 1;
	}
	const auto l_Start = std::chrono::steady_clock::now();
	const auto l_Elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - l_Start).count(); };
	uint64_t l_Last = 0;
	int l_Distinct = 0;
	const auto l_Poll = [&](const double p_Seconds)
	{
		const double l_End = l_Elapsed() + p_Seconds;
		while (l_Elapsed() < l_End)
		{
			l_Player->tick();
			if (const auto l_Frame = l_Player->currentFrame(); l_Frame && l_Frame->serial != l_Last)
			{
				l_Last = l_Frame->serial;
				++l_Distinct;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
	};
	l_Poll(0.3);
	spdlog::info("paused: position {:.3f}, frames shown {}", l_Player->position(), l_Distinct);
	l_Player->setVolume(1.f);
	l_Player->play();
	l_Distinct = 0;
	l_Poll(1.0);
	spdlog::info("playing 1s: position {:.3f}, distinct frames {}", l_Player->position(), l_Distinct);
	l_Player->seek(2.0);
	l_Distinct = 0;
	l_Poll(0.3);
	spdlog::info("after seek to 2.0: position {:.3f}, frames {}", l_Player->position(), l_Distinct);
	l_Player->setLoop(false);
	l_Poll(l_Info->duration + 0.5);
	spdlog::info("after the end: playing {}, ended {}, position {:.3f}", l_Player->playing(), l_Player->ended(), l_Player->position());
	l_Player->setLoop(true);
	l_Player->seek(l_Info->duration - 0.2);
	l_Player->play();
	l_Poll(0.8);
	spdlog::info("looped: playing {}, position {:.3f}", l_Player->playing(), l_Player->position());

	// Sound tracks and subtitles, when the file has some
	const wb::video::VideoInfo& l_Tracks = l_Player->info();
	spdlog::info("tracks: {} sound (default {}), {} subtitle", l_Tracks.audioTracks.size(), l_Tracks.defaultAudioTrack, l_Tracks.subtitleTracks.size());
	for (const wb::video::TrackInfo& l_Track : l_Tracks.audioTracks)
		spdlog::info("  sound: '{}' '{}' {}", l_Track.language, l_Track.title, l_Track.codec);
	for (const wb::video::TrackInfo& l_Track : l_Tracks.subtitleTracks)
		spdlog::info("  subtitles: '{}' '{}' {}", l_Track.language, l_Track.title, l_Track.codec);
	if (!l_Tracks.subtitleTracks.empty() || l_Tracks.audioTracks.size() > 1)
	{
		wb::video::Player::Options l_Options;
		l_Options.subtitleTrack = l_Tracks.subtitleTracks.empty() ? -1 : 0;
		auto l_Second = wb::video::Player::open(l_Blob, l_Options, l_Error);
		if (!l_Second)
		{
			spdlog::error("reopen failed: {}", l_Error);
			return 1;
		}
		l_Second->play();
		std::string l_Shown = "<none>";
		bool l_Switched = false;
		const auto l_From = std::chrono::steady_clock::now();
		while (std::chrono::steady_clock::now() - l_From < std::chrono::milliseconds(3300))
		{
			l_Second->tick();
			(void)l_Second->currentFrame();
			if (std::string l_Now = l_Second->subtitle(); l_Now != l_Shown && (!l_Now.empty() || l_Shown != "<none>"))
			{
				l_Shown = l_Now;
				std::replace(l_Now.begin(), l_Now.end(), '\n', '|');
				spdlog::info("t={:.2f} subtitle: '{}'", l_Second->position(), l_Now);
			}
			if (!l_Switched && l_Second->position() > 1.0 && l_Tracks.audioTracks.size() > 1)
			{
				l_Switched = true;
				spdlog::info("switching sound from track {} at {:.3f}", l_Second->audioTrack(), l_Second->position());
				l_Second->setAudioTrack(1);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
		spdlog::info("end: sound track {}, position {:.3f}, subtitle track {}", l_Second->audioTrack(), l_Second->position(), l_Second->subtitleTrack());
		l_Second->setSubtitleTrack(-1);
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		spdlog::info("subtitles off: '{}'", l_Second->subtitle());
	}

	// WB_SELFTEST_SRT=<file>: subtitles from a file of their own instead of the video's
	if (const char* l_Srt = std::getenv("WB_SELFTEST_SRT"))
	{
		std::vector<uint8_t> l_Bytes;
		if (wb::readFile(wb::pathFromUtf8(l_Srt), l_Bytes).ok)
		{
			auto l_Cues = std::make_shared<const std::vector<wb::SubtitleCue>>(wb::parseSubtitles(std::string_view(reinterpret_cast<const char*>(l_Bytes.data()), l_Bytes.size())));
			spdlog::info("subtitle file: {} lines", l_Cues->size());
			l_Player->setExternalSubtitles(l_Cues);
			for (const double l_At : { 0.2, 1.0, 2.3, 2.95 })
			{
				l_Player->setLoop(false);
				l_Player->seek(l_At);
				std::this_thread::sleep_for(std::chrono::milliseconds(60));
				std::string l_Shown = l_Player->subtitle();
				std::replace(l_Shown.begin(), l_Shown.end(), '\n', '|');
				spdlog::info("external at {:.2f}: '{}'", l_At, l_Shown);
			}
		}
	}
	return 0;
}

// --video-streamtest <file>: the file "arrives" slowly into a temporary file while it is played, then another copy stops
// arriving half way. Nothing may hang, and the first must play.
int videoStreamTest(const std::string& p_Path)
{
	std::vector<uint8_t> l_Data;
	if (const wb::IoResult l_Read = wb::readFile(wb::pathFromUtf8(p_Path), l_Data); !l_Read.ok)
	{
		spdlog::error("Cannot read {}", p_Path);
		return 1;
	}
	int l_Bad = 0;
	const auto l_Run = [&](const double p_FeedSeconds, const double p_Fraction, const char* p_Name)
	{
		std::string l_Error;
		const std::shared_ptr<wb::FileSource> l_File = wb::FileSource::createTemporary(std::filesystem::temp_directory_path(), l_Data.size(), l_Error);
		if (!l_File)
		{
			spdlog::error("{}: {}", p_Name, l_Error);
			++l_Bad;
			return;
		}
		const auto l_Begin = std::chrono::steady_clock::now();
		std::thread l_Feeder([&]
		{
			const size_t l_Limit = static_cast<size_t>(static_cast<double>(l_Data.size()) * p_Fraction);
			const size_t l_Step = std::max<size_t>(1, l_Data.size() / 150);
			std::string l_Problem;
			for (size_t l_At = 0; l_At < l_Limit; l_At += l_Step)
			{
				(void)l_File->append(std::span<const uint8_t>(l_Data).subspan(l_At, std::min(l_Step, l_Data.size() - l_At)), l_Problem);
				std::this_thread::sleep_for(std::chrono::duration<double>(p_FeedSeconds / 150.0));
			}
			if (p_Fraction < 1.0)
				l_File->abandon();
		});
		std::unique_ptr<wb::video::Player> l_Player = wb::video::Player::open(l_File, {}, l_Error);
		const double l_OpenedAt = std::chrono::duration<double>(std::chrono::steady_clock::now() - l_Begin).count();
		int l_Frames = 0;
		if (l_Player)
		{
			l_Player->play();
			uint64_t l_Last = 0;
			const auto l_PlayStart = std::chrono::steady_clock::now();
			while (std::chrono::steady_clock::now() - l_PlayStart < std::chrono::milliseconds(1500))
			{
				l_Player->tick();
				if (const auto l_Frame = l_Player->currentFrame(); l_Frame && l_Frame->serial != l_Last)
				{
					l_Last = l_Frame->serial;
					++l_Frames;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(4));
			}
		}
		l_Player.reset();
		l_Feeder.join();
		spdlog::info("{}: opened {} after {:.2f}s, {} distinct frames, file complete {}", p_Name, l_Player == nullptr && !l_Frames ? "no" : "yes", l_OpenedAt, l_Frames, l_File->complete());
		if (p_Fraction >= 1.0 && l_Frames < 10)
			++l_Bad;
	};
	l_Run(2.0, 1.0, "slowly arriving file");
	l_Run(1.0, 0.5, "file that stops arriving");
	spdlog::info(l_Bad == 0 ? "stream test OK" : "stream test FAILED");
	return l_Bad == 0 ? 0 : 1;
}

// --steam-selftest [steam id]: connects to a host through Steam (this very user by default) and sends a frame through the
// encrypted connection and back, then lists the sessions other people advertise. Needs Steam running and signed in.
int steamSelfTest(const std::string& p_HostId)
{
	wb::steam::Steam l_Steam;
	if (!l_Steam.init())
	{
		spdlog::error("Steam is not available: {}", l_Steam.initNote());
		return 1;
	}
	spdlog::info("Signed in as {} ({})", l_Steam.userName(), l_Steam.userId());
	const uint64_t l_Host = p_HostId.empty() ? l_Steam.userId() : std::strtoull(p_HostId.c_str(), nullptr, 10);

	std::unique_ptr<wb::net::Listener> l_Listener = l_Steam.listen();
	if (!l_Listener)
	{
		spdlog::error("Could not open a listen socket");
		return 1;
	}
	std::unique_ptr<wb::net::Stream> l_Accepted;
	std::atomic<bool> l_Done{ false };
	std::thread l_AcceptThread([&]
	{
		while (!l_Done.load() && !l_Accepted)
			l_Accepted = l_Listener->accept(200);
	});

	std::atomic<bool> l_Cancel{ false };
	std::string l_Error;
	const auto l_Started = std::chrono::steady_clock::now();
	std::unique_ptr<wb::net::Stream> l_Client = l_Steam.connector(l_Host)(l_Cancel, l_Error, [](const std::string& p_Line) { spdlog::info("  {}", p_Line); });
	if (!l_Client)
	{
		spdlog::error("Connecting failed: {}", l_Error);
		l_Done = true;
		l_AcceptThread.join();
		return 1;
	}
	spdlog::info("Connected in {:.2f} s", std::chrono::duration<double>(std::chrono::steady_clock::now() - l_Started).count());
	while (!l_Accepted && std::chrono::steady_clock::now() - l_Started < std::chrono::seconds(20))
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	l_Done = true;
	l_AcceptThread.join();
	if (!l_Accepted)
	{
		spdlog::error("The listener never saw the connection");
		return 1;
	}
	spdlog::info("The host side sees {}", l_Accepted->peerAddress());

	using wb::net::Connection;
	std::mutex l_Mutex;
	std::vector<uint8_t> l_AtServer;
	std::vector<uint8_t> l_AtClient;
	Connection l_Server(std::move(l_Accepted), Connection::Role::Server, [&](wb::net::Frame p_Frame)
	{
		const std::lock_guard l_Lock(l_Mutex);
		l_AtServer = p_Frame.payload;
	}, [](const std::string&) {});
	Connection l_ClientConnection(std::move(l_Client), Connection::Role::Client, [&](wb::net::Frame p_Frame)
	{
		const std::lock_guard l_Lock(l_Mutex);
		l_AtClient = p_Frame.payload;
	}, [](const std::string&) {});
	l_Server.setMaxFrame(1u << 22);
	l_ClientConnection.setMaxFrame(1u << 22);
	l_Server.start();
	l_ClientConnection.start();

	std::vector<uint8_t> l_Payload(1u << 20);
	for (size_t i = 0; i < l_Payload.size(); ++i)
		l_Payload[i] = static_cast<uint8_t>(i * 31 + 7);
	const auto l_Sent = std::chrono::steady_clock::now();
	l_ClientConnection.send(7, l_Payload);
	const auto l_WaitFor = [&](std::vector<uint8_t>& p_Target, const double p_Seconds)
	{
		const auto l_Until = std::chrono::steady_clock::now() + std::chrono::duration<double>(p_Seconds);
		while (std::chrono::steady_clock::now() < l_Until)
		{
			{
				const std::lock_guard l_Lock(l_Mutex);
				if (!p_Target.empty())
					return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return false;
	};
	bool l_Ok = l_WaitFor(l_AtServer, 30.0);
	if (l_Ok)
	{
		const double l_Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - l_Sent).count();
		spdlog::info("1 MiB client -> host in {:.2f} s ({:.0f} KiB/s), intact: {}", l_Seconds, 1024.0 / l_Seconds, l_AtServer == l_Payload);
		l_Ok = l_AtServer == l_Payload;
		l_Server.send(8, l_AtServer);
		l_Ok = l_WaitFor(l_AtClient, 30.0) && l_AtClient == l_Payload && l_Ok;
		spdlog::info("host -> client echo: {}", l_AtClient == l_Payload ? "intact" : "MISSING OR CORRUPT");
	}
	else
	{
		spdlog::error("Nothing arrived at the host");
	}
	l_ClientConnection.close();
	l_Server.close();

	// Join Game / invites: the connect string others would see
	l_Steam.setJoinable(true);
	for (int i = 0; i < 20; ++i)
	{
		l_Steam.pump();
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	l_Steam.setJoinable(false);

	spdlog::info(l_Ok ? "Steam self-test OK" : "Steam self-test FAILED");
	return l_Ok ? 0 : 1;
}

wb::AppOptions parseOptions(const int p_Argc, char** p_Argv)
{
	wb::AppOptions l_Options{};
	for (int i = 1; i < p_Argc; ++i)
	{
		const std::string_view l_Arg = p_Argv[i];
		if (l_Arg == "--smoke-test")
			l_Options.smokeTestFrames = 240;
		else if (l_Arg.starts_with("--smoke-test="))
			l_Options.smokeTestFrames = static_cast<uint32_t>(std::strtoul(p_Argv[i] + 13, nullptr, 10));
		else if (l_Arg.starts_with("--smoke-zoom="))
			l_Options.smokeZoom = std::strtod(p_Argv[i] + 13, nullptr);
		else if (l_Arg == "--smoke-dark")
			l_Options.smokeDark = true;
		else if (l_Arg.starts_with("--smoke-ui="))
			l_Options.smokeUi = p_Argv[i] + 11;
		else if (l_Arg.starts_with("--perf-test="))
			l_Options.perfStrokes = static_cast<uint32_t>(std::strtoul(p_Argv[i] + 12, nullptr, 10));
		else if (l_Arg == "--screenshot" && i + 1 < p_Argc)
			l_Options.screenshotPath = p_Argv[++i];
		else if (l_Arg == "+connect_steam" && i + 1 < p_Argc)
			l_Options.steamJoinHost = wb::steam::Steam::parseConnectString(std::string("+connect_steam ") + p_Argv[++i]);
	}
	return l_Options;
}
} // namespace

int main(int p_Argc, char** p_Argv)
{
	for (int i = 1; i < p_Argc; ++i)
	{
		if (std::string_view(p_Argv[i]) == "--steam-selftest")
		{
			SDL_SetMainReady();
			SDL_Init(SDL_INIT_VIDEO);
			const int l_Result = steamSelfTest(i + 1 < p_Argc ? p_Argv[i + 1] : "");
			SDL_Quit();
			return l_Result;
		}
	}
	for (int i = 1; i + 1 < p_Argc; ++i)
	{
		if (std::string_view(p_Argv[i]) == "--video-streamtest")
		{
			SDL_SetMainReady();
			SDL_Init(SDL_INIT_VIDEO);
			const int l_Result = videoStreamTest(p_Argv[i + 1]);
			SDL_Quit();
			return l_Result;
		}
		if (std::string_view(p_Argv[i]) == "--video-selftest")
		{
			SDL_SetMainReady();
			SDL_Init(SDL_INIT_VIDEO);
			const int l_Result = videoSelfTest(p_Argv[i + 1]);
			SDL_Quit();
			return l_Result;
		}
	}
	const wb::AppOptions l_Options = parseOptions(p_Argc, p_Argv);

#ifdef WB_DEBUG
	spdlog::set_level(spdlog::level::debug);
#endif

	SDL_SetMainReady();
	SDL_SetAppMetadata("Inkwell", "1.3.0", "dev.inkwell.app");
	wb::platform::InputRouter::configureHints();
	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		spdlog::critical("SDL_Init failed: {}", SDL_GetError());
		return 1;
	}

	int l_ExitCode = 0;
	try
	{
		wb::App l_App(l_Options);
		l_App.run();

		// Smoke tests fail on any validation message
		const wb::gfx::ValidationStats l_Validation = wb::gfx::GraphicsContext::validationStats();
		if (l_Options.smokeTestFrames > 0 && (l_Validation.errors > 0 || l_Validation.warnings > 0))
		{
			spdlog::error("Smoke test: {} validation errors, {} warnings", l_Validation.errors, l_Validation.warnings);
			l_ExitCode = 3;
		}
		if (l_Options.smokeTestFrames > 0 && l_App.failed())
			l_ExitCode = 4;
	}
	catch (const wb::gfx::UnsupportedGpuError& l_Error)
	{
		spdlog::critical("{}", l_Error.what());
		if (l_Options.smokeTestFrames == 0)
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Inkwell - unsupported GPU", l_Error.what(), nullptr);
		l_ExitCode = 2;
	}
	catch (const std::exception& l_Error)
	{
		spdlog::critical("Fatal error: {}", l_Error.what());
		const std::string l_Message = std::string("Inkwell hit a fatal error and has to close.\n\n") + l_Error.what();
		if (l_Options.smokeTestFrames == 0)
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Inkwell", l_Message.c_str(), nullptr);
		l_ExitCode = 1;
	}

	SDL_Quit();
	return l_ExitCode;
}
