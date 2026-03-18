import SwiftUI

struct ProjectPanelView: View {
    @ObservedObject var appSession: AppSession

    var body: some View {
        List {
            Section("Project") {
                panelRow(title: "Import model", subtitle: "Load STL / 3MF / OBJ", icon: "square.and.arrow.down")
                panelRow(title: "Open recent", subtitle: appSession.recentProjectNames.first ?? "No recent projects yet", icon: "clock.arrow.circlepath")
                panelRow(title: "Export", subtitle: "Prepare G-code export flow", icon: "square.and.arrow.up")
            }
        }
        .navigationTitle("Project")
    }

    private func panelRow(title: String, subtitle: String, icon: String) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Label(title, systemImage: icon)
                .font(.headline)
            Text(subtitle)
                .font(.subheadline)
                .foregroundStyle(.secondary)
        }
        .padding(.vertical, 6)
    }
}
