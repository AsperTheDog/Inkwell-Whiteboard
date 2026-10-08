// File helpers: UTF-8 paths, whole-file reads and crash-safe writes.
module;
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module wb.io.file;

export namespace wb
{
struct IoResult
{
	bool ok = true;
	std::string error; // human readable, set when !ok

	[[nodiscard]] static IoResult failure(std::string p_Message) { return IoResult{ .ok = false, .error = std::move(p_Message) }; }
};

[[nodiscard]] std::filesystem::path pathFromUtf8(std::string_view p_Utf8);
[[nodiscard]] std::string pathToUtf8(const std::filesystem::path& p_Path);

// Writes p_Bytes to a temporary sibling file and renames it over p_Path, so a crash or a full disk never leaves a
// half-written file behind. Missing parent directories are created.
[[nodiscard]] IoResult writeFileAtomic(const std::filesystem::path& p_Path, std::span<const uint8_t> p_Bytes);
// Reads the whole file; refuses files larger than p_MaxBytes
[[nodiscard]] IoResult readFile(const std::filesystem::path& p_Path, std::vector<uint8_t>& p_Bytes, uint64_t p_MaxBytes = 4ull << 30);
// Deletes the file if it exists; never throws
void removeFileQuiet(const std::filesystem::path& p_Path);
} // namespace wb
