// Turns local Document mutations into collaboration ops and applies remote ops without echoing them back.
module;
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>
#include <span>
#include <utility>

export module wb.net.replicator;

import wb.doc.object;
import wb.doc.document;
import wb.net.ops;

export namespace wb::net
{
class Replicator final : public DocumentListener
{
public:
	using Sink = std::function<void(Op)>;
	// Called after a remote op changed (p_Removed == false) or deleted (true) objects, so the local undo history can
	// drop what would overwrite the peer's work (History::invalidate)
	using RemoteChangeHandler = std::function<void(std::span<const ObjectId>, bool p_Removed)>;

	Replicator(Document& p_Document, Sink p_Sink);
	~Replicator() override;
	Replicator(const Replicator&) = delete;
	Replicator& operator=(const Replicator&) = delete;

	void setRemoteChangeHandler(RemoteChangeHandler p_Handler) { m_OnRemoteChange = std::move(p_Handler); }

	// Applies an op from a peer. The document listeners see it as usual, but it is not sent on.
	ApplyResult applyRemote(const Op& p_Op);

	// While any Pause exists local mutations are not turned into ops (loading a snapshot, for example)
	class Pause
	{
	public:
		explicit Pause(Replicator& p_Replicator) : m_Replicator(p_Replicator) { ++m_Replicator.m_Paused; }
		~Pause() { --m_Replicator.m_Paused; }
		Pause(const Pause&) = delete;
		Pause& operator=(const Pause&) = delete;

	private:
		Replicator& m_Replicator;
	};

	void onObjectAdded(const Object& p_Object) override;
	void onObjectRemoved(const Object& p_Object) override;
	void onObjectChanged(const Object& p_Object, uint32_t p_Changes) override;
	void onObjectsReordered() override;
	void onDocumentCleared() override;

private:
	void emit(Op p_Op)
	{
		if (m_Paused == 0)
			m_Sink(std::move(p_Op));
	}

	Document& m_Document;
	Sink m_Sink;
	RemoteChangeHandler m_OnRemoteChange;
	int m_Paused = 0;
	// The shared form of each video as the others last got it from here (to tell a volume change from a real one)
	std::unordered_map<ObjectId, std::vector<uint8_t>> m_VideoSent;
};
} // namespace wb::net
