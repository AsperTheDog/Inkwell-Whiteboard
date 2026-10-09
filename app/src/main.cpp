#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>
#include <exception>
#include <string>
#include <string_view>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <spdlog/spdlog.h>

import wb.app;
import wb.gfx.context;
import wb.io.file;
import wb.platform.input;
import wb.video.media;

namespace
{
// --video-selftest <file>: decodes a video for a few seconds without any window and reports what the player did
int videoSelfTest(const std::string& p_Path)
{
	std::vector<uint8_t> l_Bytes;
	if (const wb::IoResult l_Read = wb::readFile(wb::pathFromUtf8(p_Path), l_Bytes); !l_Read.ok)
	{
		spdlog::error("Cannot read {}", p_Path);
		return 1;
	}
	const auto l_Blob = std::make_shared<const std::vector<uint8_t>>(std::move(l_Bytes));
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
	SDL_SetAppMetadata("Inkwell", "0.1.0", "dev.inkwell.app");
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
