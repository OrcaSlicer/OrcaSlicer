import Foundation

enum PanelKind: String, Identifiable {
    case project
    case tools
    case sliceSettings
    case printer
    case view
    case appSettings

    var id: String { rawValue }
}

final class PanelRouter: ObservableObject {
    @Published var presentedPanel: PanelKind?

    func present(_ panel: PanelKind) {
        presentedPanel = panel
    }

    func dismiss() {
        presentedPanel = nil
    }
}
