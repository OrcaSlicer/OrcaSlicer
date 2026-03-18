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
                VStack(alignment: .leading, spacing: 8) {
                    Text("Layer range: \(viewportSession.layerStart) - \(viewportSession.layerEnd)")
                        .font(.subheadline)

                    Slider(
                        value: Binding(
                            get: { Double(viewportSession.layerStart) },
                            set: { viewportSession.layerStart = Int($0) }
                        ),
                        in: 0 ... Double(viewportSession.maxLayer),
                        step: 1
                    )

                    Slider(
                        value: Binding(
                            get: { Double(viewportSession.layerEnd) },
                            set: { viewportSession.layerEnd = Int($0) }
                        ),
                        in: Double(viewportSession.layerStart) ... Double(viewportSession.maxLayer),
                        step: 1
                    )
                }

                Picker("Coloring", selection: $viewportSession.coloringMode) {
                    ForEach(PreviewColoringMode.allCases, id: \.self) { mode in
                        Text(mode.rawValue).tag(mode)
                    }
                }

                Toggle("Travel moves", isOn: $viewportSession.showTravelMoves)
            }
        }
        .navigationTitle("View")
    }
}
