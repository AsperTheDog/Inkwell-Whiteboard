#include <cstdint>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <spdlog/spdlog.h>

import wb.app;
import wb.gfx.context;
import wb.platform.input;

namespace
{
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
		else if (l_Arg == "--screenshot" && i + 1 < p_Argc)
			l_Options.screenshotPath = p_Argv[++i];
	}
	return l_Options;
}
} // namespace

int main(int p_Argc, char** p_Argv)
{
	const wb::AppOptions l_Options = parseOptions(p_Argc, p_Argv);

#ifdef WB_DEBUG
	spdlog::set_level(spdlog::level::debug);
#endif

	SDL_SetMainReady();
	SDL_SetAppMetadata("Whiteboard", "0.1.0", "dev.whiteboard.app");
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
	}
	catch (const wb::gfx::UnsupportedGpuError& l_Error)
	{
		spdlog::critical("{}", l_Error.what());
		if (l_Options.smokeTestFrames == 0)
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Whiteboard - unsupported GPU", l_Error.what(), nullptr);
		l_ExitCode = 2;
	}
	catch (const std::exception& l_Error)
	{
		spdlog::critical("Fatal error: {}", l_Error.what());
		const std::string l_Message = std::string("Whiteboard hit a fatal error and has to close.\n\n") + l_Error.what();
		if (l_Options.smokeTestFrames == 0)
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Whiteboard", l_Message.c_str(), nullptr);
		l_ExitCode = 1;
	}

	SDL_Quit();
	return l_ExitCode;
}
