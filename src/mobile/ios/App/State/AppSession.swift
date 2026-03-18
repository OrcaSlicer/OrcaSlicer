import Foundation

enum SliceRunState {
    case idle
    case running
    case success
    case failure
}

final class AppSession: ObservableObject {
    @Published var recentProjectNames: [String] = ["Benchy.3mf", "PhoneStand.stl"]
    @Published var lastActionStatus: String = ""

    @Published var slicingState: SliceRunState = .idle
    @Published var slicingProgressPercent: Double = 0
    @Published var slicingMessage: String = ""
    @Published var slicingDiagnosticsLog: String = ""

    let buildSummary: String = "iOS shell scaffold"

    private let supportedSharedExtensions: Set<String> = ["3mf", "stl", "obj", "step", "stp", "amf"]

    var isSliceRunning: Bool {
        slicingState == .running
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

        lastActionStatus = "Imported shared file: \(fileName)"
    }
}
