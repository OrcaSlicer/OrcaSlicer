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
                Text("Layer slider placeholder")
                Text("Toolpath preview mode placeholder")
            }
        }
        .navigationTitle("View")
    }
}
