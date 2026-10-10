module;
#include <algorithm>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

module wb.doc.edit;

import wb.math;
import wb.doc.object;
import wb.doc.document;
import wb.doc.history;
import wb.doc.commands;
import wb.doc.hit;
import wb.brush.smoothing;

namespace wb
{
namespace
{
// The ids that exist, in back-to-front order
std::vector<const Object*> liveObjects(const Document& p_Document, const std::span<const ObjectId> p_Ids)
{
	const std::unordered_set<ObjectId> l_Wanted(p_Ids.begin(), p_Ids.end());
	std::vector<const Object*> l_Objects;
	for (const std::unique_ptr<Object>& l_Object : p_Document.objects())
	{
		if (l_Wanted.contains(l_Object->id))
			l_Objects.push_back(l_Object.get());
	}
	return l_Objects;
}

// The same, without the locked ones: what edits may touch
std::vector<const Object*> editableObjects(const Document& p_Document, const std::span<const ObjectId> p_Ids)
{
	std::vector<const Object*> l_Objects = liveObjects(p_Document, p_Ids);
	std::erase_if(l_Objects, [](const Object* p_Object) { return p_Object->locked; });
	return l_Objects;
}

std::vector<ObjectId> insertCopies(Document& p_Document, History& p_History, const std::vector<std::unique_ptr<Object>>& p_Sources, const Affine2& p_Shift, const char* p_Name)
{
	std::vector<std::unique_ptr<Object>> l_Copies;
	std::vector<ObjectId> l_Ids;
	l_Copies.reserve(p_Sources.size());
	// A group that is copied stays a group, but a new one: the copies must not join the originals. Copies that are alone
	// from their group (the rest was not copied) end up in none.
	std::unordered_map<ObjectId, size_t> l_GroupSizes;
	for (const std::unique_ptr<Object>& l_Source : p_Sources)
		++l_GroupSizes[l_Source->group];
	std::unordered_map<ObjectId, ObjectId> l_GroupMap;
	for (const std::unique_ptr<Object>& l_Source : p_Sources)
	{
		std::unique_ptr<Object> l_Copy = cloneObject(*l_Source, p_Document.allocateId());
		if (l_Source->group != INVALID_OBJECT_ID)
		{
			if (l_GroupSizes[l_Source->group] < 2)
				l_Copy->group = INVALID_OBJECT_ID;
			else
			{
				ObjectId& l_NewGroup = l_GroupMap[l_Source->group];
				if (l_NewGroup == INVALID_OBJECT_ID)
					l_NewGroup = p_Document.allocateId();
				l_Copy->group = l_NewGroup;
			}
		}
		l_Copy->transform = p_Shift * l_Copy->transform;
		l_Ids.push_back(l_Copy->id);
		l_Copies.push_back(std::move(l_Copy));
	}
	if (!l_Copies.empty())
		p_History.execute(p_Document, std::make_unique<AddObjectsCommand>(std::move(l_Copies), p_Name));
	return l_Ids;
}
} // namespace

ObjectClip copyObjects(const Document& p_Document, const std::span<const ObjectId> p_Ids)
{
	ObjectClip l_Clip;
	std::unordered_set<AssetId> l_Assets;
	for (const Object* l_Object : liveObjects(p_Document, p_Ids))
	{
		l_Clip.objects.push_back(*l_Object);
		l_Clip.bounds.expand(tightWorldBounds(*l_Object));
		const AssetId l_Used = l_Object->image() != nullptr ? l_Object->image()->asset : (l_Object->video() != nullptr ? l_Object->video()->asset : INVALID_ASSET_ID);
		if (l_Used != INVALID_ASSET_ID && l_Assets.insert(l_Used).second)
		{
			if (const ImageAsset* l_Asset = p_Document.findAsset(l_Used))
				l_Clip.assets.push_back(*l_Asset);
		}
	}
	return l_Clip;
}

std::vector<ObjectId> pasteObjects(Document& p_Document, History& p_History, const ObjectClip& p_Clip, const DVec2 p_Center, const char* p_Name)
{
	if (p_Clip.empty())
		return {};
	const Affine2 l_Shift = Affine2::translate(p_Center - p_Clip.bounds.center());
	// The pictures go into this board's asset table (identical ones are shared); copies refer to the new ids
	std::unordered_map<AssetId, AssetId> l_AssetMap;
	for (const ImageAsset& l_Asset : p_Clip.assets)
		l_AssetMap[l_Asset.id] = p_Document.addAsset(l_Asset);
	std::vector<std::unique_ptr<Object>> l_Sources;
	l_Sources.reserve(p_Clip.objects.size());
	for (const Object& l_Object : p_Clip.objects)
	{
		l_Sources.push_back(std::make_unique<Object>(l_Object));
		AssetId* l_Asset = nullptr;
		if (ImageData* l_Image = l_Sources.back()->image())
			l_Asset = &l_Image->asset;
		else if (VideoData* l_Video = l_Sources.back()->video())
			l_Asset = &l_Video->asset;
		if (l_Asset != nullptr)
		{
			if (const auto l_It = l_AssetMap.find(*l_Asset); l_It != l_AssetMap.end())
				*l_Asset = l_It->second;
		}
	}
	return insertCopies(p_Document, p_History, l_Sources, l_Shift, p_Name);
}

std::vector<ObjectId> duplicateObjects(Document& p_Document, History& p_History, const std::span<const ObjectId> p_Ids, const DVec2 p_Offset)
{
	std::vector<std::unique_ptr<Object>> l_Sources;
	for (const Object* l_Object : liveObjects(p_Document, p_Ids))
		l_Sources.push_back(std::make_unique<Object>(*l_Object));
	return insertCopies(p_Document, p_History, l_Sources, Affine2::translate(p_Offset), "Duplicate");
}

void deleteObjects(Document& p_Document, History& p_History, const std::span<const ObjectId> p_Ids)
{
	std::vector<ObjectId> l_Ids;
	for (const Object* l_Object : editableObjects(p_Document, p_Ids))
		l_Ids.push_back(l_Object->id);
	if (!l_Ids.empty())
		p_History.execute(p_Document, std::make_unique<RemoveObjectsCommand>(std::move(l_Ids), "Delete"));
}

void reorderObjects(Document& p_Document, History& p_History, const std::span<const ObjectId> p_Ids, const ZOrderMove p_Move)
{
	std::vector<ObjectId> l_Ids;
	for (const Object* l_Object : editableObjects(p_Document, p_Ids))
		l_Ids.push_back(l_Object->id);
	if (l_Ids.empty() || l_Ids.size() == p_Document.size())
		return;
	auto l_Command = std::make_unique<ReorderObjectsCommand>(std::move(l_Ids), p_Move, "Change order");
	l_Command->apply(p_Document);
	if (l_Command->changedOrder())
		p_History.push(std::move(l_Command));
}

void flipObjects(Document& p_Document, History& p_History, const std::span<const ObjectId> p_Ids, const bool p_Horizontal)
{
	Rect l_Bounds{};
	for (const Object* l_Object : editableObjects(p_Document, p_Ids))
		l_Bounds.expand(tightWorldBounds(*l_Object));
	if (l_Bounds.isEmpty())
		return;
	const Affine2 l_Mirror = Affine2::around(l_Bounds.center(), Affine2::scale(p_Horizontal ? DVec2{ -1.0, 1.0 } : DVec2{ 1.0, -1.0 }));
	transformObjects(p_Document, p_History, p_Ids, l_Mirror, p_Horizontal ? "Flip horizontally" : "Flip vertically", false);
}

void editImages(Document& p_Document, History& p_History, const std::span<const ObjectId> p_Ids, const char* p_Name, const std::function<bool(ImageData&)>& p_Edit)
{
	std::vector<SetImageDataCommand::Entry> l_Entries;
	for (const Object* l_Object : editableObjects(p_Document, p_Ids))
	{
		const ImageData* l_Image = l_Object->image();
		if (l_Image == nullptr)
			continue;
		ImageData l_After = *l_Image;
		if (p_Edit(l_After) && !(l_After == *l_Image))
			l_Entries.push_back(SetImageDataCommand::Entry{ .id = l_Object->id, .before = *l_Image, .after = l_After });
	}
	if (!l_Entries.empty())
		p_History.execute(p_Document, std::make_unique<SetImageDataCommand>(std::move(l_Entries), p_Name));
}

bool cropImage(Document& p_Document, History& p_History, const ObjectId p_Id, const Vec2 p_CropMin, const Vec2 p_CropMax)
{
	const Object* l_Object = p_Document.find(p_Id);
	const ImageData* l_Image = l_Object != nullptr ? l_Object->image() : nullptr;
	if (l_Image == nullptr || l_Object->locked)
		return false;
	constexpr float MIN_FRACTION = 0.005f;
	const Vec2 l_Min = glm::clamp(p_CropMin, Vec2{ 0.f }, Vec2{ 1.f - MIN_FRACTION });
	const Vec2 l_Max = glm::clamp(p_CropMax, l_Min + Vec2{ MIN_FRACTION }, Vec2{ 1.f });
	if (l_Min == l_Image->cropMin && l_Max == l_Image->cropMax)
		return false;

	// The whole picture keeps its place and scale on the board; only the window onto it moves
	const Rect l_Full = l_Image->fullLocalBounds();
	const DVec2 l_FullSize = l_Full.size();
	ImageData l_After = *l_Image;
	l_After.cropMin = l_Min;
	l_After.cropMax = l_Max;
	l_After.size = Vec2{ (DVec2{ l_Max } - DVec2{ l_Min }) * l_FullSize };
	const DVec2 l_Center = l_Full.min + (DVec2{ l_Min } + DVec2{ l_Max }) * 0.5 * l_FullSize;

	SetImageDataCommand::Entry l_Entry{ .id = p_Id, .before = *l_Image, .after = l_After };
	l_Entry.setTransform = true;
	l_Entry.transformBefore = l_Object->transform;
	l_Entry.transformAfter = l_Object->transform * Affine2::translate(l_Center);
	std::vector<SetImageDataCommand::Entry> l_Entries;
	l_Entries.push_back(l_Entry);
	p_History.execute(p_Document, std::make_unique<SetImageDataCommand>(std::move(l_Entries), "Crop picture"));
	return true;
}

void editVideos(Document& p_Document, History& p_History, const std::span<const ObjectId> p_Ids, const char* p_Name, const std::function<bool(VideoData&)>& p_Edit)
{
	std::vector<SetVideoDataCommand::Entry> l_Entries;
	for (const Object* l_Object : editableObjects(p_Document, p_Ids))
	{
		const VideoData* l_Video = l_Object->video();
		if (l_Video == nullptr)
			continue;
		VideoData l_After = *l_Video;
		if (p_Edit(l_After) && !(l_After == *l_Video))
			l_Entries.push_back(SetVideoDataCommand::Entry{ .id = l_Object->id, .before = *l_Video, .after = l_After });
	}
	if (!l_Entries.empty())
		p_History.execute(p_Document, std::make_unique<SetVideoDataCommand>(std::move(l_Entries), p_Name));
}

Affine2 anchoredTextTransform(const Affine2& p_Old, const Vec2 p_OldSize, const Vec2 p_NewSize, const TextAlign p_Align)
{
	const double l_Fraction = p_Align == TextAlign::Left ? 0.0 : (p_Align == TextAlign::Center ? 0.5 : 1.0);
	const DVec2 l_OldAnchor{ (l_Fraction - 0.5) * p_OldSize.x, -0.5 * p_OldSize.y };
	const DVec2 l_NewAnchor{ (l_Fraction - 0.5) * p_NewSize.x, -0.5 * p_NewSize.y };
	Affine2 l_Result = p_Old;
	l_Result.translation = p_Old.translation + p_Old.applyVector(l_OldAnchor - l_NewAnchor);
	return l_Result;
}

void editText(Document& p_Document, History& p_History, const ObjectId p_Id, const TextData& p_After, const char* p_Name)
{
	const Object* l_Object = p_Document.find(p_Id);
	const TextData* l_Before = l_Object != nullptr ? l_Object->text() : nullptr;
	if (l_Before == nullptr || l_Object->locked || *l_Before == p_After)
		return;
	std::vector<SetTextCommand::Entry> l_Entries;
	l_Entries.push_back(SetTextCommand::Entry{
		.id = p_Id,
		.before = *l_Before,
		.after = p_After,
		.transformBefore = l_Object->transform,
		.transformAfter = anchoredTextTransform(l_Object->transform, l_Before->size, p_After.size, p_After.align),
	});
	p_History.execute(p_Document, std::make_unique<SetTextCommand>(std::move(l_Entries), p_Name));
}

size_t smoothStrokes(Document& p_Document, History& p_History, const std::span<const ObjectId> p_Ids)
{
	std::vector<SetStrokePointsCommand::Entry> l_Entries;
	for (const Object* l_Object : editableObjects(p_Document, p_Ids))
	{
		const StrokeData* l_Stroke = l_Object->stroke();
		if (l_Stroke == nullptr)
			continue;
		std::vector<StrokePoint> l_After = smoothedPoints(l_Stroke->points, smoothingReach(*l_Stroke));
		if (!l_After.empty() && l_After != l_Stroke->points)
			l_Entries.push_back(SetStrokePointsCommand::Entry{ .id = l_Object->id, .before = l_Stroke->points, .after = std::move(l_After) });
	}
	const size_t l_Changed = l_Entries.size();
	if (l_Changed > 0)
		p_History.execute(p_Document, std::make_unique<SetStrokePointsCommand>(std::move(l_Entries), "Smooth strokes"));
	return l_Changed;
}

void lockObjects(Document& p_Document, History& p_History, const std::span<const ObjectId> p_Ids, const bool p_Locked)
{
	std::vector<ObjectId> l_Ids;
	for (const Object* l_Object : liveObjects(p_Document, p_Ids))
	{
		if (l_Object->locked != p_Locked)
			l_Ids.push_back(l_Object->id);
	}
	if (!l_Ids.empty())
		p_History.execute(p_Document, std::make_unique<SetLockedCommand>(std::move(l_Ids), p_Locked, p_Locked ? "Lock" : "Unlock"));
}

ObjectId groupObjects(Document& p_Document, History& p_History, const std::span<const ObjectId> p_Ids)
{
	const std::vector<const Object*> l_Objects = editableObjects(p_Document, p_Ids);
	if (l_Objects.size() < 2)
		return INVALID_OBJECT_ID;
	// Already exactly one group: nothing to do
	const ObjectId l_First = l_Objects.front()->group;
	if (l_First != INVALID_OBJECT_ID && std::ranges::all_of(l_Objects, [&](const Object* p_Object) { return p_Object->group == l_First; }))
		return l_First;

	const ObjectId l_Group = p_Document.allocateId();
	std::vector<SetGroupCommand::Entry> l_Entries;
	l_Entries.reserve(l_Objects.size());
	for (const Object* l_Object : l_Objects)
		l_Entries.push_back(SetGroupCommand::Entry{ .id = l_Object->id, .before = l_Object->group, .after = l_Group });
	p_History.execute(p_Document, std::make_unique<SetGroupCommand>(std::move(l_Entries), "Group"));
	return l_Group;
}

size_t ungroupObjects(Document& p_Document, History& p_History, const std::span<const ObjectId> p_Ids)
{
	std::vector<SetGroupCommand::Entry> l_Entries;
	for (const Object* l_Object : editableObjects(p_Document, p_Ids))
	{
		if (l_Object->group != INVALID_OBJECT_ID)
			l_Entries.push_back(SetGroupCommand::Entry{ .id = l_Object->id, .before = l_Object->group, .after = INVALID_OBJECT_ID });
	}
	const size_t l_Count = l_Entries.size();
	if (l_Count > 0)
		p_History.execute(p_Document, std::make_unique<SetGroupCommand>(std::move(l_Entries), "Ungroup"));
	return l_Count;
}

void alignObjects(Document& p_Document, History& p_History, const std::span<const ObjectId> p_Ids, const AlignMode p_Mode)
{
	const std::vector<const Object*> l_All = liveObjects(p_Document, p_Ids);
	if (l_All.size() < 2)
		return;
	struct Item
	{
		const Object* object = nullptr;
		Rect bounds{};
	};
	std::vector<Item> l_Items;
	Rect l_Total{};
	for (const Object* l_Object : l_All)
	{
		const Rect l_Bounds = tightWorldBounds(*l_Object);
		l_Total.expand(l_Bounds);
		if (!l_Object->locked)
			l_Items.push_back(Item{ .object = l_Object, .bounds = l_Bounds });
	}

	std::vector<DVec2> l_Shift(l_Items.size(), DVec2{ 0.0 });
	switch (p_Mode)
	{
	case AlignMode::Left:
		for (size_t i = 0; i < l_Items.size(); ++i)
			l_Shift[i].x = l_Total.min.x - l_Items[i].bounds.min.x;
		break;
	case AlignMode::Right:
		for (size_t i = 0; i < l_Items.size(); ++i)
			l_Shift[i].x = l_Total.max.x - l_Items[i].bounds.max.x;
		break;
	case AlignMode::CenterX:
		for (size_t i = 0; i < l_Items.size(); ++i)
			l_Shift[i].x = l_Total.center().x - l_Items[i].bounds.center().x;
		break;
	case AlignMode::Top:
		for (size_t i = 0; i < l_Items.size(); ++i)
			l_Shift[i].y = l_Total.min.y - l_Items[i].bounds.min.y;
		break;
	case AlignMode::Bottom:
		for (size_t i = 0; i < l_Items.size(); ++i)
			l_Shift[i].y = l_Total.max.y - l_Items[i].bounds.max.y;
		break;
	case AlignMode::CenterY:
		for (size_t i = 0; i < l_Items.size(); ++i)
			l_Shift[i].y = l_Total.center().y - l_Items[i].bounds.center().y;
		break;
	case AlignMode::DistributeX:
	case AlignMode::DistributeY:
	{
		if (l_Items.size() < 3)
			return;
		const int l_Axis = p_Mode == AlignMode::DistributeX ? 0 : 1;
		std::vector<size_t> l_Order(l_Items.size());
		for (size_t i = 0; i < l_Order.size(); ++i)
			l_Order[i] = i;
		std::ranges::stable_sort(l_Order, [&](const size_t p_A, const size_t p_B) { return l_Items[p_A].bounds.center()[l_Axis] < l_Items[p_B].bounds.center()[l_Axis]; });
		const double l_First = l_Items[l_Order.front()].bounds.min[l_Axis];
		const double l_Last = l_Items[l_Order.back()].bounds.max[l_Axis];
		double l_Sizes = 0.0;
		for (const Item& l_Item : l_Items)
			l_Sizes += l_Item.bounds.max[l_Axis] - l_Item.bounds.min[l_Axis];
		const double l_Gap = (l_Last - l_First - l_Sizes) / static_cast<double>(l_Items.size() - 1);
		double l_Cursor = l_First;
		for (const size_t l_Index : l_Order)
		{
			l_Shift[l_Index][l_Axis] = l_Cursor - l_Items[l_Index].bounds.min[l_Axis];
			l_Cursor += l_Items[l_Index].bounds.max[l_Axis] - l_Items[l_Index].bounds.min[l_Axis] + l_Gap;
		}
		break;
	}
	}

	std::vector<TransformObjectsCommand::Entry> l_Entries;
	for (size_t i = 0; i < l_Items.size(); ++i)
	{
		const Object& l_Object = *l_Items[i].object;
		l_Entries.push_back(TransformObjectsCommand::Entry{ .id = l_Object.id, .before = l_Object.transform, .after = Affine2::translate(l_Shift[i]) * l_Object.transform });
	}
	auto l_Command = std::make_unique<TransformObjectsCommand>(std::move(l_Entries), "Align");
	if (l_Command->changesAnything())
		p_History.execute(p_Document, std::move(l_Command));
}

bool isTilted(const Object& p_Object)
{
	const DMat2& l_Linear = p_Object.transform.linear;
	const double l_Scale = std::max(p_Object.transform.uniformScale(), 1e-300);
	const double l_Epsilon = 1e-9 * l_Scale;
	return std::abs(l_Linear[0].y) > l_Epsilon || std::abs(l_Linear[1].x) > l_Epsilon || l_Linear[0].x < 0.0 || l_Linear[1].y < 0.0 || std::abs(l_Linear[0].x - l_Linear[1].y) > 1e-6 * l_Scale;
}

void resetTransforms(Document& p_Document, History& p_History, const std::span<const ObjectId> p_Ids)
{
	std::vector<TransformObjectsCommand::Entry> l_Entries;
	for (const Object* l_Object : editableObjects(p_Document, p_Ids))
	{
		if (!isTilted(*l_Object))
			continue;
		const double l_Scale = l_Object->transform.uniformScale();
		const DVec2 l_LocalCenter = l_Object->localBounds().center();
		const DVec2 l_Center = l_Object->transform.apply(l_LocalCenter);
		Affine2 l_After;
		l_After.linear = DMat2{ l_Scale };
		l_After.translation = l_Center - l_After.linear * l_LocalCenter;
		l_Entries.push_back(TransformObjectsCommand::Entry{ .id = l_Object->id, .before = l_Object->transform, .after = l_After });
	}
	if (l_Entries.empty())
		return;
	auto l_Command = std::make_unique<TransformObjectsCommand>(std::move(l_Entries), "Reset transform", false);
	if (l_Command->changesAnything())
		p_History.execute(p_Document, std::move(l_Command));
}

void recolorObjects(Document& p_Document, History& p_History, const std::span<const ObjectId> p_Ids, const Color p_Color)
{
	std::vector<ObjectId> l_Ids;
	for (const Object* l_Object : editableObjects(p_Document, p_Ids))
	{
		const Color* l_Current = nullptr;
		bool l_Mixed = false; // a text with letters of other colours still changes
		if (const StrokeData* l_Stroke = l_Object->stroke())
		{
			l_Current = &l_Stroke->style.color;
		}
		else if (const TextData* l_Text = l_Object->text())
		{
			l_Current = &l_Text->color;
			l_Mixed = !l_Text->spans.empty();
		}
		if (l_Current != nullptr && (l_Mixed || !(l_Current->r == p_Color.r && l_Current->g == p_Color.g && l_Current->b == p_Color.b)))
			l_Ids.push_back(l_Object->id);
	}
	if (!l_Ids.empty())
		p_History.execute(p_Document, std::make_unique<SetStrokeColorCommand>(std::move(l_Ids), p_Color, "Change colour"));
}

void transformObjects(Document& p_Document, History& p_History, const std::span<const ObjectId> p_Ids, const Affine2& p_World, const char* p_Name, const bool p_Mergeable)
{
	std::vector<TransformObjectsCommand::Entry> l_Entries;
	for (const Object* l_Object : editableObjects(p_Document, p_Ids))
		l_Entries.push_back(TransformObjectsCommand::Entry{ .id = l_Object->id, .before = l_Object->transform, .after = p_World * l_Object->transform });
	if (l_Entries.empty())
		return;
	auto l_Command = std::make_unique<TransformObjectsCommand>(std::move(l_Entries), p_Name, p_Mergeable);
	if (!l_Command->changesAnything())
		return;
	p_History.execute(p_Document, std::move(l_Command));
}
} // namespace wb
