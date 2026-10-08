module;
#include <memory>
#include <span>
#include <string>
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
	for (const Object* l_Object : liveObjects(p_Document, p_Ids))
	{
		l_Clip.objects.push_back(*l_Object);
		l_Clip.bounds.expand(tightWorldBounds(*l_Object));
	}
	return l_Clip;
}

std::vector<ObjectId> pasteObjects(Document& p_Document, History& p_History, const ObjectClip& p_Clip, const DVec2 p_Center, const char* p_Name)
{
	if (p_Clip.empty())
		return {};
	const Affine2 l_Shift = Affine2::translate(p_Center - p_Clip.bounds.center());
	std::vector<std::unique_ptr<Object>> l_Sources;
	l_Sources.reserve(p_Clip.objects.size());
	for (const Object& l_Object : p_Clip.objects)
		l_Sources.push_back(std::make_unique<Object>(l_Object));
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

void recolorObjects(Document& p_Document, History& p_History, const std::span<const ObjectId> p_Ids, const Color p_Color)
{
	std::vector<ObjectId> l_Ids;
	for (const Object* l_Object : liveObjects(p_Document, p_Ids))
	{
		if (const StrokeData* l_Stroke = l_Object->stroke(); l_Stroke != nullptr && !(l_Stroke->style.color.r == p_Color.r && l_Stroke->style.color.g == p_Color.g && l_Stroke->style.color.b == p_Color.b))
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
