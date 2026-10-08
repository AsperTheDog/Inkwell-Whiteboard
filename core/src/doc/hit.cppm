// Geometry queries on objects: tight bounds, point picking and overlap with rectangles / lassos.
// Everything is evaluated in world space so transformed objects behave exactly as they are drawn.
module;
#include <span>
#include <vector>

export module wb.doc.hit;

import wb.math;
import wb.doc.object;
import wb.doc.document;

export namespace wb
{
// Bounds of the geometry itself (a stroke: every point plus its radius); tighter than Object::worldBounds()
// once an object is rotated
[[nodiscard]] Rect tightWorldBounds(const Object& p_Object);

// True when p_World lies within p_Tolerance (world units) of the object's ink
[[nodiscard]] bool hitsPoint(const Object& p_Object, DVec2 p_World, double p_Tolerance);

// The topmost object whose ink is within p_Tolerance of p_World; INVALID_OBJECT_ID when there is none
[[nodiscard]] ObjectId pickTopmost(const Document& p_Document, DVec2 p_World, double p_Tolerance);

[[nodiscard]] bool pointInPolygon(DVec2 p_Point, std::span<const DVec2> p_Polygon);

// True when the object's ink overlaps the closed polygon (partial overlap counts)
[[nodiscard]] bool touchesPolygon(const Object& p_Object, std::span<const DVec2> p_Polygon);

// Objects whose ink overlaps the area, back to front
[[nodiscard]] std::vector<ObjectId> objectsInPolygon(const Document& p_Document, std::span<const DVec2> p_Polygon);
[[nodiscard]] std::vector<ObjectId> objectsInRect(const Document& p_Document, const Rect& p_Area);
} // namespace wb
