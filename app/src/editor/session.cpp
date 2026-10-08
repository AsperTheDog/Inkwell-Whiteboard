module;
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <spdlog/spdlog.h>

module wb.session;

import wb.doc.document;
import wb.editor;
import wb.io.file;
import wb.io.serializer;

namespace wb
{
namespace
{
constexpr uint64_t AUTOSAVE_INTERVAL_NS = 30'000'000'000ull;
constexpr uint64_t ALWAYS_DIRTY = std::numeric_limits<uint64_t>::max();
// Matches no real history state: forces the next autosave attempt
constexpr uint64_t NOTHING_AUTOSAVED = ALWAYS_DIRTY - 1;
} // namespace

Session::~Session()
{
	waitForAutosave();
}

void Session::setStorageDirectory(std::filesystem::path p_Directory)
{
	m_StorageDir = std::move(p_Directory);
}

std::filesystem::path Session::recoveryPath() const
{
	return m_StorageDir / "recovery.wbrd";
}

bool Session::dirty() const
{
	return m_Editor.history().stateId() != m_SavedState;
}

std::string Session::displayName() const
{
	return m_Path ? pathToUtf8(m_Path->filename()) : std::string("Untitled");
}

IoResult Session::saveTo(const std::filesystem::path& p_Path)
{
	const std::vector<uint8_t> l_Bytes = m_Editor.saveBoard({});
	const IoResult l_Result = writeFileAtomic(p_Path, l_Bytes);
	if (!l_Result.ok)
		return l_Result;
	m_Path = p_Path;
	m_SavedState = m_Editor.history().stateId();
	m_AutosavedState = m_SavedState;
	discardRecovery();
	return l_Result;
}

IoResult Session::open(const std::filesystem::path& p_Path)
{
	std::vector<uint8_t> l_Bytes;
	if (const IoResult l_Read = readFile(p_Path, l_Bytes); !l_Read.ok)
		return l_Read;

	BoardMeta l_Meta;
	const LoadResult l_Loaded = m_Editor.loadBoard(l_Bytes, l_Meta);
	if (!l_Loaded.ok)
		return IoResult::failure(pathToUtf8(p_Path.filename()) + ": " + l_Loaded.error);
	if (l_Loaded.skippedObjects > 0)
		m_Warning = std::to_string(l_Loaded.skippedObjects) + " object(s) from a newer version of Whiteboard were left out. Saving will drop them.";

	m_Path = p_Path;
	m_SavedState = m_Editor.history().stateId();
	m_AutosavedState = m_SavedState;
	discardRecovery();
	return {};
}

void Session::newBoard()
{
	m_Editor.newBoard();
	m_Path.reset();
	m_SavedState = m_Editor.history().stateId();
	m_AutosavedState = m_SavedState;
	discardRecovery();
}

void Session::tick(const uint64_t p_NowNs)
{
	reapAutosave();
	if (m_StorageDir.empty() || m_AutosaveJob.valid())
		return;
	const uint64_t l_State = m_Editor.history().stateId();
	if (l_State == m_SavedState || l_State == m_AutosavedState)
		return; // nothing new to protect
	if (m_Editor.isBusy() || p_NowNs - m_LastAutosaveNs < AUTOSAVE_INTERVAL_NS)
		return;

	// Serialize here (the document belongs to this thread), write on a worker so large boards do not stall drawing
	std::vector<uint8_t> l_Bytes = m_Editor.saveBoard(m_Path ? pathToUtf8(*m_Path) : std::string{});
	m_AutosavedState = l_State;
	m_LastAutosaveNs = p_NowNs;
	m_AutosaveJob = std::async(std::launch::async, [l_Path = recoveryPath(), l_Data = std::move(l_Bytes)] { return writeFileAtomic(l_Path, l_Data); });
}

void Session::reapAutosave()
{
	if (!m_AutosaveJob.valid() || m_AutosaveJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return;
	const IoResult l_Result = m_AutosaveJob.get();
	if (!l_Result.ok)
	{
		spdlog::warn("Autosave failed: {}", l_Result.error);
		m_AutosavedState = NOTHING_AUTOSAVED; // try again at the next opportunity
	}
}

void Session::waitForAutosave()
{
	if (m_AutosaveJob.valid())
	{
		m_AutosaveJob.wait();
		reapAutosave();
	}
}

void Session::finish()
{
	waitForAutosave();
	discardRecovery();
}

std::optional<RecoveryInfo> Session::peekRecovery()
{
	if (m_StorageDir.empty())
		return std::nullopt;
	std::error_code l_Error;
	if (!std::filesystem::exists(recoveryPath(), l_Error))
		return std::nullopt;

	std::vector<uint8_t> l_Bytes;
	Document l_Scratch;
	BoardMeta l_Meta;
	if (!readFile(recoveryPath(), l_Bytes).ok || !deserializeBoard(l_Bytes, l_Scratch, l_Meta).ok)
	{
		// Unusable: keep it for inspection but never offer it again
		std::filesystem::rename(recoveryPath(), m_StorageDir / "recovery.damaged.wbrd", l_Error);
		spdlog::warn("Ignoring a damaged recovery file");
		return std::nullopt;
	}

	RecoveryInfo l_Info;
	l_Info.sourcePath = std::move(l_Meta.sourcePath);
	l_Info.objectCount = l_Scratch.size();
	const auto l_Written = std::filesystem::last_write_time(recoveryPath(), l_Error);
	if (!l_Error)
		l_Info.ageSeconds = std::chrono::duration_cast<std::chrono::seconds>(std::filesystem::file_time_type::clock::now() - l_Written).count();
	return l_Info;
}

IoResult Session::recover()
{
	std::vector<uint8_t> l_Bytes;
	if (const IoResult l_Read = readFile(recoveryPath(), l_Bytes); !l_Read.ok)
		return l_Read;
	BoardMeta l_Meta;
	const LoadResult l_Loaded = m_Editor.loadBoard(l_Bytes, l_Meta);
	if (!l_Loaded.ok)
		return IoResult::failure(l_Loaded.error);

	m_Path = l_Meta.sourcePath.empty() ? std::nullopt : std::optional<std::filesystem::path>(pathFromUtf8(l_Meta.sourcePath));
	m_SavedState = ALWAYS_DIRTY; // the recovered work is not on disk anywhere but the recovery file
	m_AutosavedState = m_Editor.history().stateId();
	return {};
}

void Session::discardRecovery()
{
	waitForAutosave(); // a write in flight would bring the file back
	if (!m_StorageDir.empty())
		removeFileQuiet(recoveryPath());
}
} // namespace wb
