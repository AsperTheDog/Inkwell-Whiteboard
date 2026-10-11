// The cryptography of an encrypted session, a thin layer over Monocypher so the rest of the code never touches raw primitives.
//
//   channel   Both ends make a throwaway X25519 key pair per connection and derive two keys from the shared secret (one
//             per direction). Frames are sealed with ChaCha20-Poly1305; a counter is the nonce, so a frame that is
//             altered, replayed, dropped from the middle or reordered fails to open.
//   login     The key exchange alone does not tell who is on the other end, so the password is proven inside the channel:
//             both sides compute a keyed hash of the whole key exchange with a key stretched from the password (Argon2id).
//             Somebody sitting in the middle has a different key exchange on each side, so their proofs never match.
module;
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

export module wb.net.crypto;

export namespace wb::net::crypto
{
inline constexpr size_t KEY_BYTES = 32;
inline constexpr size_t MAC_BYTES = 16;
inline constexpr size_t SALT_BYTES = 16;

using Key = std::array<uint8_t, KEY_BYTES>;
using Digest = std::array<uint8_t, 32>;
using Salt = std::array<uint8_t, SALT_BYTES>;
using Mac = std::array<uint8_t, MAC_BYTES>;

// Bytes from the operating system's secure random generator. Throws std::runtime_error if there is none.
void randomBytes(std::span<uint8_t> p_Out);
// Overwrites memory in a way the compiler may not drop
void wipe(std::span<uint8_t> p_Bytes);
// Compares in constant time
[[nodiscard]] bool equal(const Digest& p_A, const Digest& p_B);

struct KeyPair
{
	Key secret{};
	Key pub{};
	KeyPair() = default;
	KeyPair(const KeyPair&) = default;
	KeyPair& operator=(const KeyPair&) = default;
	~KeyPair() { wipe(secret); }
};
[[nodiscard]] KeyPair makeKeyPair();

// What both ends hold once the key exchange is done
struct SessionKeys
{
	Key send{};
	Key receive{};
	Digest transcript{}; // identical on both ends; commits to both public keys
	SessionKeys() = default;
	SessionKeys(const SessionKeys&) = default;
	SessionKeys& operator=(const SessionKeys&) = default;
	~SessionKeys()
	{
		wipe(send);
		wipe(receive);
	}
};
// False when the peer's public key is not a usable one (all zero shared secret)
[[nodiscard]] bool deriveSessionKeys(const KeyPair& p_Own, const Key& p_PeerPublic, bool p_IsServer, SessionKeys& p_Out);

// Seals and opens the frames of one direction. The nonce is a counter that moves on with every frame, so both ends must
// handle the frames in the same order.
class Aead
{
public:
	Aead() = default;
	explicit Aead(const Key& p_Key) : m_Key(p_Key) {}
	Aead(const Aead&) = default;
	Aead& operator=(const Aead&) = default;
	~Aead() { wipe(m_Key); }

	// Encrypts p_Data in place and writes the tag; p_Ad is authenticated but not encrypted
	void seal(std::span<const uint8_t> p_Ad, std::span<uint8_t> p_Data, Mac& p_Mac);
	// Decrypts p_Data in place; false (and the data is garbage) if anything was altered
	[[nodiscard]] bool open(std::span<const uint8_t> p_Ad, std::span<uint8_t> p_Data, const Mac& p_Mac);

private:
	Key m_Key{};
	uint64_t m_Counter = 0;
};

// A key stretched from the session password; slow on purpose. Done once per session.
class PasswordKey
{
public:
	PasswordKey() = default;
	PasswordKey(const std::string& p_Password, const Salt& p_Salt);
	PasswordKey(const PasswordKey&) = default;
	PasswordKey& operator=(const PasswordKey&) = default;
	~PasswordKey() { wipe(m_Key); }

	// What one side shows the other: a keyed hash of the key exchange. The two sides use different labels, so one
	// side's proof cannot be echoed back as the other's.
	[[nodiscard]] Digest proof(bool p_FromHost, const Digest& p_Transcript) const;

private:
	Key m_Key{};
};
} // namespace wb::net::crypto
