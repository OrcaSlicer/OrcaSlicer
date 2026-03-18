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

            Section("Diagnostics") {
                summaryRow("Render backend", value: "Metal (placeholder)")
                summaryRow("Portability API", value: "Connected")
                summaryRow("Debug logs", value: "Scene routing enabled")
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
