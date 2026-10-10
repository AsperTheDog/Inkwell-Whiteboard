module;
#include <algorithm>
#include <chrono>
#include <memory>
#include <cstdint>
#include <filesystem>
#include <future>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <spdlog/spdlog.h>

module wb.session;

import wb.doc.document;
import wb.editor;
import wb.io.blob;
import wb.io.file;
import wb.io.serializer;

namespace wb
{
namespace
{
constexpr uint64_t AUTOSAVE_INTERVAL_NS = 30'000'000'000ull;
constexpr size_t BIG_ASSET_BYTES = 2u << 20;
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

IoResult Session::beginSave(const std::filesystem::path& p_Path)
{
	if (m_Save)
		return IoResult::failure("The board is still being saved.");
	waitForAutosave(); // two writers must not meet

	// Serialized here (the document belongs to this thread). Everything big is written by the worker, read from where it
	// is: memory, the file it was imported from, the board file it was last saved in, or the copy that was downloaded.
	SaveJob l_Job;
	l_Job.path = p_Path;
	l_Job.state = m_Editor.history().stateId();
	l_Job.progress = std::make_shared<WriteProgress>();
	l_Job.placed = std::make_shared<std::vector<PlacedAsset>>();
	SplitBoard l_Board = m_Editor.saveBoardSplit({}, SplitOptions{ .bigAssetBytes = BIG_ASSET_BYTES, .deferChecksums = true });
	l_Job.bigBytes = l_Board.bigBytes;
	l_Job.done = std::async(std::launch::async, [l_Path = p_Path, l_Progress = l_Job.progress, l_Placed = l_Job.placed, l_Data = std::move(l_Board)]() mutable
	{
		finishChecksums(l_Data);
		return writeSplitBoardAtomic(l_Path, l_Data, l_Placed.get(), l_Progress.get());
	});
	m_Save = std::move(l_Job);
	return {};
}

double Session::saveProgress() const
{
	if (!m_Save || m_Save->bigBytes == 0)
		return 0.0;
	return std::min(1.0, static_cast<double>(m_Save->progress->written.load()) / static_cast<double>(m_Save->bigBytes));
}

void Session::cancelSave()
{
	if (m_Save)
		m_Save->progress->cancel.store(true);
}

std::optional<SaveOutcome> Session::pollSave()
{
	if (m_Outcome)
		return std::exchange(m_Outcome, std::nullopt);
	if (!m_Save || m_Save->done.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return std::nullopt;
	return finishSave();
}

void Session::waitForSave()
{
	if (!m_Save)
		return;
	m_Save->done.wait();
	m_Outcome = finishSave();
}

std::optional<SaveOutcome> Session::finishSave()
{
	SaveJob l_Job = std::move(*m_Save);
	m_Save.reset();
	const IoResult l_Result = l_Job.done.get();
	SaveOutcome l_Outcome{ .ok = l_Result.ok, .error = l_Result.error, .path = l_Job.path };
	if (!l_Result.ok)
		return l_Outcome;

	m_Path = l_Job.path;
	m_SavedState = l_Job.state; // edits made while it was being written are not in the file
	m_AutosavedState = m_SavedState;
	discardRecovery();

	// The big files now also sit in the board file: let go of the memory, the original files and the downloads
	std::string l_Error;
	if (const std::shared_ptr<FileSource> l_File = FileSource::open(l_Job.path, l_Error))
	{
		Document& l_Document = m_Editor.document();
		for (const PlacedAsset& l_Placed : *l_Job.placed)
		{
			const ImageAsset* l_Asset = l_Document.findAsset(l_Placed.id);
			if (l_Asset != nullptr && (l_Asset->stream || l_Asset->size() >= LARGE_ASSET_BYTES))
				l_Document.rebindAsset(l_Placed.id, std::make_shared<RangeSource>(l_File, l_Placed.offset, l_Placed.size));
		}
	}
	else
	{
		spdlog::warn("Saved, but could not reopen the board file to read pictures from it: {}", l_Error);
	}
	return l_Outcome;
}

IoResult Session::saveTo(const std::filesystem::path& p_Path)
{
	waitForSave();
	m_Outcome.reset();
	if (const IoResult l_Started = beginSave(p_Path); !l_Started.ok)
		return l_Started;
	waitForSave();
	const std::optional<SaveOutcome> l_Outcome = std::exchange(m_Outcome, std::nullopt);
	return l_Outcome && l_Outcome->ok ? IoResult{} : IoResult::failure(l_Outcome ? l_Outcome->error : std::string("The board could not be saved."));
}

IoResult Session::open(const std::filesystem::path& p_Path)
{
	waitForAutosave(); // the write may still be reading assets of the board that is about to be replaced
	std::string l_OpenError;
	const std::shared_ptr<FileSource> l_File = FileSource::open(p_Path, l_OpenError);
	if (!l_File)
		return IoResult::failure(l_OpenError);

	BoardMeta l_Meta;
	const LoadResult l_Loaded = m_Editor.loadBoardFile(l_File, l_Meta);
	if (!l_Loaded.ok)
		return IoResult::failure(pathToUtf8(p_Path.filename()) + ": " + l_Loaded.error);
	if (l_Loaded.skippedObjects > 0)
		m_Warning = std::to_string(l_Loaded.skippedObjects) + " object(s) from a newer version of Inkwell were left out. Saving will drop them.";

	m_Path = p_Path;
	m_SavedState = m_Editor.history().stateId();
	m_AutosavedState = m_SavedState;
	discardRecovery();
	return {};
}

IoResult Session::adoptBoard(const std::span<const uint8_t> p_Bytes)
{
	waitForAutosave();
	BoardMeta l_Meta;
	const LoadResult l_Loaded = m_Editor.loadBoard(p_Bytes, l_Meta, true);
	if (!l_Loaded.ok)
		return IoResult::failure(l_Loaded.error);
	m_Path.reset();
	m_SavedState = m_Editor.history().stateId();
	m_AutosavedState = m_SavedState;
	discardRecovery();
	return {};
}

void Session::newBoard()
{
	waitForAutosave();
	m_Editor.newBoard();
	m_Path.reset();
	m_SavedState = m_Editor.history().stateId();
	m_AutosavedState = m_SavedState;
	discardRecovery();
}

void Session::tick(const uint64_t p_NowNs)
{
	reapAutosave();
	if (m_StorageDir.empty() || m_AutosaveJob.valid() || m_Save)
		return;
	const uint64_t l_State = m_Editor.history().stateId();
	if (l_State == m_SavedState || l_State == m_AutosavedState)
		return; // nothing new to protect
	if (m_Editor.isBusy() || p_NowNs - m_LastAutosaveNs < AUTOSAVE_INTERVAL_NS)
		return;

	// Serialize here (the document belongs to this thread), write on a worker so large boards do not stall drawing.
	// Big files (videos, large pictures) are not copied: the worker reads them from the document's assets, which never
	// change once added. Nothing may clear the document while the job runs (waitForAutosave guards that).
	SplitBoard l_Board = m_Editor.saveBoardSplit(m_Path ? pathToUtf8(*m_Path) : std::string{}, SplitOptions{ .bigAssetBytes = BIG_ASSET_BYTES, .referenceFiles = true, .noReferencesTo = recoveryPath(), .deferChecksums = true });
	m_AutosavedState = l_State;
	m_LastAutosaveNs = p_NowNs;
	m_AutosaveJob = std::async(std::launch::async, [l_Path = recoveryPath(), l_Data = std::move(l_Board)]() mutable
	{
		finishChecksums(l_Data);
		return writeSplitBoardAtomic(l_Path, l_Data);
	});
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
	waitForSave(); // anything that waits for the writers also wants the board file finished
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

	std::string l_OpenError;
	const std::shared_ptr<FileSource> l_File = FileSource::open(recoveryPath(), l_OpenError);
	Document l_Scratch;
	BoardMeta l_Meta;
	LoadResult l_Checked;
	if (l_File)
		l_Checked = loadBoardFile(l_File, l_Scratch, l_Meta);
	if (!l_File || !l_Checked.ok)
	{
		// Unusable: keep it for inspection but never offer it again
		std::filesystem::rename(recoveryPath(), m_StorageDir / "recovery.damaged.wbrd", l_Error);
		spdlog::warn("Ignoring a damaged recovery file");
		return std::nullopt;
	}

	RecoveryInfo l_Info;
	l_Info.missingFiles = std::move(l_Checked.missingFiles);
	l_Info.sourcePath = std::move(l_Meta.sourcePath);
	l_Info.objectCount = l_Scratch.size();
	const auto l_Written = std::filesystem::last_write_time(recoveryPath(), l_Error);
	if (!l_Error)
		l_Info.ageSeconds = std::chrono::duration_cast<std::chrono::seconds>(std::filesystem::file_time_type::clock::now() - l_Written).count();
	return l_Info;
}

IoResult Session::recover()
{
	waitForAutosave();
	std::string l_OpenError;
	const std::shared_ptr<FileSource> l_File = FileSource::open(recoveryPath(), l_OpenError);
	if (!l_File)
		return IoResult::failure(l_OpenError);
	BoardMeta l_Meta;
	const LoadResult l_Loaded = m_Editor.loadBoardFile(l_File, l_Meta);
	if (!l_Loaded.ok)
		return IoResult::failure(l_Loaded.error);
	if (!l_Loaded.missingFiles.empty())
	{
		m_Warning = "These pictures or videos could not be restored because their files have moved or changed:";
		for (const std::string& l_Name : l_Loaded.missingFiles)
			m_Warning += "\n  " + l_Name;
	}

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
