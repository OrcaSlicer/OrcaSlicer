import Foundation
import Combine

enum SliceRunState {
    case idle
    case running
    case success
    case failure
}

final class AppSession: ObservableObject {
    struct UserFacingMessage: Identifiable {
        let id = UUID()
        let title: String
        let detail: String
    }

    enum ExportTarget: String, CaseIterable, Identifiable {
        case generic = "Generic profile"
        case quality = "Quality profile"
        case speed = "Speed profile"

        var id: String { rawValue }

        var fileNameSuffix: String {
            switch self {
            case .generic:
                return "generic"
            case .quality:
                return "quality"
            case .speed:
                return "speed"
            }
        }
    }

    struct ExportResultMetadata {
        let sourceProjectName: String
        let target: ExportTarget
        let artifactURL: URL
        let exportedAt: Date
    }

    @Published var recentProjectNames: [String]
    @Published var activeProjectName: String
    @Published var lastActionStatus: String
    @Published var pickerImportPresented = false
    @Published var exportTargetPickerPresented = false
    @Published var exportShareSheetPresented = false
    @Published var exportShareArtifactURL: URL?
    @Published var userMessage: UserFacingMessage?
    @Published var lastExportResult: ExportResultMetadata?

    @Published var slicingState: SliceRunState = .idle
    @Published var slicingProgressPercent: Double = 0
    @Published var slicingMessage: String = ""
    @Published var slicingDiagnosticsLog: String = ""

    @Published private(set) var buildSummary: String
    @Published private(set) var runtimeSummary: String
    @Published private(set) var renderBackend: String
    @Published private(set) var backendVersion: String
    @Published private(set) var portabilityStatus: String
    @Published private(set) var debugLogsSummary: String

    private let fileAccessService: ProjectFileAccessServicing
    private let store: ProjectProfileStore
    private var recentProjectURLs: [String: URL] = [:]
    private var cancellables: Set<AnyCancellable> = []

    var isSliceRunning: Bool {
        slicingState == .running
    }

    init(
        store: ProjectProfileStore,
        fileAccessService: ProjectFileAccessServicing = LocalProjectFileAccessService()
    ) {
        self.store = store
        self.fileAccessService = fileAccessService
        NSLog("AppSession.init store=%p fileAccessService=%@", Unmanaged.passUnretained(store).toOpaque(), String(describing: type(of: fileAccessService)))

        let diagnostics = store.diagnostics
        buildSummary = diagnostics.buildSummary
        runtimeSummary = diagnostics.runtimeSummary
        renderBackend = diagnostics.renderBackend
        backendVersion = diagnostics.backendVersion
        portabilityStatus = diagnostics.portabilityAPI
        debugLogsSummary = diagnostics.debugLogs

        recentProjectNames = ["Benchy.3mf", "PhoneStand.stl"]
        activeProjectName = "No model loaded"
        lastActionStatus = "Ready"

        bindDiagnostics()
        if let firstRecent = recentProjectNames.first {
            store.hydrateFromRecentProjectName(firstRecent)
        }
    }

    func beginSlicing(message: String) {
        slicingState = .running
        slicingProgressPercent = 0
        slicingMessage = message
        slicingDiagnosticsLog = ""
        lastActionStatus = message
    }

    func updateSlicingProgress(percent: Double, message: String) {
        slicingProgressPercent = max(0, min(percent, 100))
        slicingMessage = message
        lastActionStatus = "Slicing: \(Int(slicingProgressPercent))%"
    }

    func completeSlicingSuccessfully(summary: String, diagnosticsLog: String) {
        slicingState = .success
        slicingProgressPercent = 100
        slicingMessage = summary
        slicingDiagnosticsLog = diagnosticsLog
        lastActionStatus = summary
    }

    func failSlicing(message: String, diagnosticsLog: String) {
        slicingState = .failure
        slicingMessage = message
        slicingDiagnosticsLog = diagnosticsLog
        lastActionStatus = message
    }

    func cancelSlicing() {
        slicingState = .idle
        slicingProgressPercent = 0
        slicingMessage = "Slicing cancelled"
        lastActionStatus = "Slicing cancelled"
    }

    func beginImportFlow() {
        pickerImportPresented = true
    }

    func beginExportFlow() {
        guard activeProjectName != "No model loaded" else {
            presentFailure(title: "Export unavailable", detail: "Import or open a project before exporting G-code.")
            return
        }

        exportTargetPickerPresented = true
    }

    func importPickedDocument(_ fileURL: URL) {
        guard fileAccessService.isSupportedModelFile(fileURL) else {
            presentFailure(title: "Unsupported model", detail: "Only STL, 3MF, OBJ, STEP, and AMF files are supported.")
            return
        }

        let fileName = fileAccessService.displayName(for: fileURL)
        recentProjectURLs[fileName] = fileURL
        activateProject(named: fileName, statusPrefix: "Imported model")
    }

    func openRecentProject(named fileName: String) {
        guard let fileURL = recentProjectURLs[fileName] else {
            presentFailure(title: "Cannot open recent project", detail: "The saved path for \(fileName) is unavailable. Re-import the file.")
            return
        }

        guard fileAccessService.fileExists(at: fileURL) else {
            recentProjectURLs[fileName] = nil
            recentProjectNames.removeAll { $0 == fileName }
            presentFailure(title: "Recent file missing", detail: "\(fileName) is no longer on disk.")
            return
        }

        activateProject(named: fileName, statusPrefix: "Opened recent project")
    }

    func runExport(target: ExportTarget) {
        let sourceProject = activeProjectName
        let sourceStem = URL(fileURLWithPath: sourceProject).deletingPathExtension().lastPathComponent
        let normalizedStem = sourceStem.isEmpty ? "project" : sourceStem
        let fileName = "\(normalizedStem)-\(target.fileNameSuffix).gcode"
        let payload = generatePlaceholderGcode(for: sourceProject, target: target)

        do {
            let artifactURL = try fileAccessService.createExportArtifact(fileName: fileName, gcodePayload: payload)
            exportShareArtifactURL = artifactURL
            exportShareSheetPresented = true
            lastExportResult = ExportResultMetadata(
                sourceProjectName: sourceProject,
                target: target,
                artifactURL: artifactURL,
                exportedAt: Date()
            )
            lastActionStatus = "Exported \(artifactURL.lastPathComponent)"
        } catch {
            presentFailure(title: "Export failed", detail: error.localizedDescription)
        }
    }

    func handleSharedFile(_ fileURL: URL) {
        guard fileAccessService.isSupportedModelFile(fileURL) else {
            let fileName = fileAccessService.displayName(for: fileURL)
            presentFailure(title: "Unsupported shared file", detail: "\(fileName) is not a supported model type.")
            return
        }

        let fileName = fileAccessService.displayName(for: fileURL)
        recentProjectURLs[fileName] = fileURL
        activateProject(named: fileName, statusPrefix: "Imported shared file")
    }

    private func bindDiagnostics() {
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
    }

    private func activateProject(named fileName: String, statusPrefix: String) {
        updateRecentProjects(with: fileName)
        activeProjectName = fileName
        store.hydrateFromRecentProjectName(fileName)
        lastActionStatus = "\(statusPrefix): \(fileName)"
    }

    private func updateRecentProjects(with fileName: String) {
        recentProjectNames.removeAll { $0.caseInsensitiveCompare(fileName) == .orderedSame }
        recentProjectNames.insert(fileName, at: 0)

        if recentProjectNames.count > 10 {
            recentProjectNames = Array(recentProjectNames.prefix(10))
        }
    }

    private func presentFailure(title: String, detail: String) {
        userMessage = UserFacingMessage(title: title, detail: detail)
        lastActionStatus = detail
    }

    private func generatePlaceholderGcode(for projectName: String, target: ExportTarget) -> String {
        [
            "; OrcaSlicer iOS placeholder export",
            "; source=\(projectName)",
            "; target=\(target.rawValue)",
            "G21",
            "G90",
            "M82",
            "G28",
            "G1 Z0.28 F1200",
            "G1 X10 Y10 F1800",
            "M84"
        ].joined(separator: "\n")
    }
}
