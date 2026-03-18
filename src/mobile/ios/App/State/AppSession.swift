import Foundation
import Combine

final class AppSession: ObservableObject {
    @Published var recentProjectNames: [String] = ["Benchy.3mf", "PhoneStand.stl"]
    @Published var lastActionStatus: String = ""

    @Published private(set) var buildSummary: String
    @Published private(set) var runtimeSummary: String
    @Published private(set) var renderBackend: String
    @Published private(set) var backendVersion: String
    @Published private(set) var portabilityStatus: String
    @Published private(set) var debugLogsSummary: String

    private let supportedSharedExtensions: Set<String> = ["3mf", "stl", "obj", "step", "stp", "amf"]
    private let store: ProjectProfileStore
    private var cancellables: Set<AnyCancellable> = []

    init(store: ProjectProfileStore) {
        self.store = store
        buildSummary = store.diagnostics.buildSummary
        runtimeSummary = store.diagnostics.runtimeSummary
        renderBackend = store.diagnostics.renderBackend
        backendVersion = store.diagnostics.backendVersion
        portabilityStatus = store.diagnostics.portabilityAPI
        debugLogsSummary = store.diagnostics.debugLogs

        store.$diagnostics
            .receive(on: DispatchQueue.main)
            .sink { [weak self] diagnostics in
                self?.buildSummary = diagnostics.buildSummary
                self?.runtimeSummary = diagnostics.runtimeSummary
                self?.renderBackend = diagnostics.renderBackend
                self?.backendVersion = diagnostics.backendVersion
                self?.portabilityStatus = diagnostics.portabilityAPI
                self?.debugLogsSummary = diagnostics.debugLogs
            }
            .store(in: &cancellables)

        store.hydrateFromRecentProjectName(recentProjectNames.first)
    }

    func handleSharedFile(_ fileURL: URL) {
        let fileName = fileURL.lastPathComponent
        let fileExtension = fileURL.pathExtension.lowercased()

        guard supportedSharedExtensions.contains(fileExtension) else {
            lastActionStatus = "Unsupported shared file: \(fileName)"
            return
        }

        recentProjectNames.removeAll { $0.caseInsensitiveCompare(fileName) == .orderedSame }
        recentProjectNames.insert(fileName, at: 0)

        if recentProjectNames.count > 10 {
            recentProjectNames = Array(recentProjectNames.prefix(10))
        }

        store.hydrateFromRecentProjectName(fileName)
        lastActionStatus = "Imported shared file: \(fileName)"
    }
}
