import Foundation

final class AppSession: ObservableObject {
    @Published var recentProjectNames: [String] = ["Benchy.3mf", "PhoneStand.stl"]
    @Published var lastActionStatus: String = ""

    let buildSummary: String = "iOS shell scaffold"

    private let supportedSharedExtensions: Set<String> = ["3mf", "stl", "obj", "step", "stp", "amf"]

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
