import Foundation

final class SliceSettingsState: ObservableObject {
    enum QualityPreset: String, CaseIterable {
        case draft = "Draft"
        case balanced = "Balanced"
        case quality = "Quality"
    }

    @Published var qualityPreset: QualityPreset = .balanced
    @Published var infillPercent: Int = 15
    @Published var supportsEnabled: Bool = false
    @Published var materialName: String = "PLA Basic"
    @Published var profileName: String = "0.20mm Standard"
}
