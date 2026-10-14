import Foundation

/// Screen placement is independent of the instance selected for controls/logs.
/// It contains identities only and never owns, starts, or stops a session.
nonisolated struct SimulatorDisplaySelection: Equatable {
    private(set) var primaryID: UUID?
    private(set) var pinnedIDs: [UUID] = []

    init(primaryID: UUID? = nil) { self.primaryID = primaryID }

    var visibleIDs: [UUID] { (primaryID.map { [$0] } ?? []) + pinnedIDs }

    /// Keep placements across stop/restart, but do not allocate a grid cell to
    /// a detached instance that has neither a running session nor an image.
    func displayedIDs(among displayableIDs: Set<UUID>) -> [UUID] {
        visibleIDs.filter { displayableIDs.contains($0) }
    }

    mutating func reconcile(availableIDs: Set<UUID>) {
        if let primaryID, !availableIDs.contains(primaryID) { self.primaryID = nil }
        var seen = Set<UUID>()
        pinnedIDs = pinnedIDs.filter { availableIDs.contains($0) && $0 != primaryID && seen.insert($0).inserted }
    }

    mutating func select(_ id: UUID?, availableIDs: Set<UUID>, displayableIDs: Set<UUID>? = nil) {
        reconcile(availableIDs: availableIDs)
        guard let id, availableIDs.contains(id), displayableIDs?.contains(id) ?? true,
              !visibleIDs.contains(id) else { return }
        primaryID = id
    }

    @discardableResult mutating func pin(_ id: UUID, availableIDs: Set<UUID>) -> Bool {
        reconcile(availableIDs: availableIDs)
        guard availableIDs.contains(id), !pinnedIDs.contains(id) else { return false }
        if primaryID == id { primaryID = nil }
        pinnedIDs.append(id)
        return true
    }

    @discardableResult mutating func closePinned(_ id: UUID, availableIDs: Set<UUID>) -> Bool {
        reconcile(availableIDs: availableIDs)
        guard pinnedIDs.contains(id) else { return false }
        pinnedIDs.removeAll { $0 == id }
        return true
    }

    /// Closing a restored primary has the same display-only effect as closing
    /// an automatically added or dragged card. It never changes selection/runtime.
    @discardableResult mutating func close(_ id: UUID, availableIDs: Set<UUID>) -> Bool {
        reconcile(availableIDs: availableIDs)
        guard visibleIDs.contains(id) else { return false }
        if primaryID == id { primaryID = nil }
        pinnedIDs.removeAll { $0 == id }
        return true
    }

    /// Resolve input by the card identity, never by the current sidebar choice.
    func inputTarget(for cardID: UUID, availableIDs: Set<UUID>) -> UUID? {
        availableIDs.contains(cardID) && visibleIDs.contains(cardID) ? cardID : nil
    }
}

/// Internal mouse dragging carries identities only. The workspace nonce and
/// source window bind a gesture to its own instance manager.
nonisolated struct SimulatorInstanceDragPayload: Codable, Equatable {
    let workspaceID: UUID
    let instanceID: UUID

    func encoded() -> Data? { try? JSONEncoder().encode(self) }

    static func instanceID(from data: Data, workspaceID: UUID, availableIDs: Set<UUID>) -> UUID? {
        guard data.count <= 256,
              let payload = try? JSONDecoder().decode(Self.self, from: data),
              payload.workspaceID == workspaceID, availableIDs.contains(payload.instanceID) else { return nil }
        return payload.instanceID
    }
}

/// Event coordinates are authoritative for an application-local drag. No
/// pasteboard, global pointer position or native dragging session is needed.
nonisolated struct SimulatorInstanceDragGesture {
    let payload: SimulatorInstanceDragPayload
    let sourceWindowNumber: Int
    let start: CGPoint
    private(set) var isDragging = false
    private(set) var isCancelled = false

    init?(payload: SimulatorInstanceDragPayload, sourceWindowNumber: Int, start: CGPoint) {
        guard sourceWindowNumber > 0, start.x.isFinite, start.y.isFinite else { return nil }
        self.payload = payload; self.sourceWindowNumber = sourceWindowNumber; self.start = start
    }

    @discardableResult mutating func update(locationInWindow point: CGPoint, windowNumber: Int) -> Bool {
        guard !isCancelled, windowNumber == sourceWindowNumber, point.x.isFinite, point.y.isFinite else {
            cancel()
            return false
        }
        if hypot(point.x - start.x, point.y - start.y) >= 4 { isDragging = true }
        return true
    }

    mutating func cancel() { isCancelled = true }

    func targetInstanceID(workspaceID: UUID, windowNumber: Int, bounds: CGRect,
                          locationInTarget point: CGPoint, displayableIDs: Set<UUID>) -> UUID? {
        guard isDragging, !isCancelled, windowNumber == sourceWindowNumber,
              payload.workspaceID == workspaceID, displayableIDs.contains(payload.instanceID),
              bounds.origin.x.isFinite, bounds.origin.y.isFinite,
              bounds.width.isFinite, bounds.height.isFinite, bounds.width > 0, bounds.height > 0,
              point.x.isFinite, point.y.isFinite, bounds.contains(point) else { return nil }
        return payload.instanceID
    }
}

/// Use the display column's actual width; the grid never requests a horizontal
/// scroll range. Canvas uses this proportional size but retains guest geometry.
nonisolated struct SimulatorDisplayGridLayout: Equatable {
    let columns: Int
    let columnWidth: CGFloat
    let horizontalInset: CGFloat
    let spacing: CGFloat = 20

    init(availableWidth: CGFloat, itemCount: Int) {
        let width = availableWidth.isFinite ? max(0, availableWidth) : 0
        horizontalInset = min(16, width / 4)
        let contentWidth = max(0, width - horizontalInset * 2)
        let fitting = Int(floor((contentWidth + spacing) / (212 + spacing)))
        columns = max(1, min(max(1, itemCount), fitting))
        columnWidth = max(0, (contentWidth - CGFloat(columns - 1) * spacing) / CGFloat(columns))
    }

    func canvasSize(_ native: CGSize) -> CGSize {
        guard native.width.isFinite, native.height.isFinite, native.width > 0, native.height > 0 else { return .zero }
        let scale = min(1, columnWidth / native.width)
        return CGSize(width: native.width * scale, height: native.height * scale)
    }
}
