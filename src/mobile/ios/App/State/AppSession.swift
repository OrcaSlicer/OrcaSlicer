import Foundation

final class AppSession: ObservableObject {
    @Published var recentProjectNames: [String] = ["Benchy.3mf", "PhoneStand.stl"]
    @Published var lastActionStatus: String = ""

    let buildSummary: String = "iOS shell scaffold"
}
