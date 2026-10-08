// The open board as a file: its path, whether it has unsaved changes, and crash recovery.
//
// Dirty tracking compares History::stateId() with the id at the last save, so undoing back to the saved state is
// clean again. While the board is dirty an autosave copy is written next to the settings (recovery.wbrd) when the
// editor is idle; it is removed on save and on a clean exit, so finding it at startup means the last run ended
// without saving.
module;
#include <cstdint>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <utility>

export module wb.session;

import wb.editor;
import wb.io.file;

export namespace wb
{
struct RecoveryInfo
{
	std::string sourcePath; // file the unsaved board was opened from; empty for an untitled board
	uint64_t objectCount = 0;
	int64_t ageSeconds = 0;
};

class Session
{
public:
	explicit Session(Editor& p_Editor) : m_Editor(p_Editor) {}
	~Session();
	Session(const Session&) = delete;
	Session& operator=(const Session&) = delete;

	// Enables autosave and recovery, storing them in p_Directory
	void setStorageDirectory(std::filesystem::path p_Directory);

	[[nodiscard]] bool dirty() const;
	[[nodiscard]] const std::optional<std::filesystem::path>& path() const { return m_Path; }
	[[nodiscard]] std::string displayName() const; // file name, or "Untitled"
	// A note about the last load (e.g. objects from a newer version were left out); cleared when read
	[[nodiscard]] std::string takeWarning() { return std::exchange(m_Warning, {}); }

	[[nodiscard]] IoResult saveTo(const std::filesystem::path& p_Path);
	[[nodiscard]] IoResult open(const std::filesystem::path& p_Path);
	void newBoard();

	// Call regularly (about once a second is plenty): writes the autosave copy when due
	void tick(uint64_t p_NowNs);
	// Clean exit: nothing left to recover
	void finish();

	[[nodiscard]] std::optional<RecoveryInfo> peekRecovery();
	[[nodiscard]] IoResult recover();
	void discardRecovery();

private:
	[[nodiscard]] std::filesystem::path recoveryPath() const;
	void reapAutosave();
	void waitForAutosave();

	Editor& m_Editor;
	std::filesystem::path m_StorageDir;
	std::optional<std::filesystem::path> m_Path;
	std::string m_Warning;
	uint64_t m_SavedState = 0;
	uint64_t m_AutosavedState = 0;
	uint64_t m_LastAutosaveNs = 0;
	std::future<IoResult> m_AutosaveJob;
};
} // namespace wb
