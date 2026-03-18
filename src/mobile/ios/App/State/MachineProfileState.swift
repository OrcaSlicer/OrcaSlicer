import Foundation

final class MachineProfileState: ObservableObject {
    @Published var printerName: String = "Orca iOS Placeholder"
    @Published var nozzleSummary: String = "0.4 mm nozzle"
    @Published var materialSummary: String = "PLA / Generic"
}
