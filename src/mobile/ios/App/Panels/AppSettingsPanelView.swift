import SwiftUI

struct AppSettingsPanelView: View {
    @ObservedObject var appSession: AppSession

    var body: some View {
        List {
            Section("App") {
                summaryRow("Theme", value: "Dark")
                summaryRow("Build", value: appSession.buildSummary)
                summaryRow("Runtime", value: appSession.runtimeSummary)
                summaryRow("Status", value: appSession.lastActionStatus.isEmpty ? "Ready" : appSession.lastActionStatus)
            }

            Section("Diagnostics") {
                summaryRow("Render backend", value: appSession.renderBackend)
                summaryRow("Backend version", value: appSession.backendVersion)
                summaryRow("Portability API", value: appSession.portabilityStatus)
                summaryRow("Debug logs", value: appSession.debugLogsSummary)
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
