import SwiftUI

struct PrinterPanelView: View {
    @ObservedObject var machineProfileState: MachineProfileState

    var body: some View {
        List {
            Section("Machine") {
                summaryRow("Printer", value: machineProfileState.printerName)
                summaryRow("Nozzle", value: machineProfileState.nozzleSummary)
                summaryRow("Material", value: machineProfileState.materialSummary)
            }

            Section("Connectivity") {
                summaryRow("LAN mode", value: "Standby")
                summaryRow("Cloud sync", value: "Not linked")
                summaryRow("Firmware", value: "Placeholder 01.00")
            }
        }
        .navigationTitle("Printer")
    }

    private func summaryRow(_ name: String, value: String) -> some View {
        HStack {
            Text(name)
            Spacer()
            Text(value)
                .foregroundStyle(.secondary)
        }
    }
}
