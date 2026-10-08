// App: saving, opening, the unsaved-changes flow, recovery, settings and the dialogs that go with them.
module;
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <spdlog/spdlog.h>

module wb.app;

import wb.brush.stroke_builder;
import wb.editor;
import wb.io.file;
import wb.io.settings;
import wb.math;
import wb.session;
import wb.tools.tool;

namespace wb
{
namespace
{
constexpr const char* FILE_EXTENSION = ".wbrd";
constexpr uint64_t TOAST_NS = 2'200'000'000ull;

std::string describeAge(const int64_t p_Seconds)
{
	const auto l_Count = [](const int64_t p_Value, const char* p_Unit) { return std::to_string(p_Value) + " " + p_Unit + (p_Value == 1 ? "" : "s") + " ago"; };
	if (p_Seconds < 60)
		return "less than a minute ago";
	if (p_Seconds < 3600)
		return l_Count(p_Seconds / 60, "minute");
	if (p_Seconds < 86400)
		return l_Count(p_Seconds / 3600, "hour");
	return l_Count(p_Seconds / 86400, "day");
}
} // namespace

// ------------------------------------------------------------------------------------------------ startup / shutdown

void App::initPersistence()
{
	m_DialogEvent = SDL_RegisterEvents(1);
	if (m_Options.smokeTestFrames > 0)
		return; // the scripted session must not read or write the user's data

	if (char* l_Path = SDL_GetPrefPath("Whiteboard", "Whiteboard"))
	{
		m_PrefDir = pathFromUtf8(l_Path);
		SDL_free(l_Path);
	}
	if (m_PrefDir.empty())
	{
		spdlog::warn("No preferences folder available: settings and autosave are disabled");
		return;
	}
	m_Session.setStorageDirectory(m_PrefDir);
	loadSettings();

	m_Recovery = m_Session.peekRecovery();
	m_RecoveryRequested = m_Recovery.has_value();
	updateTitle();
}

void App::finishPersistence(const bool p_DiscardRecovery)
{
	if (m_PrefDir.empty())
		return;
	saveSettings();
	// A normal exit with nothing unsaved (or the user chose to discard) leaves no recovery copy behind
	if (p_DiscardRecovery || !m_Session.dirty())
		m_Session.finish();
}

void App::loadSettings()
{
	m_Settings.load(m_PrefDir / "settings.ini");
	const Settings& l_S = m_Settings;

	m_DarkTheme = l_S.getBool("ui.dark", m_DarkTheme);
	m_ShowGrid = l_S.getBool("ui.grid", m_ShowGrid);
	m_LastDirectory = l_S.getString("files.lastDirectory");
	for (size_t i = 0; i < m_CustomColor.size(); ++i)
		m_CustomColor[i] = l_S.getFloat("brush.custom" + std::to_string(i), m_CustomColor[i]);

	tools::BrushState& l_Brush = m_Editor.brush();
	l_Brush.color = Color{ l_S.getFloat("brush.r", l_Brush.color.r), l_S.getFloat("brush.g", l_Brush.color.g), l_S.getFloat("brush.b", l_Brush.color.b), 1.f };
	l_Brush.sizePoints = std::clamp(l_S.getFloat("brush.size", l_Brush.sizePoints), 0.5f, 40.f);

	tools::EraserState& l_Eraser = m_Editor.eraser();
	l_Eraser.sizePoints = std::clamp(l_S.getFloat("eraser.size", l_Eraser.sizePoints), 4.f, 120.f);
	l_Eraser.mode = l_S.getInt("eraser.mode", static_cast<long long>(l_Eraser.mode)) == 1 ? tools::EraserMode::Stroke : tools::EraserMode::Segment;

	BrushSettings& l_B = m_Editor.brushSettings();
	l_B.smoothingMinCutoff = l_S.getFloat("feel.minCutoff", l_B.smoothingMinCutoff);
	l_B.smoothingBeta = l_S.getFloat("feel.beta", l_B.smoothingBeta);
	l_B.pressureSmoothing = l_S.getFloat("feel.pressureSmoothing", l_B.pressureSmoothing);
	l_B.pressureGamma = l_S.getFloat("feel.pressureGamma", l_B.pressureGamma);
	l_B.minWidthFraction = l_S.getFloat("feel.minWidth", l_B.minWidthFraction);
	l_B.pressureSensitivity = l_S.getFloat("feel.pressureSensitivity", l_B.pressureSensitivity);
	l_B.simulatePressureForMouse = l_S.getBool("feel.simulatePressure", l_B.simulatePressureForMouse);
	l_B.resampleSpacingPx = l_S.getFloat("feel.resampleSpacing", l_B.resampleSpacingPx);
	l_B.minSampleDistancePx = l_S.getFloat("feel.minSampleDistance", l_B.minSampleDistancePx);
	l_B.simplifyTolerancePx = l_S.getFloat("feel.simplifyTolerance", l_B.simplifyTolerancePx);
	l_B.taperStartPx = l_S.getFloat("feel.taperStart", l_B.taperStartPx);
	l_B.taperEndPx = l_S.getFloat("feel.taperEnd", l_B.taperEndPx);
}

void App::saveSettings()
{
	Settings l_S = m_Settings; // keeps keys written by other versions
	l_S.setBool("ui.dark", m_DarkTheme);
	l_S.setBool("ui.grid", m_ShowGrid);
	l_S.set("files.lastDirectory", m_LastDirectory);
	for (size_t i = 0; i < m_CustomColor.size(); ++i)
		l_S.setFloat("brush.custom" + std::to_string(i), m_CustomColor[i]);

	const tools::BrushState& l_Brush = m_Editor.brush();
	l_S.setFloat("brush.r", l_Brush.color.r);
	l_S.setFloat("brush.g", l_Brush.color.g);
	l_S.setFloat("brush.b", l_Brush.color.b);
	l_S.setFloat("brush.size", l_Brush.sizePoints);

	const tools::EraserState& l_Eraser = m_Editor.eraser();
	l_S.setFloat("eraser.size", l_Eraser.sizePoints);
	l_S.setInt("eraser.mode", l_Eraser.mode == tools::EraserMode::Stroke ? 1 : 0);

	const BrushSettings& l_B = m_Editor.brushSettings();
	l_S.setFloat("feel.minCutoff", l_B.smoothingMinCutoff);
	l_S.setFloat("feel.beta", l_B.smoothingBeta);
	l_S.setFloat("feel.pressureSmoothing", l_B.pressureSmoothing);
	l_S.setFloat("feel.pressureGamma", l_B.pressureGamma);
	l_S.setFloat("feel.minWidth", l_B.minWidthFraction);
	l_S.setFloat("feel.pressureSensitivity", l_B.pressureSensitivity);
	l_S.setBool("feel.simulatePressure", l_B.simulatePressureForMouse);
	l_S.setFloat("feel.resampleSpacing", l_B.resampleSpacingPx);
	l_S.setFloat("feel.minSampleDistance", l_B.minSampleDistancePx);
	l_S.setFloat("feel.simplifyTolerance", l_B.simplifyTolerancePx);
	l_S.setFloat("feel.taperStart", l_B.taperStartPx);
	l_S.setFloat("feel.taperEnd", l_B.taperEndPx);

	if (const IoResult l_Result = l_S.save(m_PrefDir / "settings.ini"); !l_Result.ok)
		spdlog::warn("Could not save settings: {}", l_Result.error);
}

// ------------------------------------------------------------------------------------------------ actions

void App::requestAction(const Action p_Action)
{
	requestRedraw();
	if (m_Options.smokeTestFrames > 0 || !m_Session.dirty())
	{
		performAction(p_Action);
		return;
	}
	m_PromptAction = p_Action;
	m_PromptRequested = true;
}

void App::performAction(const Action p_Action)
{
	requestRedraw();
	switch (p_Action)
	{
	case Action::None:
		return;
	case Action::NewBoard:
		m_Session.newBoard();
		updateTitle();
		showToast("New board");
		return;
	case Action::OpenFile:
		showOpenDialog();
		return;
	case Action::Quit:
		m_Running = false;
		return;
	}
}

void App::requestSave(const bool p_SaveAs, const Action p_After)
{
	if (m_Session.path() && !p_SaveAs)
		saveToPath(*m_Session.path(), p_After);
	else
		showSaveDialog(p_After);
}

void App::saveToPath(const std::filesystem::path& p_Path, const Action p_After)
{
	const IoResult l_Result = m_Session.saveTo(p_Path);
	requestRedraw();
	if (!l_Result.ok)
	{
		showMessage("The board could not be saved.\n\n" + l_Result.error);
		return; // a pending Quit / New / Open is dropped so nothing is lost
	}
	m_LastDirectory = pathToUtf8(p_Path.parent_path());
	updateTitle();
	showToast("Saved " + m_Session.displayName());
	performAction(p_After);
}

void App::openPath(const std::filesystem::path& p_Path)
{
	const IoResult l_Result = m_Session.open(p_Path);
	requestRedraw();
	if (!l_Result.ok)
	{
		showMessage("The board could not be opened.\n\n" + l_Result.error);
		return;
	}
	m_LastDirectory = pathToUtf8(p_Path.parent_path());
	updateTitle();
	showToast("Opened " + m_Session.displayName());
	if (std::string l_Warning = m_Session.takeWarning(); !l_Warning.empty())
		showMessage(std::move(l_Warning));
}

// ------------------------------------------------------------------------------------------------ file dialogs

void App::showOpenDialog()
{
	if (m_DialogKind != DialogKind::None)
		return; // one dialog at a time
	static const SDL_DialogFileFilter s_Filters[] = { { "Whiteboard boards", "wbrd" } };
	m_DialogKind = DialogKind::Open;
	SDL_ShowOpenFileDialog(&App::dialogCallback, this, m_Window.handle(), s_Filters, 1, m_LastDirectory.empty() ? nullptr : m_LastDirectory.c_str(), false);
}

void App::showSaveDialog(const Action p_After)
{
	if (m_DialogKind != DialogKind::None)
		return;
	static const SDL_DialogFileFilter s_Filters[] = { { "Whiteboard boards", "wbrd" } };
	m_DialogKind = DialogKind::Save;
	m_DialogAfter = p_After;

	std::string l_Start;
	if (m_Session.path())
		l_Start = pathToUtf8(*m_Session.path());
	else if (!m_LastDirectory.empty())
		l_Start = pathToUtf8(pathFromUtf8(m_LastDirectory) / (m_Session.displayName() + FILE_EXTENSION));
	SDL_ShowSaveFileDialog(&App::dialogCallback, this, m_Window.handle(), s_Filters, 1, l_Start.empty() ? nullptr : l_Start.c_str());
}

// May run on any thread: hand the result to the main loop and wake it
void SDLCALL App::dialogCallback(void* p_User, const char* const* p_Files, int)
{
	App* l_App = static_cast<App*>(p_User);
	{
		const std::lock_guard<std::mutex> l_Lock(l_App->m_DialogMutex);
		l_App->m_DialogPath = (p_Files != nullptr && p_Files[0] != nullptr) ? std::optional<std::string>(p_Files[0]) : std::nullopt;
		l_App->m_DialogReady = true;
	}
	SDL_Event l_Event{};
	l_Event.type = l_App->m_DialogEvent;
	SDL_PushEvent(&l_Event);
}

void App::handleDialogResult()
{
	std::optional<std::string> l_Path;
	{
		const std::lock_guard<std::mutex> l_Lock(m_DialogMutex);
		if (!m_DialogReady)
			return;
		m_DialogReady = false;
		l_Path = std::move(m_DialogPath);
		m_DialogPath.reset();
	}
	const DialogKind l_Kind = std::exchange(m_DialogKind, DialogKind::None);
	const Action l_After = std::exchange(m_DialogAfter, Action::None);
	requestRedraw();
	if (!l_Path)
		return; // cancelled (or the dialog failed): the board stays as it is

	std::filesystem::path l_File = pathFromUtf8(*l_Path);
	if (l_Kind == DialogKind::Open)
	{
		openPath(l_File);
	}
	else if (l_Kind == DialogKind::Save)
	{
		if (!l_File.has_extension())
			l_File += FILE_EXTENSION;
		saveToPath(l_File, l_After);
	}
}

bool App::handleFileShortcut(const SDL_KeyboardEvent& p_Event)
{
	if ((p_Event.mod & SDL_KMOD_CTRL) == 0 || p_Event.repeat || m_Editor.isBusy())
		return false;
	const bool l_Shift = (p_Event.mod & SDL_KMOD_SHIFT) != 0;
	switch (p_Event.key)
	{
	case SDLK_S:
		requestSave(l_Shift, Action::None);
		return true;
	case SDLK_O:
		requestAction(Action::OpenFile);
		return true;
	case SDLK_N:
		requestAction(Action::NewBoard);
		return true;
	default:
		return false;
	}
}

// ------------------------------------------------------------------------------------------------ title, toast, messages

void App::updateTitle()
{
	std::string l_Title = m_Session.displayName();
	if (m_Session.dirty())
		l_Title += "*";
	l_Title += " - Whiteboard";
	if (l_Title == m_LastTitle)
		return;
	m_LastTitle = l_Title;
	SDL_SetWindowTitle(m_Window.handle(), l_Title.c_str());
}

void App::showToast(std::string p_Text)
{
	m_Toast = std::move(p_Text);
	m_ToastUntilNs = SDL_GetTicksNS() + TOAST_NS;
	requestRedraw();
}

void App::showMessage(std::string p_Text)
{
	spdlog::warn("{}", p_Text);
	m_Message = std::move(p_Text);
	m_MessageRequested = true;
	requestRedraw();
}

// ------------------------------------------------------------------------------------------------ UI (temporary ImGui)

void App::buildToast()
{
	if (m_Toast.empty())
		return;
	if (SDL_GetTicksNS() >= m_ToastUntilNs)
	{
		m_Toast.clear();
		return;
	}
	const ImGuiViewport* l_Viewport = ImGui::GetMainViewport();
	const float l_Scale = m_Window.displayScale();
	ImGui::SetNextWindowPos(ImVec2(l_Viewport->WorkPos.x + l_Viewport->WorkSize.x * 0.5f, l_Viewport->WorkPos.y + 18.f * l_Scale), ImGuiCond_Always, ImVec2(0.5f, 0.f));
	ImGui::SetNextWindowBgAlpha(0.88f);
	constexpr ImGuiWindowFlags l_Flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
	if (ImGui::Begin("##toast", nullptr, l_Flags))
		ImGui::TextUnformatted(m_Toast.c_str());
	ImGui::End();
}

void App::buildDialogs()
{
	const ImGuiViewport* l_Viewport = ImGui::GetMainViewport();
	const ImVec2 l_Center(l_Viewport->WorkPos.x + l_Viewport->WorkSize.x * 0.5f, l_Viewport->WorkPos.y + l_Viewport->WorkSize.y * 0.5f);
	const float l_Scale = m_Window.displayScale();
	constexpr ImGuiWindowFlags l_Flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;

	if (m_PromptRequested)
	{
		ImGui::OpenPopup("Unsaved changes");
		m_PromptRequested = false;
	}
	ImGui::SetNextWindowPos(l_Center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	if (ImGui::BeginPopupModal("Unsaved changes", nullptr, l_Flags))
	{
		ImGui::Text("\"%s\" has changes that are not saved.", m_Session.displayName().c_str());
		ImGui::Spacing();
		const Action l_Action = m_PromptAction;
		if (ImGui::Button("Save", ImVec2(110.f * l_Scale, 0.f)))
		{
			ImGui::CloseCurrentPopup();
			m_PromptAction = Action::None;
			requestSave(false, l_Action);
		}
		ImGui::SameLine();
		if (ImGui::Button("Don't save", ImVec2(110.f * l_Scale, 0.f)))
		{
			ImGui::CloseCurrentPopup();
			m_PromptAction = Action::None;
			m_QuitDiscard = l_Action == Action::Quit;
			performAction(l_Action);
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(110.f * l_Scale, 0.f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			ImGui::CloseCurrentPopup();
			m_PromptAction = Action::None;
		}
		ImGui::EndPopup();
	}

	if (m_RecoveryRequested)
	{
		ImGui::OpenPopup("Recover unsaved work");
		m_RecoveryRequested = false;
	}
	ImGui::SetNextWindowPos(l_Center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	if (m_Recovery && ImGui::BeginPopupModal("Recover unsaved work", nullptr, l_Flags))
	{
		const std::string l_Name = m_Recovery->sourcePath.empty() ? std::string("an untitled board") : "\"" + pathToUtf8(pathFromUtf8(m_Recovery->sourcePath).filename()) + "\"";
		ImGui::TextWrapped("Whiteboard closed before %s was saved.", l_Name.c_str());
		ImGui::Text("%llu objects, last autosaved %s.", static_cast<unsigned long long>(m_Recovery->objectCount), describeAge(m_Recovery->ageSeconds).c_str());
		ImGui::Spacing();
		if (ImGui::Button("Recover", ImVec2(130.f * l_Scale, 0.f)))
		{
			ImGui::CloseCurrentPopup();
			m_Recovery.reset();
			if (const IoResult l_Result = m_Session.recover(); l_Result.ok)
			{
				updateTitle();
				showToast("Recovered unsaved work");
			}
			else
			{
				showMessage("The unsaved work could not be recovered.\n\n" + l_Result.error);
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Discard", ImVec2(130.f * l_Scale, 0.f)))
		{
			ImGui::CloseCurrentPopup();
			m_Recovery.reset();
			m_Session.discardRecovery();
		}
		ImGui::EndPopup();
	}

	if (m_MessageRequested)
	{
		ImGui::OpenPopup("Whiteboard##message");
		m_MessageRequested = false;
	}
	ImGui::SetNextWindowPos(l_Center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(0.f, 0.f), ImVec2(520.f * l_Scale, 400.f * l_Scale));
	if (ImGui::BeginPopupModal("Whiteboard##message", nullptr, l_Flags))
	{
		ImGui::PushTextWrapPos(480.f * l_Scale);
		ImGui::TextUnformatted(m_Message.c_str());
		ImGui::PopTextWrapPos();
		ImGui::Spacing();
		if (ImGui::Button("OK", ImVec2(110.f * l_Scale, 0.f)) || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}
} // namespace wb
