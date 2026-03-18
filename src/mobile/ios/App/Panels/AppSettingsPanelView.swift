import SwiftUI

struct AppSettingsPanelView: View {
    @ObservedObject var appSession: AppSession

    var body: some View {
        List {
            Section("App") {
                summaryRow("Theme", value: "Dark")
                summaryRow("Build", value: appSession.buildSummary)
                summaryRow("Status", value: appSession.lastActionStatus.isEmpty ? "Ready" : appSession.lastActionStatus)
            }

            Section("Debug") {
                Text("Portability diagnostics placeholder")
                Text("Renderer stats placeholder")
            }
        }
        .navigationTitle("Settings")
    }

    private func summaryRow(_ title: String, value: String) -> some View {
        HStack {
            Text(title)
            Spacer()
            Text(value)
                .multilineTextAlignment(.trailing)
                .foregroundStyle(.secondary)
        }
    }
}
