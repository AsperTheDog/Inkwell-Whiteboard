// Free-list allocator over an abstract range [0, capacity), used to sub-allocate GPU buffers.
// Freed neighbours coalesce. Allocation is first fit.
module;
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

export module wb.util.range_allocator;

export namespace wb
{
class RangeAllocator
{
public:
	explicit RangeAllocator(uint64_t p_Capacity = 0);

	[[nodiscard]] std::optional<uint64_t> allocate(uint64_t p_Count);
	void free(uint64_t p_Offset, uint64_t p_Count);
	// Extends the range; the new tail is free (and merges with a free block at the old end)
	void grow(uint64_t p_NewCapacity);
	void reset(uint64_t p_Capacity);

	[[nodiscard]] uint64_t capacity() const { return m_Capacity; }
	[[nodiscard]] uint64_t used() const { return m_Used; }
	[[nodiscard]] uint64_t largestFreeBlock() const;
	[[nodiscard]] size_t freeBlockCount() const { return m_Free.size(); }

private:
	std::map<uint64_t, uint64_t> m_Free; // offset -> size
	uint64_t m_Capacity = 0;
	uint64_t m_Used = 0;
};
} // namespace wb
