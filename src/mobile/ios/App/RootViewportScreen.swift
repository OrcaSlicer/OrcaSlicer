import SwiftUI

struct RootViewportScreen: View {
    @ObservedObject var appSession: AppSession
    @ObservedObject var viewportSession: ViewportSession
    @ObservedObject var panelRouter: PanelRouter
    @ObservedObject var toolState: ToolPanelState
    @ObservedObject var sliceSettingsState: SliceSettingsState
    @ObservedObject var machineProfileState: MachineProfileState

    var body: some View {
        ZStack {
            MetalViewportContainer(viewportSession: viewportSession)
                .ignoresSafeArea()

            FloatingControlOverlay(
                appSession: appSession,
                panelRouter: panelRouter,
                onSliceTapped: startSlice
            )
        }
        .sheet(item: $panelRouter.presentedPanel) { panel in
            panelContent(panel)
                .presentationDetents([.medium, .large])
                .presentationDragIndicator(.visible)
        }
    }

    @ViewBuilder
    private func panelContent(_ panel: PanelKind) -> some View {
        NavigationStack {
            switch panel {
            case .project:
                ProjectPanelView(appSession: appSession)
            case .tools:
                ToolsPanelView(toolState: toolState)
            case .sliceSettings:
                SliceSettingsPanelView(sliceSettingsState: sliceSettingsState)
            case .printer:
                PrinterPanelView(machineProfileState: machineProfileState)
            case .view:
                ViewPanelView(viewportSession: viewportSession)
            case .appSettings:
                AppSettingsPanelView(appSession: appSession)
            }
        }
    }

    private func startSlice() {
        appSession.lastActionStatus = "Slice queued at \(Date.now.formatted(date: .omitted, time: .shortened))."
        panelRouter.present(.sliceSettings)
    }
}
