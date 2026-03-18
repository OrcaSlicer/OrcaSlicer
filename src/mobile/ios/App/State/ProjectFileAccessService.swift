import Foundation

protocol ProjectFileAccessServicing {
    var supportedModelExtensions: Set<String> { get }

    func isSupportedModelFile(_ fileURL: URL) -> Bool
    func fileExists(at fileURL: URL) -> Bool
    func displayName(for fileURL: URL) -> String
    func createExportArtifact(fileName: String, gcodePayload: String) throws -> URL
}

enum ProjectFileAccessError: LocalizedError {
    case unsupportedExportLocation
    case failedToWriteArtifact

    var errorDescription: String? {
        switch self {
        case .unsupportedExportLocation:
            return "Unable to resolve an export destination."
        case .failedToWriteArtifact:
            return "Failed to write generated G-code to storage."
        }
    }
}

struct LocalProjectFileAccessService: ProjectFileAccessServicing {
    let supportedModelExtensions: Set<String> = ["3mf", "stl", "obj", "step", "stp", "amf"]

    func isSupportedModelFile(_ fileURL: URL) -> Bool {
        supportedModelExtensions.contains(fileURL.pathExtension.lowercased())
    }

    func fileExists(at fileURL: URL) -> Bool {
        FileManager.default.fileExists(atPath: fileURL.path)
    }

    func displayName(for fileURL: URL) -> String {
        fileURL.lastPathComponent
    }

    func createExportArtifact(fileName: String, gcodePayload: String) throws -> URL {
        guard let exportsDirectory = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first else {
            throw ProjectFileAccessError.unsupportedExportLocation
        }

        let outputURL = exportsDirectory.appendingPathComponent(fileName)

        do {
            try gcodePayload.write(to: outputURL, atomically: true, encoding: .utf8)
        } catch {
            throw ProjectFileAccessError.failedToWriteArtifact
        }

        return outputURL
    }
}
