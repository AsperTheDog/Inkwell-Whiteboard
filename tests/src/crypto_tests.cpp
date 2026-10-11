#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <gtest/gtest.h>

import wb.net.connection;
import wb.net.crypto;
import wb.net.socket;

using namespace wb::net;

namespace
{
std::vector<uint8_t> bytesOf(const std::string& p_Text)
{
	return { p_Text.begin(), p_Text.end() };
}

// Two Connections over loopback and what each one has received so far
struct Pair
{
	std::mutex mutex;
	std::vector<Frame> serverGot;
	std::vector<Frame> clientGot;
	std::string serverClosed;
	std::string clientClosed;
	std::unique_ptr<Connection> server;
	std::unique_ptr<Connection> client;

	void makeServer(Socket p_Socket)
	{
		server = std::make_unique<Connection>(
			std::move(p_Socket), Connection::Role::Server,
			[this](Frame p_Frame)
			{
				const std::lock_guard l_Lock(mutex);
				serverGot.push_back(std::move(p_Frame));
			},
			[this](const std::string& p_Why)
			{
				const std::lock_guard l_Lock(mutex);
				serverClosed = p_Why;
			});
		server->setMaxFrame(1u << 20);
	}
	void makeClient(Socket p_Socket)
	{
		client = std::make_unique<Connection>(
			std::move(p_Socket), Connection::Role::Client,
			[this](Frame p_Frame)
			{
				const std::lock_guard l_Lock(mutex);
				clientGot.push_back(std::move(p_Frame));
			},
			[this](const std::string& p_Why)
			{
				const std::lock_guard l_Lock(mutex);
				clientClosed = p_Why;
			});
		client->setMaxFrame(1u << 20);
	}
	template <typename Check>
	bool waitFor(Check p_Check, const int p_Ms = 5000)
	{
		const auto l_End = std::chrono::steady_clock::now() + std::chrono::milliseconds(p_Ms);
		while (std::chrono::steady_clock::now() < l_End)
		{
			{
				const std::lock_guard l_Lock(mutex);
				if (p_Check())
					return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		return false;
	}
};

// A listener and a client socket already connected to it
struct Link
{
	Socket listener;
	Socket client;
	Socket accepted;
	std::string error;

	bool open()
	{
		listener = Socket::listenTcp(0, error);
		if (!listener.valid())
			return false;
		return true;
	}
	bool connect(Socket& p_Listener, Socket& p_Out)
	{
		std::atomic<bool> l_Cancel{ false };
		p_Out = Socket::connectTcp("127.0.0.1", p_Listener.localPort(), 3000, l_Cancel, error);
		return p_Out.valid();
	}
};

// A man in the middle that forwards bytes between a client and a server, and can damage one byte on the way up
struct Relay
{
	Socket towardClient;
	Socket towardServer;
	std::thread up;
	std::thread down;
	size_t corruptAt = 0; // 1-based index of the byte from the client to flip; 0 = none

	void pump(Socket& p_From, Socket& p_To, const size_t p_CorruptAt)
	{
		std::array<uint8_t, 1> l_Byte{};
		size_t l_Count = 0;
		while (p_From.recvAll(l_Byte))
		{
			if (++l_Count == p_CorruptAt)
				l_Byte[0] ^= 0x01;
			if (!p_To.sendAll(l_Byte))
				break;
		}
		p_To.shutdown();
	}
	void start()
	{
		up = std::thread([this] { pump(towardClient, towardServer, corruptAt); });
		down = std::thread([this] { pump(towardServer, towardClient, 0); });
	}
	~Relay()
	{
		towardClient.shutdown();
		towardServer.shutdown();
		if (up.joinable())
			up.join();
		if (down.joinable())
			down.join();
	}
};
} // namespace

TEST(Crypto, BothEndsDeriveMirroredKeys)
{
	const crypto::KeyPair l_Server = crypto::makeKeyPair();
	const crypto::KeyPair l_Client = crypto::makeKeyPair();
	crypto::SessionKeys l_ServerKeys;
	crypto::SessionKeys l_ClientKeys;
	ASSERT_TRUE(crypto::deriveSessionKeys(l_Server, l_Client.pub, true, l_ServerKeys));
	ASSERT_TRUE(crypto::deriveSessionKeys(l_Client, l_Server.pub, false, l_ClientKeys));
	EXPECT_EQ(l_ServerKeys.send, l_ClientKeys.receive);
	EXPECT_EQ(l_ServerKeys.receive, l_ClientKeys.send);
	EXPECT_NE(l_ServerKeys.send, l_ServerKeys.receive);
	EXPECT_EQ(l_ServerKeys.transcript, l_ClientKeys.transcript);

	// Another key pair gives different keys and a different transcript
	const crypto::KeyPair l_Other = crypto::makeKeyPair();
	crypto::SessionKeys l_OtherKeys;
	ASSERT_TRUE(crypto::deriveSessionKeys(l_Other, l_Server.pub, false, l_OtherKeys));
	EXPECT_NE(l_OtherKeys.send, l_ClientKeys.send);
	EXPECT_NE(l_OtherKeys.transcript, l_ClientKeys.transcript);
}

TEST(Crypto, RefusesAPublicKeyThatMakesTheSecretPredictable)
{
	const crypto::KeyPair l_Own = crypto::makeKeyPair();
	crypto::SessionKeys l_Keys;
	EXPECT_FALSE(crypto::deriveSessionKeys(l_Own, crypto::Key{}, true, l_Keys)); // all zero: a point anybody can use
}

TEST(Crypto, SealedFramesOpenOnlyUnaltered)
{
	crypto::Key l_Key{};
	crypto::randomBytes(l_Key);
	const std::array<uint8_t, 4> l_Ad{ 1, 2, 3, 4 };

	crypto::Aead l_Sender(l_Key);
	crypto::Mac l_MacA{};
	crypto::Mac l_MacB{};
	std::vector<uint8_t> l_A = bytesOf("first frame");
	std::vector<uint8_t> l_B = bytesOf("second frame");
	l_Sender.seal(l_Ad, l_A, l_MacA);
	l_Sender.seal(l_Ad, l_B, l_MacB);
	EXPECT_NE(l_A, bytesOf("first frame")); // actually encrypted

	{
		crypto::Aead l_Receiver(l_Key);
		std::vector<uint8_t> l_Copy = l_A;
		ASSERT_TRUE(l_Receiver.open(l_Ad, l_Copy, l_MacA));
		EXPECT_EQ(l_Copy, bytesOf("first frame"));
		l_Copy = l_B;
		ASSERT_TRUE(l_Receiver.open(l_Ad, l_Copy, l_MacB));
		EXPECT_EQ(l_Copy, bytesOf("second frame"));
	}
	{
		// Altered ciphertext
		crypto::Aead l_Receiver(l_Key);
		std::vector<uint8_t> l_Copy = l_A;
		l_Copy[2] ^= 1;
		EXPECT_FALSE(l_Receiver.open(l_Ad, l_Copy, l_MacA));
	}
	{
		// Altered tag
		crypto::Aead l_Receiver(l_Key);
		std::vector<uint8_t> l_Copy = l_A;
		crypto::Mac l_Bad = l_MacA;
		l_Bad[0] ^= 1;
		EXPECT_FALSE(l_Receiver.open(l_Ad, l_Copy, l_Bad));
	}
	{
		// Altered associated data (the frame size)
		crypto::Aead l_Receiver(l_Key);
		std::vector<uint8_t> l_Copy = l_A;
		const std::array<uint8_t, 4> l_OtherAd{ 1, 2, 3, 5 };
		EXPECT_FALSE(l_Receiver.open(l_OtherAd, l_Copy, l_MacA));
	}
	{
		// Reordered or with the first frame dropped: the second frame arrives first
		crypto::Aead l_Receiver(l_Key);
		std::vector<uint8_t> l_Copy = l_B;
		EXPECT_FALSE(l_Receiver.open(l_Ad, l_Copy, l_MacB));
	}
	{
		// Replayed: the same frame twice
		crypto::Aead l_Receiver(l_Key);
		std::vector<uint8_t> l_Copy = l_A;
		ASSERT_TRUE(l_Receiver.open(l_Ad, l_Copy, l_MacA));
		l_Copy = l_A;
		EXPECT_FALSE(l_Receiver.open(l_Ad, l_Copy, l_MacA));
	}
}

TEST(Crypto, PasswordProofsDependOnPasswordSaltAndKeyExchange)
{
	crypto::Salt l_SaltA{};
	crypto::Salt l_SaltB{};
	l_SaltB[0] = 1;
	crypto::Digest l_TranscriptA{};
	crypto::Digest l_TranscriptB{};
	l_TranscriptB[0] = 1;

	const crypto::PasswordKey l_Right("correct horse", l_SaltA);
	const crypto::PasswordKey l_Same("correct horse", l_SaltA);
	const crypto::PasswordKey l_Wrong("correct horsf", l_SaltA);
	const crypto::PasswordKey l_OtherSalt("correct horse", l_SaltB);

	EXPECT_EQ(l_Right.proof(false, l_TranscriptA), l_Same.proof(false, l_TranscriptA));
	EXPECT_NE(l_Right.proof(false, l_TranscriptA), l_Wrong.proof(false, l_TranscriptA));
	EXPECT_NE(l_Right.proof(false, l_TranscriptA), l_OtherSalt.proof(false, l_TranscriptA));
	// A different key exchange (somebody in the middle) gives a different proof
	EXPECT_NE(l_Right.proof(false, l_TranscriptA), l_Right.proof(false, l_TranscriptB));
	// The client proof cannot be echoed back as the host one
	EXPECT_NE(l_Right.proof(false, l_TranscriptA), l_Right.proof(true, l_TranscriptA));
	EXPECT_TRUE(crypto::equal(l_Right.proof(true, l_TranscriptA), l_Same.proof(true, l_TranscriptA)));
}

TEST(SecureConnection, FramesFlowBothWaysInOrderAndTheTranscriptsMatch)
{
	Link l_Link;
	ASSERT_TRUE(l_Link.open()) << l_Link.error;
	ASSERT_TRUE(l_Link.connect(l_Link.listener, l_Link.client)) << l_Link.error;
	l_Link.accepted = l_Link.listener.accept(3000);
	ASSERT_TRUE(l_Link.accepted.valid());

	Pair l_Pair;
	l_Pair.makeServer(std::move(l_Link.accepted));
	l_Pair.makeClient(std::move(l_Link.client));
	l_Pair.server->start();
	l_Pair.client->start();

	// Queued before the key exchange has finished: they wait for it
	for (uint8_t i = 0; i < 50; ++i)
	{
		l_Pair.client->send(7, { i, 1, 2 });
		l_Pair.server->send(8, std::vector<uint8_t>(size_t{ i } * 1000, i));
	}
	l_Pair.client->send(9, {}); // an empty payload
	ASSERT_TRUE(l_Pair.waitFor([&] { return l_Pair.serverGot.size() == 51 && l_Pair.clientGot.size() == 50; }));
	for (uint8_t i = 0; i < 50; ++i)
	{
		EXPECT_EQ(l_Pair.serverGot[i].type, 7);
		EXPECT_EQ(l_Pair.serverGot[i].payload, (std::vector<uint8_t>{ i, 1, 2 }));
		EXPECT_EQ(l_Pair.clientGot[i].type, 8);
		EXPECT_EQ(l_Pair.clientGot[i].payload, std::vector<uint8_t>(size_t{ i } * 1000, i));
	}
	EXPECT_EQ(l_Pair.serverGot[50].type, 9);
	EXPECT_TRUE(l_Pair.serverGot[50].payload.empty());
	EXPECT_EQ(l_Pair.server->transcript(), l_Pair.client->transcript());
	EXPECT_NE(l_Pair.server->transcript(), crypto::Digest{});
}

TEST(SecureConnection, RefusesToBeSpokenToByAnythingElse)
{
	Link l_Link;
	ASSERT_TRUE(l_Link.open()) << l_Link.error;
	ASSERT_TRUE(l_Link.connect(l_Link.listener, l_Link.client)) << l_Link.error;
	l_Link.accepted = l_Link.listener.accept(3000);
	ASSERT_TRUE(l_Link.accepted.valid());

	Pair l_Pair;
	l_Pair.makeServer(std::move(l_Link.accepted));
	l_Pair.server->start();
	const std::string l_Http = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
	ASSERT_TRUE(l_Link.client.sendAll(bytesOf(l_Http)));
	ASSERT_TRUE(l_Pair.waitFor([&] { return !l_Pair.serverClosed.empty(); }));
	EXPECT_EQ(l_Pair.serverClosed, "Not an Inkwell peer");
}

TEST(SecureConnection, ATamperedByteCutsTheConnectionAndAPlainRelayChangesNothing)
{
	// The client sends 5 + 32 bytes of key frame, then 4 bytes of frame size: byte 42 is the first sealed byte
	for (const size_t l_CorruptAt : { size_t{ 0 }, size_t{ 42 } })
	{
		Link l_Link;
		ASSERT_TRUE(l_Link.open()) << l_Link.error;
		Link l_Front; // the relay's own listener
		ASSERT_TRUE(l_Front.open()) << l_Front.error;

		Relay l_Relay;
		l_Relay.corruptAt = l_CorruptAt;
		Socket l_ClientSocket;
		ASSERT_TRUE(l_Front.connect(l_Front.listener, l_ClientSocket)) << l_Front.error;
		l_Relay.towardClient = l_Front.listener.accept(3000);
		ASSERT_TRUE(l_Relay.towardClient.valid());
		ASSERT_TRUE(l_Link.connect(l_Link.listener, l_Relay.towardServer)) << l_Link.error;
		Socket l_ServerSocket = l_Link.listener.accept(3000);
		ASSERT_TRUE(l_ServerSocket.valid());
		l_Relay.start();

		Pair l_Pair;
		l_Pair.makeServer(std::move(l_ServerSocket));
		l_Pair.makeClient(std::move(l_ClientSocket));
		l_Pair.server->start();
		l_Pair.client->start();
		l_Pair.client->send(7, bytesOf("a drawing of a cat"));

		if (l_CorruptAt == 0)
		{
			ASSERT_TRUE(l_Pair.waitFor([&] { return l_Pair.serverGot.size() == 1; }));
			EXPECT_EQ(l_Pair.serverGot[0].payload, bytesOf("a drawing of a cat"));
		}
		else
		{
			ASSERT_TRUE(l_Pair.waitFor([&] { return !l_Pair.serverClosed.empty(); }));
			EXPECT_EQ(l_Pair.serverClosed, "Corrupted or tampered data");
			const std::lock_guard l_Lock(l_Pair.mutex);
			EXPECT_TRUE(l_Pair.serverGot.empty());
		}
		l_Pair.client.reset();
		}
}

TEST(SecureConnection, TrafficOnTheWireIsNotPlaintext)
{
	Link l_Link;
	ASSERT_TRUE(l_Link.open()) << l_Link.error;
	ASSERT_TRUE(l_Link.connect(l_Link.listener, l_Link.client)) << l_Link.error;
	l_Link.accepted = l_Link.listener.accept(3000);
	ASSERT_TRUE(l_Link.accepted.valid());

	// The server is a real Connection; the test plays the client by hand and reads what the server writes
	Pair l_Pair;
	l_Pair.makeServer(std::move(l_Link.accepted));
	l_Pair.server->start();
	const std::string l_Secret = "TOP SECRET BOARD CONTENTS";
	l_Pair.server->send(7, bytesOf(l_Secret));

	std::array<uint8_t, 5 + 32> l_KeyFrame{};
	ASSERT_TRUE(l_Link.client.recvAll(l_KeyFrame));
	EXPECT_EQ(l_KeyFrame[4], 0xF1);
	const crypto::KeyPair l_Own = crypto::makeKeyPair();
	std::array<uint8_t, 5 + 32> l_Answer{};
	l_Answer[0] = 32;
	l_Answer[4] = 0xF2;
	std::copy(l_Own.pub.begin(), l_Own.pub.end(), l_Answer.begin() + 5);
	ASSERT_TRUE(l_Link.client.sendAll(l_Answer));

	std::vector<uint8_t> l_Wire(4 + 1 + l_Secret.size() + crypto::MAC_BYTES);
	ASSERT_TRUE(l_Link.client.recvAll(l_Wire));
	EXPECT_EQ(std::search(l_Wire.begin(), l_Wire.end(), l_Secret.begin(), l_Secret.end()), l_Wire.end());

	// With the keys it does open
	crypto::Key l_ServerPublic{};
	std::copy(l_KeyFrame.begin() + 5, l_KeyFrame.end(), l_ServerPublic.begin());
	crypto::SessionKeys l_Keys;
	ASSERT_TRUE(crypto::deriveSessionKeys(l_Own, l_ServerPublic, false, l_Keys));
	crypto::Aead l_Open(l_Keys.receive);
	crypto::Mac l_Mac{};
	std::copy(l_Wire.end() - crypto::MAC_BYTES, l_Wire.end(), l_Mac.begin());
	ASSERT_TRUE(l_Open.open(std::span<const uint8_t>(l_Wire.data(), 4), std::span<uint8_t>(l_Wire.data() + 4, 1 + l_Secret.size()), l_Mac));
	EXPECT_EQ(l_Wire[4], 7);
	EXPECT_EQ(std::string(l_Wire.begin() + 5, l_Wire.begin() + 5 + static_cast<std::ptrdiff_t>(l_Secret.size())), l_Secret);

	// The peer goes away first: a shutdown alone does not wake a reader that waits on a silent peer (Windows)
	l_Link.client.close();
}
