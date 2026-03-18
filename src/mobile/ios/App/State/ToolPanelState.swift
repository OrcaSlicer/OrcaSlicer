import Foundation
import Combine

final class ToolPanelState: ObservableObject {
    enum ToolKind: String, CaseIterable {
        case move
        case rotate
        case scale
        case arrange
        case supports

        var title: String {
            switch self {
            case .move: return "Move"
            case .rotate: return "Rotate"
            case .scale: return "Scale"
            case .arrange: return "Arrange"
            case .supports: return "Supports"
            }
        }
    }

    @Published var activeTool: ToolKind = .move {
        didSet {
            guard !isHydratingFromStore, oldValue != activeTool else { return }
            store.updateToolPanel(activeToolID: activeTool.rawValue)
        }
    }

    @Published var snapToBed = true {
        didSet {
            guard !isHydratingFromStore, oldValue != snapToBed else { return }
            store.updateToolPanel(snapToBed: snapToBed)
        }
    }

    @Published var uniformScale = true {
        didSet {
            guard !isHydratingFromStore, oldValue != uniformScale else { return }
            store.updateToolPanel(uniformScale: uniformScale)
        }
    }

    private let store: ProjectProfileStore
    private var cancellables: Set<AnyCancellable> = []
    private var isHydratingFromStore = false

    init(store: ProjectProfileStore) {
        self.store = store
        bindStore()
    }

    private func bindStore() {
        store.$toolPanel
            .receive(on: DispatchQueue.main)
            .sink { [weak self] panel in
                guard let self else { return }
                self.isHydratingFromStore = true
                self.activeTool = ToolKind(rawValue: panel.activeToolID) ?? .move
                self.snapToBed = panel.snapToBed
                self.uniformScale = panel.uniformScale
                self.isHydratingFromStore = false
            }
            .store(in: &cancellables)
    }
}
