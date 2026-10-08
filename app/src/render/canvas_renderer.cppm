// Draws the board: background grid, then every visible object back to front.
//
// Stroke points live in one device-local "point pool" buffer, sub-allocated per stroke and uploaded once
// (re-uploaded when the stroke changes). Each frame the renderer culls objects against the view and writes one
// instance record per visible stroke (local->screen transform, color, point range) into a per-frame buffer.
// The stroke being drawn is streamed every frame through a separate per-frame point buffer.
module;
#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <volk.h>

export module wb.render.canvas_renderer;

import wb.math;
import wb.doc.object;
import wb.doc.document;
import wb.view.camera;
import wb.util.range_allocator;
import wb.gfx.context;
import wb.gfx.frames;
import wb.gfx.buffer;

export namespace wb::render
{
// The stroke currently being drawn (points relative to origin, in world units)
struct LiveStrokeView
{
	DVec2 origin{ 0.0 };
	Color color{};
	std::span<const StrokePoint> committed;
	std::span<const StrokePoint> tail;
};

struct GridStyle
{
	bool enabled = true;
	Color dotColor{};
};

struct CanvasStats
{
	uint32_t gpuStrokes = 0;
	uint32_t visibleStrokes = 0;
	uint64_t drawnSegments = 0;
	uint64_t poolPointsUsed = 0;
	uint64_t poolPointsCapacity = 0;
	uint32_t uploadsThisFrame = 0;
};

class CanvasRenderer final : public DocumentListener
{
public:
	void init(const gfx::GraphicsContext& p_Context, VkFormat p_ColorFormat);
	void destroy(const gfx::GraphicsContext& p_Context);

	// Starts mirroring p_Document on the GPU (uploads everything it already contains)
	void attach(Document& p_Document);
	void detach();

	// Outside a rendering scope: uploads pending strokes and builds this frame's instances
	void prepare(const gfx::GraphicsContext& p_Context, gfx::FrameScheduler& p_Frames, gfx::StagingBelt& p_Staging, VkCommandBuffer p_Cmd, uint32_t p_Slot, const Camera& p_Camera, const LiveStrokeView* p_Live);
	// Inside the rendering scope of the frame target
	void record(VkCommandBuffer p_Cmd, const Camera& p_Camera, const GridStyle& p_Grid) const;

	[[nodiscard]] const CanvasStats& stats() const { return m_Stats; }

	// DocumentListener
	void onObjectAdded(const Object& p_Object) override;
	void onObjectRemoved(const Object& p_Object) override;
	void onObjectChanged(const Object& p_Object, uint32_t p_Changes) override;
	void onDocumentCleared() override;

private:
	struct GpuStroke
	{
		uint64_t offset = 0; // in points
		uint32_t count = 0;
	};

	struct PendingFree
	{
		uint64_t afterTimeline = 0; // safe once the GPU has completed this timeline value
		uint64_t offset = 0;
		uint64_t count = 0;
	};

	struct Draw
	{
		uint32_t instance = 0;
		uint32_t vertexCount = 0;
	};

	struct FrameResources
	{
		gfx::Buffer instances;
		gfx::Buffer livePoints;
		VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
	};

	void createPipelines(const gfx::GraphicsContext& p_Context, VkFormat p_ColorFormat);
	void releaseRange(uint64_t p_Offset, uint64_t p_Count);
	void processFrees(uint64_t p_CompletedTimeline);
	void uploadPending(const gfx::GraphicsContext& p_Context, gfx::FrameScheduler& p_Frames, gfx::StagingBelt& p_Staging, VkCommandBuffer p_Cmd);
	void ensurePoolCapacity(const gfx::GraphicsContext& p_Context, gfx::FrameScheduler& p_Frames, VkCommandBuffer p_Cmd, uint64_t p_ExtraPoints);
	static void ensureCapacity(const gfx::GraphicsContext& p_Context, gfx::FrameScheduler& p_Frames, gfx::Buffer& p_Buffer, VkDeviceSize p_Bytes, const char* p_Name);

	Document* m_Document = nullptr;
	std::unordered_map<ObjectId, GpuStroke> m_Strokes;
	std::unordered_set<ObjectId> m_PendingUploads;
	std::vector<PendingFree> m_PendingFrees;
	uint64_t m_CurrentTimeline = 0; // value the next submitted frame will signal

	gfx::Buffer m_PointPool;
	RangeAllocator m_PoolAllocator;
	uint64_t m_MaxPoolPoints = 0;

	std::array<FrameResources, gfx::FRAMES_IN_FLIGHT> m_Frames{};
	uint32_t m_Slot = 0;
	std::vector<Draw> m_Draws;

	VkDescriptorSetLayout m_SetLayout = VK_NULL_HANDLE;
	VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
	VkPipelineLayout m_StrokeLayout = VK_NULL_HANDLE;
	VkPipelineLayout m_GridLayout = VK_NULL_HANDLE;
	VkPipeline m_StrokePipeline = VK_NULL_HANDLE;
	VkPipeline m_GridPipeline = VK_NULL_HANDLE;

	CanvasStats m_Stats{};
};
} // namespace wb::render
