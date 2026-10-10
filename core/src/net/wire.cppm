// Byte-level encoding for the collaboration protocol: little endian, bounds checked on read.
module;
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module wb.net.wire;

import wb.math;

export namespace wb::net
{
class ByteWriter
{
public:
	void u8(const uint8_t p_Value) { m_Bytes.push_back(p_Value); }
	void u16(const uint16_t p_Value) { raw(&p_Value, sizeof(p_Value)); }
	void u32(const uint32_t p_Value) { raw(&p_Value, sizeof(p_Value)); }
	void u64(const uint64_t p_Value) { raw(&p_Value, sizeof(p_Value)); }
	void f32(const float p_Value) { raw(&p_Value, sizeof(p_Value)); }
	void f64(const double p_Value) { raw(&p_Value, sizeof(p_Value)); }
	void boolean(const bool p_Value) { u8(p_Value ? 1 : 0); }
	void string(const std::string_view p_Text)
	{
		u32(static_cast<uint32_t>(p_Text.size()));
		raw(p_Text.data(), p_Text.size());
	}
	// Length-prefixed block of bytes
	void blob(const std::span<const uint8_t> p_Bytes)
	{
		u32(static_cast<uint32_t>(p_Bytes.size()));
		raw(p_Bytes.data(), p_Bytes.size());
	}
	void transform(const Affine2& p_Transform)
	{
		f64(p_Transform.linear[0][0]);
		f64(p_Transform.linear[0][1]);
		f64(p_Transform.linear[1][0]);
		f64(p_Transform.linear[1][1]);
		f64(p_Transform.translation.x);
		f64(p_Transform.translation.y);
	}
	void raw(const void* p_Data, const size_t p_Size)
	{
		const auto* l_Bytes = static_cast<const uint8_t*>(p_Data);
		m_Bytes.insert(m_Bytes.end(), l_Bytes, l_Bytes + p_Size);
	}

	[[nodiscard]] size_t size() const { return m_Bytes.size(); }
	[[nodiscard]] std::span<const uint8_t> bytes() const { return m_Bytes; }
	std::vector<uint8_t> take() { return std::move(m_Bytes); }

private:
	std::vector<uint8_t> m_Bytes;
};

// Once a read runs past the end, ok() stays false and every later read returns zero
class ByteReader
{
public:
	explicit ByteReader(const std::span<const uint8_t> p_Data) : m_Data(p_Data) {}

	[[nodiscard]] bool ok() const { return m_Ok; }
	[[nodiscard]] size_t remaining() const { return m_Data.size() - m_Pos; }
	void fail() { m_Ok = false; }

	uint8_t u8() { return read<uint8_t>(); }
	uint16_t u16() { return read<uint16_t>(); }
	uint32_t u32() { return read<uint32_t>(); }
	uint64_t u64() { return read<uint64_t>(); }
	float f32() { return read<float>(); }
	double f64() { return read<double>(); }
	bool boolean() { return u8() != 0; }

	std::span<const uint8_t> take(const size_t p_Size)
	{
		if (!m_Ok || p_Size > remaining())
		{
			m_Ok = false;
			return {};
		}
		const std::span<const uint8_t> l_Result = m_Data.subspan(m_Pos, p_Size);
		m_Pos += p_Size;
		return l_Result;
	}

	std::string string(const size_t p_MaxBytes)
	{
		const uint32_t l_Size = u32();
		if (l_Size > p_MaxBytes)
		{
			m_Ok = false;
			return {};
		}
		const std::span<const uint8_t> l_Bytes = take(l_Size);
		return std::string(reinterpret_cast<const char*>(l_Bytes.data()), l_Bytes.size());
	}

	std::span<const uint8_t> blob(const size_t p_MaxBytes)
	{
		const uint32_t l_Size = u32();
		if (l_Size > p_MaxBytes)
		{
			m_Ok = false;
			return {};
		}
		return take(l_Size);
	}

	// False (and the reader failed) for non-finite values
	bool transform(Affine2& p_Transform)
	{
		double l_Values[6];
		for (double& l_Value : l_Values)
			l_Value = f64();
		if (!m_Ok)
			return false;
		for (const double l_Value : l_Values)
		{
			if (!std::isfinite(l_Value))
			{
				m_Ok = false;
				return false;
			}
		}
		p_Transform.linear[0][0] = l_Values[0];
		p_Transform.linear[0][1] = l_Values[1];
		p_Transform.linear[1][0] = l_Values[2];
		p_Transform.linear[1][1] = l_Values[3];
		p_Transform.translation = DVec2{ l_Values[4], l_Values[5] };
		return true;
	}

private:
	template <typename T>
	T read()
	{
		T l_Value{};
		if (!m_Ok || sizeof(T) > remaining())
		{
			m_Ok = false;
			return l_Value;
		}
		std::memcpy(&l_Value, m_Data.data() + m_Pos, sizeof(T));
		m_Pos += sizeof(T);
		return l_Value;
	}

	std::span<const uint8_t> m_Data;
	size_t m_Pos = 0;
	bool m_Ok = true;
};
} // namespace wb::net
