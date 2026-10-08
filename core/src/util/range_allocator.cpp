module;
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>

module wb.util.range_allocator;

namespace wb
{
RangeAllocator::RangeAllocator(const uint64_t p_Capacity)
{
	reset(p_Capacity);
}

void RangeAllocator::reset(const uint64_t p_Capacity)
{
	m_Free.clear();
	m_Capacity = p_Capacity;
	m_Used = 0;
	if (p_Capacity > 0)
		m_Free.emplace(0, p_Capacity);
}

std::optional<uint64_t> RangeAllocator::allocate(const uint64_t p_Count)
{
	if (p_Count == 0)
		return std::nullopt;
	for (auto l_It = m_Free.begin(); l_It != m_Free.end(); ++l_It)
	{
		if (l_It->second < p_Count)
			continue;
		const uint64_t l_Offset = l_It->first;
		const uint64_t l_Remaining = l_It->second - p_Count;
		m_Free.erase(l_It);
		if (l_Remaining > 0)
			m_Free.emplace(l_Offset + p_Count, l_Remaining);
		m_Used += p_Count;
		return l_Offset;
	}
	return std::nullopt;
}

void RangeAllocator::free(const uint64_t p_Offset, const uint64_t p_Count)
{
	if (p_Count == 0)
		return;
	assert(p_Offset + p_Count <= m_Capacity);
	m_Used -= std::min(m_Used, p_Count);

	uint64_t l_Offset = p_Offset;
	uint64_t l_Size = p_Count;

	// Merge with the following block
	const auto l_Next = m_Free.lower_bound(p_Offset);
	assert(l_Next == m_Free.end() || l_Next->first >= p_Offset + p_Count);
	if (l_Next != m_Free.end() && l_Next->first == p_Offset + p_Count)
	{
		l_Size += l_Next->second;
		m_Free.erase(l_Next);
	}

	// Merge with the preceding block
	auto l_After = m_Free.lower_bound(p_Offset);
	if (l_After != m_Free.begin())
	{
		const auto l_Prev = std::prev(l_After);
		assert(l_Prev->first + l_Prev->second <= p_Offset);
		if (l_Prev->first + l_Prev->second == p_Offset)
		{
			l_Offset = l_Prev->first;
			l_Size += l_Prev->second;
			m_Free.erase(l_Prev);
		}
	}
	m_Free.emplace(l_Offset, l_Size);
}

void RangeAllocator::grow(const uint64_t p_NewCapacity)
{
	if (p_NewCapacity <= m_Capacity)
		return;
	const uint64_t l_OldCapacity = m_Capacity;
	m_Capacity = p_NewCapacity;
	m_Used += p_NewCapacity - l_OldCapacity; // free() subtracts it again
	free(l_OldCapacity, p_NewCapacity - l_OldCapacity);
}

uint64_t RangeAllocator::largestFreeBlock() const
{
	uint64_t l_Largest = 0;
	for (const auto& [l_Offset, l_Size] : m_Free)
		l_Largest = std::max(l_Largest, l_Size);
	return l_Largest;
}
} // namespace wb
