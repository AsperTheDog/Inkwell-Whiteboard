module;
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "monocypher.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <sys/random.h>
#endif

module wb.net.crypto;

namespace wb::net::crypto
{
namespace
{
// 19 MiB and two passes (the OWASP minimum for Argon2id): about a tenth of a second
constexpr uint32_t ARGON_BLOCKS = 19456;
constexpr uint32_t ARGON_PASSES = 2;

// Domain separation: a hash made for one purpose can never be mistaken for another
constexpr std::string_view LABEL_KEYS = "inkwell/2 session keys";
constexpr std::string_view LABEL_TRANSCRIPT = "inkwell/2 transcript";
constexpr std::string_view LABEL_PROOF_HOST = "inkwell/2 proof from host";
constexpr std::string_view LABEL_PROOF_CLIENT = "inkwell/2 proof from client";

void append(std::vector<uint8_t>& p_Out, const std::string_view p_Part)
{
	p_Out.insert(p_Out.end(), reinterpret_cast<const uint8_t*>(p_Part.data()), reinterpret_cast<const uint8_t*>(p_Part.data()) + p_Part.size());
}

void append(std::vector<uint8_t>& p_Out, const std::array<uint8_t, 32>& p_Part)
{
	p_Out.insert(p_Out.end(), p_Part.begin(), p_Part.end());
}

std::array<uint8_t, 24> nonceOf(const uint64_t p_Counter)
{
	std::array<uint8_t, 24> l_Nonce{};
	for (size_t i = 0; i < 8; ++i)
		l_Nonce[i] = static_cast<uint8_t>(p_Counter >> (8 * i));
	return l_Nonce;
}
} // namespace

void randomBytes(const std::span<uint8_t> p_Out)
{
#ifdef _WIN32
	if (BCryptGenRandom(nullptr, p_Out.data(), static_cast<ULONG>(p_Out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
		throw std::runtime_error("No secure random number generator");
#else
	size_t l_Done = 0;
	while (l_Done < p_Out.size())
	{
		const ssize_t l_Got = getrandom(p_Out.data() + l_Done, p_Out.size() - l_Done, 0);
		if (l_Got <= 0)
			throw std::runtime_error("No secure random number generator");
		l_Done += static_cast<size_t>(l_Got);
	}
#endif
}

void wipe(const std::span<uint8_t> p_Bytes)
{
	crypto_wipe(p_Bytes.data(), p_Bytes.size());
}

bool equal(const Digest& p_A, const Digest& p_B)
{
	return crypto_verify32(p_A.data(), p_B.data()) == 0;
}

KeyPair makeKeyPair()
{
	KeyPair l_Pair;
	randomBytes(l_Pair.secret);
	crypto_x25519_public_key(l_Pair.pub.data(), l_Pair.secret.data());
	return l_Pair;
}

bool deriveSessionKeys(const KeyPair& p_Own, const Key& p_PeerPublic, const bool p_IsServer, SessionKeys& p_Out)
{
	Key l_Shared{};
	crypto_x25519(l_Shared.data(), p_Own.secret.data(), p_PeerPublic.data());
	// A small-order peer key gives an all-zero secret that anybody could compute
	const Key l_Zero{};
	if (crypto_verify32(l_Shared.data(), l_Zero.data()) == 0)
		return false;

	const Key& l_ServerPublic = p_IsServer ? p_Own.pub : p_PeerPublic;
	const Key& l_ClientPublic = p_IsServer ? p_PeerPublic : p_Own.pub;

	std::vector<uint8_t> l_Input;
	append(l_Input, LABEL_KEYS);
	append(l_Input, l_ServerPublic);
	append(l_Input, l_ClientPublic);
	std::array<uint8_t, 64> l_Both{};
	crypto_blake2b_keyed(l_Both.data(), l_Both.size(), l_Shared.data(), l_Shared.size(), l_Input.data(), l_Input.size());
	const uint8_t* l_ServerToClient = l_Both.data();
	const uint8_t* l_ClientToServer = l_Both.data() + KEY_BYTES;
	std::memcpy(p_Out.send.data(), p_IsServer ? l_ServerToClient : l_ClientToServer, KEY_BYTES);
	std::memcpy(p_Out.receive.data(), p_IsServer ? l_ClientToServer : l_ServerToClient, KEY_BYTES);

	// The transcript is public data only, so it can be shown and compared freely
	std::vector<uint8_t> l_Public;
	append(l_Public, LABEL_TRANSCRIPT);
	append(l_Public, l_ServerPublic);
	append(l_Public, l_ClientPublic);
	crypto_blake2b(p_Out.transcript.data(), p_Out.transcript.size(), l_Public.data(), l_Public.size());

	crypto_wipe(l_Shared.data(), l_Shared.size());
	crypto_wipe(l_Both.data(), l_Both.size());
	return true;
}

void Aead::seal(const std::span<const uint8_t> p_Ad, const std::span<uint8_t> p_Data, Mac& p_Mac)
{
	const std::array<uint8_t, 24> l_Nonce = nonceOf(m_Counter++);
	crypto_aead_lock(p_Data.data(), p_Mac.data(), m_Key.data(), l_Nonce.data(), p_Ad.data(), p_Ad.size(), p_Data.data(), p_Data.size());
}

bool Aead::open(const std::span<const uint8_t> p_Ad, const std::span<uint8_t> p_Data, const Mac& p_Mac)
{
	const std::array<uint8_t, 24> l_Nonce = nonceOf(m_Counter++);
	return crypto_aead_unlock(p_Data.data(), p_Mac.data(), m_Key.data(), l_Nonce.data(), p_Ad.data(), p_Ad.size(), p_Data.data(), p_Data.size()) == 0;
}

PasswordKey::PasswordKey(const std::string& p_Password, const Salt& p_Salt)
{
	std::vector<uint8_t> l_Work(size_t{ ARGON_BLOCKS } * 1024);
	const crypto_argon2_config l_Config{ .algorithm = CRYPTO_ARGON2_ID, .nb_blocks = ARGON_BLOCKS, .nb_passes = ARGON_PASSES, .nb_lanes = 1 };
	const crypto_argon2_inputs l_Inputs{ .pass = reinterpret_cast<const uint8_t*>(p_Password.data()), .salt = p_Salt.data(), .pass_size = static_cast<uint32_t>(p_Password.size()), .salt_size = static_cast<uint32_t>(p_Salt.size()) };
	crypto_argon2(m_Key.data(), static_cast<uint32_t>(m_Key.size()), l_Work.data(), l_Config, l_Inputs, crypto_argon2_no_extras);
	crypto_wipe(l_Work.data(), l_Work.size());
}

Digest PasswordKey::proof(const bool p_FromHost, const Digest& p_Transcript) const
{
	std::vector<uint8_t> l_Input;
	append(l_Input, p_FromHost ? LABEL_PROOF_HOST : LABEL_PROOF_CLIENT);
	append(l_Input, p_Transcript);
	Digest l_Proof{};
	crypto_blake2b_keyed(l_Proof.data(), l_Proof.size(), m_Key.data(), m_Key.size(), l_Input.data(), l_Input.size());
	return l_Proof;
}
} // namespace wb::net::crypto
