import Foundation
import Combine

final class SliceSettingsState: ObservableObject {
    enum QualityPreset: String, CaseIterable {
        case draft = "Draft"
        case balanced = "Balanced"
        case quality = "Quality"

        init(presetID: String) {
            switch presetID {
            case "draft":
                self = .draft
            case "quality":
                self = .quality
            default:
                self = .balanced
            }
        }

        var presetID: String {
            switch self {
            case .draft: return "draft"
            case .balanced: return "balanced"
            case .quality: return "quality"
            }
        }

        var serviceValue: String {
            presetID
        }
    }

    @Published var qualityPreset: QualityPreset = .balanced
    @Published var infillPercent: Int = 15
    @Published var supportsEnabled: Bool = false
    @Published var materialName: String = "PLA Basic"
    @Published var profileName: String = "0.20mm Standard"
    @Published var nozzleTemperatureC: Int = 220
    @Published var bedTemperatureC: Int = 60
    @Published var validationMessage: String = ""

    private let store: ProjectProfileStore
    private var cancellables: Set<AnyCancellable> = []

    init(store: ProjectProfileStore) {
        self.store = store
        loadFromProfileService()

        store.$sliceSettings
            .receive(on: DispatchQueue.main)
            .sink { [weak self] settings in
                self?.apply(settings)
            }
            .store(in: &cancellables)
    }

    func loadFromProfileService() {
        apply(store.sliceSettings)
    }

    func saveToProfileService() {
        let updated = SliceSettingsData(
            presetID: qualityPreset.presetID,
            infillPercent: infillPercent,
            supportsEnabled: supportsEnabled,
            materialName: materialName,
            profileName: profileName,
            nozzleTemperatureC: nozzleTemperatureC,
            bedTemperatureC: bedTemperatureC
        )

        do {
            try store.saveSliceSettings(updated)
            validationMessage = ""
        } catch {
            validationMessage = error.localizedDescription
        }
    }

    private func apply(_ settings: SliceSettingsData) {
        qualityPreset = QualityPreset(presetID: settings.presetID)
        infillPercent = settings.infillPercent
        supportsEnabled = settings.supportsEnabled
        materialName = settings.materialName
        profileName = settings.profileName
        nozzleTemperatureC = settings.nozzleTemperatureC
        bedTemperatureC = settings.bedTemperatureC
    }
}
