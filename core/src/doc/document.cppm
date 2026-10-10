// The board: objects in z-order (back to front) plus change notifications.
//
// All mutations go through Document so listeners (GPU caches, spatial index, autosave...) stay in sync.
// Undoable edits are expressed as Commands (wb.doc.history) that call these primitives.
module;
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

export module wb.doc.document;

import wb.math;
import wb.doc.object;
import wb.io.blob;

export namespace wb
{
// Object and asset ids carry the slot of the peer that created them in their top bits (see Document::setIdSlot)
inline constexpr unsigned ID_SLOT_SHIFT = 48;
[[nodiscard]] inline uint16_t idSlotOf(const uint64_t p_Id) { return static_cast<uint16_t>(p_Id >> ID_SLOT_SHIFT); }

// What a modify() call touched, so listeners can skip work (e.g. no GPU re-upload for a pure move)
namespace ObjectChange
{
inline constexpr uint32_t Transform = 1u << 0;
inline constexpr uint32_t Geometry = 1u << 1; // payload shape (stroke points, image size, text...)
inline constexpr uint32_t Style = 1u << 2;    // color, size and other appearance
inline constexpr uint32_t Lock = 1u << 3;     // locked / unlocked
inline constexpr uint32_t All = Transform | Geometry | Style | Lock;
} // namespace ObjectChange

// Files at least this big are not kept in memory: they are read from disk when needed
inline constexpr uint64_t LARGE_ASSET_BYTES = uint64_t{ 16 } << 20;

// An image or video file as it was imported (PNG, JPEG, GIF, MP4...), kept byte for byte so saving never re-encodes it.
// Small files sit in `bytes`. Big ones (videos, mostly) are never loaded: `stream` reads them from wherever they are on
// disk (the original file, the board file they were saved in, or the copy that was downloaded from a collaborator).
struct ImageAsset
{
	AssetId id = INVALID_ASSET_ID;
	std::vector<uint8_t> bytes;
	std::shared_ptr<const ByteSource> stream; // when set, the file is here and `bytes` is empty
	uint32_t width = 0;  // pixels of the first frame
	uint32_t height = 0;
	std::string name;    // file name it came from, for the user's benefit

	[[nodiscard]] uint64_t size() const { return stream ? stream->size() : bytes.size(); }
	// A file that is still being downloaded can be played from the start, but nothing else should rely on all of it
	[[nodiscard]] bool complete() const { return !stream || stream->available() >= stream->size(); }
	// Copies up to p_Count bytes from p_Offset; fewer only at the end or when the file cannot be read any more
	size_t read(const uint64_t p_Offset, void* p_Target, size_t p_Count, const std::atomic<bool>* p_Cancel = nullptr) const
	{
		if (stream)
			return stream->read(p_Offset, p_Target, p_Count, p_Cancel);
		if (p_Offset >= bytes.size())
			return 0;
		p_Count = static_cast<size_t>(std::min<uint64_t>(p_Count, bytes.size() - p_Offset));
		std::memcpy(p_Target, bytes.data() + p_Offset, p_Count);
		return p_Count;
	}
	// The file as a source that outlives this asset (memory is copied)
	[[nodiscard]] std::shared_ptr<const ByteSource> source() const { return stream ? stream : std::make_shared<MemorySource>(bytes); }
	// The whole file in memory; empty when it is bigger than p_Max or cannot be read
	[[nodiscard]] std::vector<uint8_t> load(const uint64_t p_Max = uint64_t{ 1 } << 30) const
	{
		return stream ? readAllBytes(*stream, p_Max) : (bytes.size() <= p_Max ? bytes : std::vector<uint8_t>{});
	}
};

// A font file embedded in the board so text looks the same on machines that do not have the font. Identified by
// family and style (TextStyle flags of the face the file really is).
struct FontAsset
{
	std::string family;
	uint8_t style = 0;
	std::vector<uint8_t> bytes;
};

// A tiny RGBA picture standing in for an image or video whose file has not arrived yet (collaboration). Not saved.
struct AssetPreview
{
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint8_t> rgba; // width * height * 4
};

class DocumentListener
{
public:
	virtual ~DocumentListener() = default;
	virtual void onObjectAdded(const Object& p_Object) = 0;
	virtual void onObjectRemoved(const Object& p_Object) = 0;
	virtual void onObjectChanged(const Object& p_Object, uint32_t p_Changes) = 0;
	virtual void onObjectsReordered() {}
	virtual void onDocumentCleared() = 0;
};

class Document
{
public:
	struct Removed
	{
		std::unique_ptr<Object> object;
		size_t zIndex = 0;
	};

	Document() = default;
	Document(const Document&) = delete;
	Document& operator=(const Document&) = delete;

	[[nodiscard]] ObjectId allocateId() { return m_NextId++; }
	// The id the next allocateId() returns (saved in files so ids stay unique across sessions)
	[[nodiscard]] ObjectId peekNextId() const { return m_NextId; }
	// Ensures future ids are greater than p_Id (used when loading files). Ids from another slot are ignored.
	void reserveId(ObjectId p_Id);

	// Collaboration: the top 16 bits of every object and asset id this document creates are its slot, so peers never
	// hand out the same id. Slot 0 (the default) is a plain single-user counter. Changing the slot restarts the
	// counters above whatever the document already holds for that slot.
	void setIdSlot(uint16_t p_Slot);
	[[nodiscard]] uint16_t idSlot() const { return m_Slot; }

	// Inserts at p_ZIndex (clamped; SIZE_MAX appends on top). The object id must be valid and unused.
	void insert(std::unique_ptr<Object> p_Object, size_t p_ZIndex = SIZE_MAX);
	// Removes the object and hands it back with the z-index it had
	[[nodiscard]] Removed take(ObjectId p_Id);
	// Applies p_Edit, then bumps the version and notifies listeners. No-op for unknown ids.
	// p_Changes (ObjectChange flags) describes what the edit may touch.
	void modify(ObjectId p_Id, const std::function<void(Object&)>& p_Edit, uint32_t p_Changes = ObjectChange::All);
	// Moves an object to a new z-index (clamped)
	void moveTo(ObjectId p_Id, size_t p_ZIndex);
	// Reorders every object: p_Order must list each existing id exactly once (back to front). Ignored otherwise.
	void setOrder(std::span<const ObjectId> p_Order);
	void clear();

	[[nodiscard]] const Object* find(ObjectId p_Id) const;
	[[nodiscard]] size_t indexOf(ObjectId p_Id) const; // SIZE_MAX when absent
	[[nodiscard]] std::span<const std::unique_ptr<Object>> objects() const { return m_Objects; }
	[[nodiscard]] size_t size() const { return m_Objects.size(); }
	[[nodiscard]] bool empty() const { return m_Objects.empty(); }
	[[nodiscard]] Rect contentBounds() const;
	// Every object's world bounds in one array, in the order of objects(). Scanning it is much cheaper than walking the
	// objects, so culling and hit tests use it to skip what is far away.
	[[nodiscard]] std::span<const Rect> boundsList() const;

	// ---- image assets (immutable once added; an edit makes a new asset)
	// Stores the asset and returns its id. Identical bytes share one asset.
	AssetId addAsset(ImageAsset p_Asset);
	// Loading: stores the asset under the id it was saved with. False when the id is taken or invalid.
	bool insertAsset(ImageAsset p_Asset);
	// The file of an asset now also exists as p_Source (same content: the board was saved, a download finished). The asset
	// lets go of its memory and reads from there from now on. False when the asset is unknown or the sizes differ.
	bool rebindAsset(AssetId p_Id, std::shared_ptr<const ByteSource> p_Source);
	[[nodiscard]] const ImageAsset* findAsset(AssetId p_Id) const;
	// Removes an asset (a download that will never finish). Objects that use it keep their id and show a placeholder.
	void eraseAsset(AssetId p_Id);
	[[nodiscard]] const std::unordered_map<AssetId, ImageAsset>& assets() const { return m_Assets; }

	void setPreview(AssetId p_Id, AssetPreview p_Preview) { m_Previews[p_Id] = std::move(p_Preview); }
	[[nodiscard]] const AssetPreview* findPreview(AssetId p_Id) const
	{
		const auto l_It = m_Previews.find(p_Id);
		return l_It != m_Previews.end() ? &l_It->second : nullptr;
	}

	// ---- embedded fonts (replaced when the same family and style is stored again)
	void setFontAsset(FontAsset p_Font);
	[[nodiscard]] const FontAsset* findFontAsset(std::string_view p_Family, uint8_t p_Style) const;
	[[nodiscard]] const std::vector<FontAsset>& fontAssets() const { return m_Fonts; }

	// Incremented by every mutation
	[[nodiscard]] uint64_t revision() const { return m_Revision; }

	void addListener(DocumentListener* p_Listener);
	void removeListener(DocumentListener* p_Listener);

private:
	std::vector<std::unique_ptr<Object>> m_Objects; // back to front
	// boundsList() cache: kept in step by insert/take, patched lazily after modify (a few ids) or rebuilt (many, reorders)
	mutable std::vector<Rect> m_Bounds;
	mutable std::vector<ObjectId> m_StaleBounds;
	mutable bool m_BoundsDirty = true;
	std::unordered_map<ObjectId, Object*> m_ById;
	std::vector<DocumentListener*> m_Listeners;
	std::unordered_map<AssetId, ImageAsset> m_Assets;
	std::unordered_map<AssetId, AssetPreview> m_Previews;
	std::vector<FontAsset> m_Fonts;
	std::unordered_map<uint64_t, std::vector<AssetId>> m_AssetsByHash;
	uint16_t m_Slot = 0;
	ObjectId m_NextId = 1;
	AssetId m_NextAsset = 1;
	uint64_t m_Revision = 0;
};
} // namespace wb
