module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

module wb.net.protocol;

import wb.doc.object;
import wb.net.ops;
import wb.net.wire;
import wb.net.crypto;

namespace wb::net
{
namespace
{
void writeUser(ByteWriter& p_Writer, const UserInfo& p_User)
{
	p_Writer.u16(p_User.slot);
	p_Writer.string(p_User.name);
	p_Writer.u32(p_User.color);
	p_Writer.boolean(p_User.host);
	p_Writer.boolean(p_User.readOnly);
}

bool readUser(ByteReader& p_Reader, UserInfo& p_User)
{
	p_User.slot = p_Reader.u16();
	p_User.name = p_Reader.string(MAX_NAME_BYTES);
	p_User.color = p_Reader.u32();
	p_User.host = p_Reader.boolean();
	p_User.readOnly = p_Reader.boolean();
	return p_Reader.ok();
}

// A whole payload must be consumed: trailing bytes mean the peer and we disagree about the format
bool finished(const ByteReader& p_Reader)
{
	return p_Reader.ok() && p_Reader.remaining() == 0;
}
} // namespace

std::string sanitizeName(const std::string& p_Name)
{
	std::string l_Clean;
	for (const char l_Char : p_Name)
	{
		const auto l_Byte = static_cast<uint8_t>(l_Char);
		if (l_Byte >= 0x20 && l_Byte != 0x7F)
			l_Clean.push_back(l_Char);
	}
	const size_t l_Begin = l_Clean.find_first_not_of(' ');
	if (l_Begin == std::string::npos)
		return "Guest";
	l_Clean = l_Clean.substr(l_Begin, l_Clean.find_last_not_of(' ') - l_Begin + 1);
	if (l_Clean.size() > MAX_NAME_BYTES)
	{
		size_t l_Cut = MAX_NAME_BYTES;
		while (l_Cut > 0 && (static_cast<uint8_t>(l_Clean[l_Cut]) & 0xC0) == 0x80)
			--l_Cut; // do not split a multi-byte character
		l_Clean.resize(l_Cut);
	}
	return l_Clean.empty() ? "Guest" : l_Clean;
}

std::vector<uint8_t> encode(const Challenge& p_Message)
{
	ByteWriter l_Writer;
	l_Writer.raw(p_Message.salt.data(), p_Message.salt.size());
	return l_Writer.take();
}

std::vector<uint8_t> encode(const Hello& p_Message)
{
	ByteWriter l_Writer;
	l_Writer.u16(p_Message.version);
	l_Writer.string(p_Message.name);
	l_Writer.raw(p_Message.proof.data(), p_Message.proof.size());
	return l_Writer.take();
}

std::vector<uint8_t> encode(const Welcome& p_Message)
{
	ByteWriter l_Writer;
	l_Writer.u16(p_Message.slot);
	l_Writer.u32(static_cast<uint32_t>(p_Message.users.size()));
	for (const UserInfo& l_User : p_Message.users)
		writeUser(l_Writer, l_User);
	l_Writer.raw(p_Message.proof.data(), p_Message.proof.size());
	return l_Writer.take();
}

std::vector<uint8_t> encode(const Reject& p_Message)
{
	ByteWriter l_Writer;
	l_Writer.string(p_Message.reason);
	return l_Writer.take();
}

std::vector<uint8_t> encode(const SnapshotChunk& p_Message)
{
	ByteWriter l_Writer;
	l_Writer.boolean(p_Message.last);
	l_Writer.raw(p_Message.bytes.data(), p_Message.bytes.size());
	return l_Writer.take();
}

std::vector<uint8_t> encode(const OpsMessage& p_Message)
{
	ByteWriter l_Writer;
	l_Writer.u32(static_cast<uint32_t>(p_Message.ops.size()));
	for (const Op& l_Op : p_Message.ops)
		encodeOp(l_Writer, l_Op);
	return l_Writer.take();
}

std::vector<uint8_t> encode(const UserInfo& p_Message)
{
	ByteWriter l_Writer;
	writeUser(l_Writer, p_Message);
	return l_Writer.take();
}

std::vector<uint8_t> encode(const UserLeft& p_Message)
{
	ByteWriter l_Writer;
	l_Writer.u16(p_Message.slot);
	return l_Writer.take();
}

bool decode(const std::span<const uint8_t> p_Payload, Challenge& p_Message)
{
	ByteReader l_Reader(p_Payload);
	const std::span<const uint8_t> l_Salt = l_Reader.take(p_Message.salt.size());
	if (!l_Reader.ok())
		return false;
	std::copy(l_Salt.begin(), l_Salt.end(), p_Message.salt.begin());
	return finished(l_Reader);
}

bool decode(const std::span<const uint8_t> p_Payload, Hello& p_Message)
{
	ByteReader l_Reader(p_Payload);
	p_Message.version = l_Reader.u16();
	p_Message.name = l_Reader.string(MAX_NAME_BYTES);
	const std::span<const uint8_t> l_Proof = l_Reader.take(p_Message.proof.size());
	if (!l_Reader.ok())
		return false;
	std::copy(l_Proof.begin(), l_Proof.end(), p_Message.proof.begin());
	return finished(l_Reader);
}

bool decode(const std::span<const uint8_t> p_Payload, Welcome& p_Message)
{
	ByteReader l_Reader(p_Payload);
	p_Message.slot = l_Reader.u16();
	const uint32_t l_Count = l_Reader.u32();
	if (!l_Reader.ok() || l_Count > MAX_USERS)
		return false;
	p_Message.users.clear();
	for (uint32_t i = 0; i < l_Count; ++i)
	{
		UserInfo l_User;
		if (!readUser(l_Reader, l_User))
			return false;
		p_Message.users.push_back(std::move(l_User));
	}
	const std::span<const uint8_t> l_Proof = l_Reader.take(p_Message.proof.size());
	if (!l_Reader.ok())
		return false;
	std::copy(l_Proof.begin(), l_Proof.end(), p_Message.proof.begin());
	return finished(l_Reader);
}

bool decode(const std::span<const uint8_t> p_Payload, Reject& p_Message)
{
	ByteReader l_Reader(p_Payload);
	p_Message.reason = l_Reader.string(MAX_REASON_BYTES);
	return finished(l_Reader);
}

bool decode(const std::span<const uint8_t> p_Payload, SnapshotChunk& p_Message)
{
	ByteReader l_Reader(p_Payload);
	p_Message.last = l_Reader.boolean();
	if (!l_Reader.ok())
		return false;
	const std::span<const uint8_t> l_Rest = l_Reader.take(l_Reader.remaining());
	p_Message.bytes.assign(l_Rest.begin(), l_Rest.end());
	return l_Reader.ok();
}

bool decode(const std::span<const uint8_t> p_Payload, OpsMessage& p_Message)
{
	ByteReader l_Reader(p_Payload);
	const uint32_t l_Count = l_Reader.u32();
	// Every op takes at least one byte: a larger count than the payload can hold is a lie
	if (!l_Reader.ok() || l_Count > MAX_OPS_PER_FRAME || l_Count > l_Reader.remaining())
		return false;
	p_Message.ops.clear();
	p_Message.ops.reserve(l_Count);
	for (uint32_t i = 0; i < l_Count; ++i)
	{
		Op l_Op;
		if (!decodeOp(l_Reader, l_Op))
			return false;
		p_Message.ops.push_back(std::move(l_Op));
	}
	return finished(l_Reader);
}

bool decode(const std::span<const uint8_t> p_Payload, UserInfo& p_Message)
{
	ByteReader l_Reader(p_Payload);
	return readUser(l_Reader, p_Message) && finished(l_Reader);
}

bool decode(const std::span<const uint8_t> p_Payload, UserLeft& p_Message)
{
	ByteReader l_Reader(p_Payload);
	p_Message.slot = l_Reader.u16();
	return finished(l_Reader);
}

std::vector<uint8_t> encode(const AssetRequest& p_Message)
{
	ByteWriter l_Writer;
	l_Writer.u64(p_Message.id);
	l_Writer.u32(p_Message.ticket);
	return l_Writer.take();
}

std::vector<uint8_t> encode(const AssetPreviewMessage& p_Message)
{
	ByteWriter l_Writer;
	l_Writer.u64(p_Message.id);
	l_Writer.u32(p_Message.width);
	l_Writer.u32(p_Message.height);
	l_Writer.raw(p_Message.rgba.data(), p_Message.rgba.size());
	return l_Writer.take();
}

bool decode(const std::span<const uint8_t> p_Payload, AssetPreviewMessage& p_Message)
{
	ByteReader l_Reader(p_Payload);
	p_Message.id = l_Reader.u64();
	p_Message.width = l_Reader.u32();
	p_Message.height = l_Reader.u32();
	if (!l_Reader.ok() || p_Message.id == INVALID_ASSET_ID || p_Message.width > MAX_PREVIEW_SIDE || p_Message.height > MAX_PREVIEW_SIDE || (p_Message.width == 0) != (p_Message.height == 0))
		return false;
	const size_t l_Bytes = static_cast<size_t>(p_Message.width) * p_Message.height * 4;
	const std::span<const uint8_t> l_Pixels = l_Reader.take(l_Bytes);
	if (!l_Reader.ok() || l_Reader.remaining() != 0)
		return false;
	p_Message.rgba.assign(l_Pixels.begin(), l_Pixels.end());
	return true;
}

std::vector<uint8_t> encode(const CursorMessage& p_Message)
{
	ByteWriter l_Writer;
	l_Writer.u16(p_Message.slot);
	l_Writer.f64(p_Message.x);
	l_Writer.f64(p_Message.y);
	l_Writer.boolean(p_Message.visible);
	return l_Writer.take();
}

bool decode(const std::span<const uint8_t> p_Payload, CursorMessage& p_Message)
{
	ByteReader l_Reader(p_Payload);
	p_Message.slot = l_Reader.u16();
	p_Message.x = l_Reader.f64();
	p_Message.y = l_Reader.f64();
	p_Message.visible = l_Reader.boolean();
	constexpr double LIMIT = 1e12;
	return finished(l_Reader) && std::isfinite(p_Message.x) && std::isfinite(p_Message.y) && std::abs(p_Message.x) <= LIMIT && std::abs(p_Message.y) <= LIMIT;
}

std::vector<uint8_t> encode(const VideoStateMessage& p_Message)
{
	ByteWriter l_Writer;
	l_Writer.u64(p_Message.id);
	l_Writer.boolean(p_Message.playing);
	l_Writer.f64(p_Message.position);
	return l_Writer.take();
}

bool decode(const std::span<const uint8_t> p_Payload, VideoStateMessage& p_Message)
{
	ByteReader l_Reader(p_Payload);
	p_Message.id = l_Reader.u64();
	p_Message.playing = l_Reader.boolean();
	p_Message.position = l_Reader.f64();
	return finished(l_Reader) && p_Message.id != INVALID_OBJECT_ID && std::isfinite(p_Message.position) && p_Message.position >= 0.0 && p_Message.position <= 86400.0;
}

std::vector<uint8_t> encode(const StrokePreviewMessage& p_Message)
{
	ByteWriter l_Writer;
	l_Writer.u16(p_Message.slot);
	l_Writer.u32(p_Message.stroke);
	l_Writer.boolean(p_Message.end);
	for (const float l_Channel : p_Message.color)
		l_Writer.f32(l_Channel);
	l_Writer.f64(p_Message.originX);
	l_Writer.f64(p_Message.originY);
	l_Writer.u32(p_Message.base);
	l_Writer.u32(static_cast<uint32_t>(p_Message.points.size()));
	for (const StrokePoint& l_Point : p_Message.points)
	{
		l_Writer.f32(l_Point.position.x);
		l_Writer.f32(l_Point.position.y);
		l_Writer.f32(l_Point.radius);
	}
	return l_Writer.take();
}

bool decode(const std::span<const uint8_t> p_Payload, StrokePreviewMessage& p_Message)
{
	ByteReader l_Reader(p_Payload);
	p_Message.slot = l_Reader.u16();
	p_Message.stroke = l_Reader.u32();
	p_Message.end = l_Reader.boolean();
	for (float& l_Channel : p_Message.color)
		l_Channel = l_Reader.f32();
	p_Message.originX = l_Reader.f64();
	p_Message.originY = l_Reader.f64();
	p_Message.base = l_Reader.u32();
	const uint32_t l_Count = l_Reader.u32();
	if (!l_Reader.ok() || l_Count > MAX_STROKE_POINTS_PER_MESSAGE || l_Count > l_Reader.remaining() / 12 || p_Message.base > MAX_LIVE_STROKE_POINTS)
		return false;
	p_Message.points.clear();
	p_Message.points.reserve(l_Count);
	constexpr float LIMIT = 1e9f;
	for (uint32_t l_Index = 0; l_Index < l_Count; ++l_Index)
	{
		StrokePoint l_Point;
		l_Point.position.x = l_Reader.f32();
		l_Point.position.y = l_Reader.f32();
		l_Point.radius = l_Reader.f32();
		if (!std::isfinite(l_Point.position.x) || !std::isfinite(l_Point.position.y) || !std::isfinite(l_Point.radius) || std::abs(l_Point.position.x) > LIMIT || std::abs(l_Point.position.y) > LIMIT || l_Point.radius < 0.f || l_Point.radius > LIMIT)
			return false;
		p_Message.points.push_back(l_Point);
	}
	constexpr double ORIGIN_LIMIT = 1e12;
	for (const float l_Channel : p_Message.color)
	{
		if (!(l_Channel >= 0.f && l_Channel <= 1.f))
			return false; // also catches NaN
	}
	return finished(l_Reader) && std::isfinite(p_Message.originX) && std::isfinite(p_Message.originY) && std::abs(p_Message.originX) <= ORIGIN_LIMIT && std::abs(p_Message.originY) <= ORIGIN_LIMIT;
}

std::vector<uint8_t> encode(const AssetChunk& p_Message)
{
	ByteWriter l_Writer;
	l_Writer.u64(p_Message.id);
	l_Writer.u32(p_Message.ticket);
	l_Writer.u64(p_Message.total);
	l_Writer.u64(p_Message.offset);
	l_Writer.u32(p_Message.width);
	l_Writer.u32(p_Message.height);
	l_Writer.string(p_Message.name);
	l_Writer.raw(p_Message.bytes.data(), p_Message.bytes.size());
	return l_Writer.take();
}

bool decode(const std::span<const uint8_t> p_Payload, AssetRequest& p_Message)
{
	ByteReader l_Reader(p_Payload);
	p_Message.id = l_Reader.u64();
	p_Message.ticket = l_Reader.u32();
	return finished(l_Reader) && p_Message.id != INVALID_ASSET_ID;
}

bool decode(const std::span<const uint8_t> p_Payload, AssetChunk& p_Message)
{
	ByteReader l_Reader(p_Payload);
	p_Message.id = l_Reader.u64();
	p_Message.ticket = l_Reader.u32();
	p_Message.total = l_Reader.u64();
	p_Message.offset = l_Reader.u64();
	p_Message.width = l_Reader.u32();
	p_Message.height = l_Reader.u32();
	p_Message.name = l_Reader.string(MAX_ASSET_NAME_BYTES);
	if (!l_Reader.ok())
		return false;
	const std::span<const uint8_t> l_Rest = l_Reader.take(l_Reader.remaining());
	if (!l_Reader.ok() || p_Message.id == INVALID_ASSET_ID || p_Message.total == 0 || p_Message.total > MAX_ASSET_BYTES || l_Rest.empty() || l_Rest.size() > ASSET_CHUNK || p_Message.offset >= p_Message.total || l_Rest.size() > p_Message.total - p_Message.offset)
		return false;
	p_Message.bytes.assign(l_Rest.begin(), l_Rest.end());
	return true;
}
} // namespace wb::net
