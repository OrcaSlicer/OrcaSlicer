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

            Section("Status") {
                Text("Profile synchronization placeholder")
                Text("Remote printer connectivity placeholder")
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
