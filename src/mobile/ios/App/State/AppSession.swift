import Foundation

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

    @Published var recentProjectNames: [String] = ["Benchy.3mf", "PhoneStand.stl"]
    @Published var activeProjectName: String = "No model loaded"
    @Published var lastActionStatus: String = ""
    @Published var pickerImportPresented = false
    @Published var exportTargetPickerPresented = false
    @Published var exportShareSheetPresented = false
    @Published var exportShareArtifactURL: URL?
    @Published var userMessage: UserFacingMessage?
    @Published var lastExportResult: ExportResultMetadata?

    let buildSummary: String = "iOS shell scaffold"

    private let fileAccessService: ProjectFileAccessServicing
    private var recentProjectURLs: [String: URL] = [:]

    init(fileAccessService: ProjectFileAccessServicing = LocalProjectFileAccessService()) {
        self.fileAccessService = fileAccessService
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
        updateRecentProjects(with: fileName)
        activeProjectName = fileName
        lastActionStatus = "Imported model: \(fileName)"
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

        updateRecentProjects(with: fileName)
        activeProjectName = fileName
        lastActionStatus = "Opened recent project: \(fileName)"
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
        importPickedDocument(fileURL)
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
