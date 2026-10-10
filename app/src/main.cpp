#include <chrono>
#include <cstdint>
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
import wb.platform.input;
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
	}
	return l_Options;
}
} // namespace

int main(int p_Argc, char** p_Argv)
{
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
	SDL_SetAppMetadata("Inkwell", "1.0.1", "dev.inkwell.app");
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
