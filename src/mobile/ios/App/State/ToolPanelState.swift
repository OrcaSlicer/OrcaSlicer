import Foundation

final class ToolPanelState: ObservableObject {
    enum ToolKind: CaseIterable {
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

    @Published var activeTool: ToolKind = .move
    @Published var snapToBed = true
    @Published var uniformScale = true
}
