// Standard document commands. Each restores the exact z-order on undo.
module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module wb.doc.commands;

import wb.math;
import wb.doc.document;
import wb.doc.history;
import wb.doc.object;

export namespace wb
{
// Whether a picture or video object uses file p_Asset
[[nodiscard]] inline bool objectUsesAsset(const Object* p_Object, const AssetId p_Asset)
{
	if (p_Object == nullptr)
		return false;
	if (const ImageData* l_Image = p_Object->image())
		return l_Image->asset == p_Asset;
	if (const VideoData* l_Video = p_Object->video())
		return l_Video->asset == p_Asset;
	return false;
}

// Adds new objects on top of the board
class AddObjectsCommand final : public Command
{
public:
	AddObjectsCommand(std::vector<std::unique_ptr<Object>> p_Objects, std::string p_Name);

	void apply(Document& p_Document) override;
	void revert(Document& p_Document) override;
	[[nodiscard]] std::string_view name() const override { return m_Name; }
	[[nodiscard]] bool dependsOn(ObjectId p_Id, bool p_Removed) const override
	{
		// Undo removes and redo re-adds the objects; neither works once a peer deleted them
		return p_Removed && std::ranges::any_of(m_Entries, [p_Id](const Entry& p_Entry) { return p_Entry.id == p_Id; });
	}
	[[nodiscard]] bool usesAsset(AssetId p_Asset) const override
	{
		return std::ranges::any_of(m_Entries, [p_Asset](const Entry& p_Entry) { return objectUsesAsset(p_Entry.object.get(), p_Asset); });
	}

private:
	struct Entry
	{
		ObjectId id = INVALID_OBJECT_ID;
		std::unique_ptr<Object> object; // owned while not in the document
		size_t zIndex = SIZE_MAX;       // SIZE_MAX until first applied (append)
	};
	std::vector<Entry> m_Entries;
	std::string m_Name;
};

// Removes objects (and puts them back at their original z-index on undo)
class RemoveObjectsCommand final : public Command
{
public:
	RemoveObjectsCommand(std::vector<ObjectId> p_Ids, std::string p_Name);

	void apply(Document& p_Document) override;
	void revert(Document& p_Document) override;
	[[nodiscard]] std::string_view name() const override { return m_Name; }
	[[nodiscard]] bool dependsOn(ObjectId p_Id, bool p_Removed) const override
	{
		(void)p_Id;
		(void)p_Removed;
		return false;
	}
	[[nodiscard]] bool usesAsset(AssetId p_Asset) const override
	{
		return std::ranges::any_of(m_Removed, [p_Asset](const Document::Removed& p_Entry) { return objectUsesAsset(p_Entry.object.get(), p_Asset); });
	}

private:
	std::vector<ObjectId> m_Ids;
	std::vector<Document::Removed> m_Removed; // sorted by z-index, ascending
	std::string m_Name;
};

// A batch of "replace one object by zero or more others, at the same z-position" steps (the eraser's cuts).
// Steps are performed live through replace() while a gesture is in progress; the finished command is then pushed
// to the history, so the whole gesture is a single undo step.
class ReplaceObjectsCommand final : public Command
{
public:
	explicit ReplaceObjectsCommand(std::string p_Name) : m_Name(std::move(p_Name)) {}

	// Performs the replacement now and records it. Unknown ids are ignored.
	void replace(Document& p_Document, ObjectId p_Id, std::vector<std::unique_ptr<Object>> p_Added);
	[[nodiscard]] bool empty() const { return m_Steps.empty(); }

	void apply(Document& p_Document) override;
	void revert(Document& p_Document) override;
	[[nodiscard]] std::string_view name() const override { return m_Name; }
	[[nodiscard]] bool dependsOn(ObjectId p_Id, bool p_Removed) const override
	{
		if (!p_Removed)
			return false;
		return std::ranges::any_of(m_Steps, [p_Id](const Step& p_Step) { return p_Step.removedId == p_Id || std::ranges::find(p_Step.addedIds, p_Id) != p_Step.addedIds.end(); });
	}
	[[nodiscard]] bool usesAsset(AssetId p_Asset) const override
	{
		return std::ranges::any_of(m_Steps, [p_Asset](const Step& p_Step)
		{
			return objectUsesAsset(p_Step.removed.get(), p_Asset) || std::ranges::any_of(p_Step.added, [p_Asset](const std::unique_ptr<Object>& p_Object) { return objectUsesAsset(p_Object.get(), p_Asset); });
		});
	}

private:
	struct Step
	{
		ObjectId removedId = INVALID_OBJECT_ID;
		size_t zIndex = 0;
		std::unique_ptr<Object> removed;            // owned while the step is applied (the object is out of the document)
		std::vector<ObjectId> addedIds;
		std::vector<std::unique_ptr<Object>> added; // owned while the step is reverted
	};
	std::vector<Step> m_Steps;
	std::string m_Name;
};
// Gives objects new transforms. Used for drags (already applied live, then pushed) and for nudges, which merge
// into one undo step while the history allows it.
class TransformObjectsCommand final : public Command
{
public:
	struct Entry
	{
		ObjectId id = INVALID_OBJECT_ID;
		Affine2 before{};
		Affine2 after{};
	};

	TransformObjectsCommand(std::vector<Entry> p_Entries, std::string p_Name, bool p_Mergeable = false);

	void apply(Document& p_Document) override;
	void revert(Document& p_Document) override;
	[[nodiscard]] std::string_view name() const override { return m_Name; }
	[[nodiscard]] bool dependsOn(ObjectId p_Id, bool p_Removed) const override
	{
		(void)p_Removed;
		return std::ranges::any_of(m_Entries, [p_Id](const Entry& p_Entry) { return p_Entry.id == p_Id; });
	}
	bool mergeWith(Command& p_Next) override;
	[[nodiscard]] bool changesAnything() const;

private:
	std::vector<Entry> m_Entries;
	std::string m_Name;
	bool m_Mergeable = false;
};

enum class ZOrderMove : uint8_t
{
	ToFront,
	ToBack,
	Forward,  // one step up past the next object that is not part of the move
	Backward,
};

// Changes the stacking order of some objects, keeping their relative order
class ReorderObjectsCommand final : public Command
{
public:
	ReorderObjectsCommand(std::vector<ObjectId> p_Ids, ZOrderMove p_Move, std::string p_Name);

	void apply(Document& p_Document) override;
	void revert(Document& p_Document) override;
	[[nodiscard]] std::string_view name() const override { return m_Name; }
	[[nodiscard]] bool dependsOn(ObjectId p_Id, bool p_Removed) const override
	{
		(void)p_Id;
		(void)p_Removed;
		return false;
	}
	// After apply(): whether the order really changed (moving the top object forward changes nothing)
	[[nodiscard]] bool changedOrder() const { return m_Changed; }

private:
	std::vector<ObjectId> m_Ids;
	ZOrderMove m_Move;
	bool m_Changed = false;
	std::vector<std::pair<ObjectId, size_t>> m_OldPositions; // ascending by position
	std::string m_Name;
};

// Replaces the data of image objects (play/pause, a recompressed asset...)
class SetImageDataCommand final : public Command
{
 public:
	struct Entry
	{
		ObjectId id = INVALID_OBJECT_ID;
		ImageData before{};
		ImageData after{};
		// Cropping changes how big the shown part is, so the object moves with it (the shown part stays where it is)
		bool setTransform = false;
		Affine2 transformBefore{};
		Affine2 transformAfter{};
	};

	SetImageDataCommand(std::vector<Entry> p_Entries, std::string p_Name);

	void apply(Document& p_Document) override;
	void revert(Document& p_Document) override;
	[[nodiscard]] std::string_view name() const override { return m_Name; }
	[[nodiscard]] bool dependsOn(ObjectId p_Id, bool p_Removed) const override
	{
		(void)p_Removed;
		return std::ranges::any_of(m_Entries, [p_Id](const Entry& p_Entry) { return p_Entry.id == p_Id; });
	}
	[[nodiscard]] bool usesAsset(AssetId p_Asset) const override
	{
		return std::ranges::any_of(m_Entries, [p_Asset](const Entry& p_Entry) { return p_Entry.before.asset == p_Asset || p_Entry.after.asset == p_Asset; });
	}

private:
	std::vector<Entry> m_Entries;
	std::string m_Name;
};

// Replaces the data of video objects (loop and sound switches)
class SetVideoDataCommand final : public Command
{
public:
	struct Entry
	{
		ObjectId id = INVALID_OBJECT_ID;
		VideoData before{};
		VideoData after{};
		// Cropping changes how big the shown part is, so the object moves with it
		bool setTransform = false;
		Affine2 transformBefore{};
		Affine2 transformAfter{};
	};

	SetVideoDataCommand(std::vector<Entry> p_Entries, std::string p_Name);

	void apply(Document& p_Document) override;
	void revert(Document& p_Document) override;
	[[nodiscard]] std::string_view name() const override { return m_Name; }
	[[nodiscard]] bool dependsOn(ObjectId p_Id, bool p_Removed) const override
	{
		(void)p_Removed;
		return std::ranges::any_of(m_Entries, [p_Id](const Entry& p_Entry) { return p_Entry.id == p_Id; });
	}
	[[nodiscard]] bool usesAsset(AssetId p_Asset) const override
	{
		return std::ranges::any_of(m_Entries, [p_Asset](const Entry& p_Entry) { return p_Entry.before.asset == p_Asset || p_Entry.after.asset == p_Asset; });
	}

private:
	std::vector<Entry> m_Entries;
	std::string m_Name;
};

// Replaces the data of text objects. The transform changes with it: a box that grows keeps its anchored corner or
// edge in place, which moves the object's origin (see anchoredTextTransform).
class SetTextCommand final : public Command
{
public:
	struct Entry
	{
		ObjectId id = INVALID_OBJECT_ID;
		TextData before{};
		TextData after{};
		Affine2 transformBefore{};
		Affine2 transformAfter{};
	};

	SetTextCommand(std::vector<Entry> p_Entries, std::string p_Name);
	void apply(Document& p_Document) override;
	void revert(Document& p_Document) override;
	[[nodiscard]] std::string_view name() const override { return m_Name; }
	[[nodiscard]] bool dependsOn(ObjectId p_Id, bool p_Removed) const override
	{
		(void)p_Removed;
		return std::ranges::any_of(m_Entries, [p_Id](const Entry& p_Entry) { return p_Entry.id == p_Id; });
	}

private:
	std::vector<Entry> m_Entries;
	std::string m_Name;
};

// Replaces the points of strokes (smoothing)
class SetStrokePointsCommand final : public Command
{
public:
	struct Entry
	{
		ObjectId id = INVALID_OBJECT_ID;
		std::vector<StrokePoint> before;
		std::vector<StrokePoint> after;
	};

	SetStrokePointsCommand(std::vector<Entry> p_Entries, std::string p_Name);

	void apply(Document& p_Document) override;
	void revert(Document& p_Document) override;
	[[nodiscard]] std::string_view name() const override { return m_Name; }
	[[nodiscard]] bool dependsOn(ObjectId p_Id, bool p_Removed) const override
	{
		(void)p_Removed;
		return std::ranges::any_of(m_Entries, [p_Id](const Entry& p_Entry) { return p_Entry.id == p_Id; });
	}

private:
	std::vector<Entry> m_Entries;
	std::string m_Name;
};

// Locks or unlocks objects
class SetLockedCommand final : public Command
{
public:
	SetLockedCommand(std::vector<ObjectId> p_Ids, bool p_Locked, std::string p_Name);

	void apply(Document& p_Document) override;
	void revert(Document& p_Document) override;
	[[nodiscard]] std::string_view name() const override { return m_Name; }
	[[nodiscard]] bool dependsOn(ObjectId p_Id, bool p_Removed) const override
	{
		(void)p_Removed;
		return std::ranges::find(m_Ids, p_Id) != m_Ids.end();
	}

private:
	std::vector<ObjectId> m_Ids;
	bool m_Locked = false;
	std::string m_Name;
};

// Puts objects into groups, or takes them out (group INVALID_OBJECT_ID)
class SetGroupCommand final : public Command
{
public:
	struct Entry
	{
		ObjectId id = INVALID_OBJECT_ID;
		ObjectId before = INVALID_OBJECT_ID;
		ObjectId after = INVALID_OBJECT_ID;
	};

	SetGroupCommand(std::vector<Entry> p_Entries, std::string p_Name);

	void apply(Document& p_Document) override;
	void revert(Document& p_Document) override;
	[[nodiscard]] std::string_view name() const override { return m_Name; }
	[[nodiscard]] bool dependsOn(ObjectId p_Id, bool p_Removed) const override
	{
		(void)p_Removed;
		return std::ranges::any_of(m_Entries, [p_Id](const Entry& p_Entry) { return p_Entry.id == p_Id; });
	}

private:
	std::vector<Entry> m_Entries;
	std::string m_Name;
};

// Sets the note of objects (the text of the notes panel)
class SetNoteCommand final : public Command
{
public:
	struct Entry
	{
		ObjectId id = INVALID_OBJECT_ID;
		std::string before;
		std::string after;
	};

	SetNoteCommand(std::vector<Entry> p_Entries, std::string p_Name);

	void apply(Document& p_Document) override;
	void revert(Document& p_Document) override;
	// Typing: a note edited again right away (same objects) stays one undo step
	bool mergeWith(Command& p_Next) override;
	[[nodiscard]] std::string_view name() const override { return m_Name; }
	[[nodiscard]] bool dependsOn(ObjectId p_Id, bool p_Removed) const override
	{
		(void)p_Removed;
		return std::ranges::any_of(m_Entries, [p_Id](const Entry& p_Entry) { return p_Entry.id == p_Id; });
	}

private:
	std::vector<Entry> m_Entries;
	std::string m_Name;
};

// Recolours strokes and text (the alpha channel of each is kept)
class SetStrokeColorCommand final : public Command
{
public:
	SetStrokeColorCommand(std::vector<ObjectId> p_Ids, Color p_Color, std::string p_Name);

	void apply(Document& p_Document) override;
	void revert(Document& p_Document) override;
	[[nodiscard]] std::string_view name() const override { return m_Name; }
	[[nodiscard]] bool dependsOn(ObjectId p_Id, bool p_Removed) const override
	{
		(void)p_Removed;
		return std::ranges::find(m_Ids, p_Id) != m_Ids.end();
	}

private:
	std::vector<ObjectId> m_Ids;
	std::vector<Color> m_Old;
	std::vector<std::vector<ColorSpan>> m_OldSpans; // per id: letters of a text that had a colour of their own
	Color m_New{};
	std::string m_Name;
};
} // namespace wb
