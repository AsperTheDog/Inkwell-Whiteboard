module;
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

std::vector<ObjectId> insertCopies(Document& p_Document, History& p_History, const std::vector<std::unique_ptr<Object>>& p_Sources, const Affine2& p_Shift, const char* p_Name)
{
	std::vector<std::unique_ptr<Object>> l_Copies;
	std::vector<ObjectId> l_Ids;
	l_Copies.reserve(p_Sources.size());
	for (const std::unique_ptr<Object>& l_Source : p_Sources)
	{
		std::unique_ptr<Object> l_Copy = cloneObject(*l_Source, p_Document.allocateId());
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
		if (const ImageData* l_Image = l_Object->image(); l_Image != nullptr && l_Assets.insert(l_Image->asset).second)
		{
			if (const ImageAsset* l_Asset = p_Document.findAsset(l_Image->asset))
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
		if (ImageData* l_Image = l_Sources.back()->image())
		{
			if (const auto l_It = l_AssetMap.find(l_Image->asset); l_It != l_AssetMap.end())
				l_Image->asset = l_It->second;
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
	for (const Object* l_Object : liveObjects(p_Document, p_Ids))
		l_Ids.push_back(l_Object->id);
	if (!l_Ids.empty())
		p_History.execute(p_Document, std::make_unique<RemoveObjectsCommand>(std::move(l_Ids), "Delete"));
}

void reorderObjects(Document& p_Document, History& p_History, const std::span<const ObjectId> p_Ids, const ZOrderMove p_Move)
{
	std::vector<ObjectId> l_Ids;
	for (const Object* l_Object : liveObjects(p_Document, p_Ids))
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
	for (const Object* l_Object : liveObjects(p_Document, p_Ids))
		l_Bounds.expand(tightWorldBounds(*l_Object));
	if (l_Bounds.isEmpty())
		return;
	const Affine2 l_Mirror = Affine2::around(l_Bounds.center(), Affine2::scale(p_Horizontal ? DVec2{ -1.0, 1.0 } : DVec2{ 1.0, -1.0 }));
	transformObjects(p_Document, p_History, p_Ids, l_Mirror, p_Horizontal ? "Flip horizontally" : "Flip vertically", false);
}

void editImages(Document& p_Document, History& p_History, const std::span<const ObjectId> p_Ids, const char* p_Name, const std::function<bool(ImageData&)>& p_Edit)
{
	std::vector<SetImageDataCommand::Entry> l_Entries;
	for (const Object* l_Object : liveObjects(p_Document, p_Ids))
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
	if (l_Before == nullptr || *l_Before == p_After)
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
	for (const Object* l_Object : liveObjects(p_Document, p_Ids))
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
	for (const Object* l_Object : liveObjects(p_Document, p_Ids))
	{
		const Color* l_Current = nullptr;
		if (const StrokeData* l_Stroke = l_Object->stroke())
			l_Current = &l_Stroke->style.color;
		else if (const TextData* l_Text = l_Object->text())
			l_Current = &l_Text->color;
		if (l_Current != nullptr && !(l_Current->r == p_Color.r && l_Current->g == p_Color.g && l_Current->b == p_Color.b))
			l_Ids.push_back(l_Object->id);
	}
	if (!l_Ids.empty())
		p_History.execute(p_Document, std::make_unique<SetStrokeColorCommand>(std::move(l_Ids), p_Color, "Change colour"));
}

void transformObjects(Document& p_Document, History& p_History, const std::span<const ObjectId> p_Ids, const Affine2& p_World, const char* p_Name, const bool p_Mergeable)
{
	std::vector<TransformObjectsCommand::Entry> l_Entries;
	for (const Object* l_Object : liveObjects(p_Document, p_Ids))
		l_Entries.push_back(TransformObjectsCommand::Entry{ .id = l_Object->id, .before = l_Object->transform, .after = p_World * l_Object->transform });
	if (l_Entries.empty())
		return;
	auto l_Command = std::make_unique<TransformObjectsCommand>(std::move(l_Entries), p_Name, p_Mergeable);
	if (!l_Command->changesAnything())
		return;
	p_History.execute(p_Document, std::move(l_Command));
}
} // namespace wb
