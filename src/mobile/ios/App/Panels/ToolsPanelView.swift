import SwiftUI

struct ToolsPanelView: View {
    @ObservedObject var toolState: ToolPanelState

    var body: some View {
        List {
            Section("Transform") {
                Picker("Active tool", selection: $toolState.activeTool) {
                    ForEach(ToolPanelState.ToolKind.allCases, id: \.self) { tool in
                        Text(tool.title).tag(tool)
                    }
                }
                .pickerStyle(.inline)

                Toggle("Snap to bed", isOn: $toolState.snapToBed)
                Toggle("Uniform scale lock", isOn: $toolState.uniformScale)
            }

            Section("Arrange") {
                panelText("Auto-arrange models")
                panelText("Orient for strongest faces")
                panelText("Support painting placeholder")
            }
        }
        .navigationTitle("Tools")
    }

    private func panelText(_ text: String) -> some View {
        Text(text)
            .font(.body)
            .padding(.vertical, 4)
    }
}
