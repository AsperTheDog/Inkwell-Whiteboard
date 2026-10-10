// SHA-256, used to prove knowledge of a session password without sending it.
module;
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

export module wb.util.sha256;

export namespace wb
{
using Sha256Digest = std::array<uint8_t, 32>;

[[nodiscard]] Sha256Digest sha256(std::span<const uint8_t> p_Data)
{
	static constexpr std::array<uint32_t, 64> K{
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
	};
	std::array<uint32_t, 8> l_State{ 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };

	const auto l_Block = [&](const uint8_t* p_Bytes)
	{
		std::array<uint32_t, 64> l_W{};
		for (size_t i = 0; i < 16; ++i)
			l_W[i] = (uint32_t(p_Bytes[i * 4]) << 24) | (uint32_t(p_Bytes[i * 4 + 1]) << 16) | (uint32_t(p_Bytes[i * 4 + 2]) << 8) | uint32_t(p_Bytes[i * 4 + 3]);
		for (size_t i = 16; i < 64; ++i)
		{
			const uint32_t l_S0 = std::rotr(l_W[i - 15], 7) ^ std::rotr(l_W[i - 15], 18) ^ (l_W[i - 15] >> 3);
			const uint32_t l_S1 = std::rotr(l_W[i - 2], 17) ^ std::rotr(l_W[i - 2], 19) ^ (l_W[i - 2] >> 10);
			l_W[i] = l_W[i - 16] + l_S0 + l_W[i - 7] + l_S1;
		}
		uint32_t a = l_State[0], b = l_State[1], c = l_State[2], d = l_State[3], e = l_State[4], f = l_State[5], g = l_State[6], h = l_State[7];
		for (size_t i = 0; i < 64; ++i)
		{
			const uint32_t l_T1 = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + l_W[i];
			const uint32_t l_T2 = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
			h = g;
			g = f;
			f = e;
			e = d + l_T1;
			d = c;
			c = b;
			b = a;
			a = l_T1 + l_T2;
		}
		l_State[0] += a;
		l_State[1] += b;
		l_State[2] += c;
		l_State[3] += d;
		l_State[4] += e;
		l_State[5] += f;
		l_State[6] += g;
		l_State[7] += h;
	};

	size_t l_Done = 0;
	while (p_Data.size() - l_Done >= 64)
	{
		l_Block(p_Data.data() + l_Done);
		l_Done += 64;
	}
	std::array<uint8_t, 128> l_Tail{};
	const size_t l_Rest = p_Data.size() - l_Done;
	for (size_t i = 0; i < l_Rest; ++i)
		l_Tail[i] = p_Data[l_Done + i];
	l_Tail[l_Rest] = 0x80;
	const size_t l_TailSize = l_Rest < 56 ? 64 : 128;
	const uint64_t l_Bits = static_cast<uint64_t>(p_Data.size()) * 8;
	for (size_t i = 0; i < 8; ++i)
		l_Tail[l_TailSize - 1 - i] = static_cast<uint8_t>(l_Bits >> (8 * i));
	l_Block(l_Tail.data());
	if (l_TailSize == 128)
		l_Block(l_Tail.data() + 64);

	Sha256Digest l_Out{};
	for (size_t i = 0; i < 8; ++i)
	{
		l_Out[i * 4] = static_cast<uint8_t>(l_State[i] >> 24);
		l_Out[i * 4 + 1] = static_cast<uint8_t>(l_State[i] >> 16);
		l_Out[i * 4 + 2] = static_cast<uint8_t>(l_State[i] >> 8);
		l_Out[i * 4 + 3] = static_cast<uint8_t>(l_State[i]);
	}
	return l_Out;
}
} // namespace wb
