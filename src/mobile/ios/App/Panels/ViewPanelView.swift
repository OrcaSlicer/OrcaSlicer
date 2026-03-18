import SwiftUI

struct ViewPanelView: View {
    @ObservedObject var viewportSession: ViewportSession

    var body: some View {
        List {
            Section("Camera") {
                Toggle("Top-down camera shading", isOn: $viewportSession.isLookingDownward)
                Button("Reset camera") {
                    viewportSession.resetCamera()
                }
            }

            Section("Preview") {
                summaryRow("Layer range", value: "0 - 186")
                summaryRow("Coloring", value: "Speed")
                summaryRow("Travel moves", value: "Visible")
            }
        }
        .navigationTitle("View")
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
