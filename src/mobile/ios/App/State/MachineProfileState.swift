import Foundation
import Combine

final class MachineProfileState: ObservableObject {
    @Published var printerName: String = "No printer selected"
    @Published var nozzleSummary: String = "0.4 mm nozzle"
    @Published var materialSummary: String = "PLA / Generic"
    @Published var lanMode: String = "Unknown"
    @Published var cloudSync: String = "Unknown"
    @Published var firmwareVersion: String = "Unknown"

    private var cancellables: Set<AnyCancellable> = []

    init(store: ProjectProfileStore) {
        store.$machineProfile
            .receive(on: DispatchQueue.main)
            .sink { [weak self] profile in
                self?.printerName = profile.printerName
                self?.nozzleSummary = profile.nozzleSummary
                self?.materialSummary = profile.materialSummary
                self?.lanMode = profile.lanConnectivity.title
                self?.cloudSync = profile.cloudSync.title
                self?.firmwareVersion = profile.firmwareVersion
            }
            .store(in: &cancellables)
    }
}
