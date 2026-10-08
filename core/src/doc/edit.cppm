// Editing operations on a set of objects (what the keyboard shortcuts and the selection bar do). Each one makes a
// single undo step and returns the objects it created, if any.
module;
#include <functional>
#include <span>
#include <vector>

export module wb.doc.edit;

import wb.math;
import wb.doc.object;
import wb.doc.document;
import wb.doc.history;
import wb.doc.commands;

export namespace wb
{
// A copy of some objects, detached from any document
struct ObjectClip
{
	std::vector<Object> objects; // back to front
	Rect bounds{};               // tight world bounds of the copied objects
	std::vector<ImageAsset> assets; // the pictures the copied images use (the clip can outlive the board it came from)

	[[nodiscard]] bool empty() const { return objects.empty(); }
};

[[nodiscard]] ObjectClip copyObjects(const Document& p_Document, std::span<const ObjectId> p_Ids);

// Adds the clip's objects on top, moved so the centre of their bounds lands on p_Center. Returns the new ids.
std::vector<ObjectId> pasteObjects(Document& p_Document, History& p_History, const ObjectClip& p_Clip, DVec2 p_Center, const char* p_Name = "Paste");

// Copies the objects, shifted by p_Offset, on top of the board. Returns the new ids.
std::vector<ObjectId> duplicateObjects(Document& p_Document, History& p_History, std::span<const ObjectId> p_Ids, DVec2 p_Offset);

void deleteObjects(Document& p_Document, History& p_History, std::span<const ObjectId> p_Ids);

void reorderObjects(Document& p_Document, History& p_History, std::span<const ObjectId> p_Ids, ZOrderMove p_Move);

// Mirrors the objects about the centre of their bounds, along the horizontal or the vertical axis of the board
void flipObjects(Document& p_Document, History& p_History, std::span<const ObjectId> p_Ids, bool p_Horizontal);

// Edits the image data of the image objects among p_Ids (one undo step). p_Edit returns false to leave an image alone.
void editImages(Document& p_Document, History& p_History, std::span<const ObjectId> p_Ids, const char* p_Name, const std::function<bool(ImageData&)>& p_Edit);

void recolorObjects(Document& p_Document, History& p_History, std::span<const ObjectId> p_Ids, Color p_Color);

// Applies p_World after each object's own transform (new = p_World * old). Consecutive calls with p_Mergeable
// set collapse into one undo step (arrow-key nudging).
void transformObjects(Document& p_Document, History& p_History, std::span<const ObjectId> p_Ids, const Affine2& p_World, const char* p_Name, bool p_Mergeable);
} // namespace wb
