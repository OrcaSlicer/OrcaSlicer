import SwiftUI

@main
struct OrcaSlicerIOSApp: App {
    @StateObject private var appSession = AppSession()
    @StateObject private var viewportSession = ViewportSession()
    @StateObject private var panelRouter = PanelRouter()
    @StateObject private var toolState = ToolPanelState()
    @StateObject private var sliceSettingsState = SliceSettingsState()
    @StateObject private var machineProfileState = MachineProfileState()

    var body: some Scene {
        WindowGroup {
            RootViewportScreen(
                appSession: appSession,
                viewportSession: viewportSession,
                panelRouter: panelRouter,
                toolState: toolState,
                sliceSettingsState: sliceSettingsState,
                machineProfileState: machineProfileState
            )
            .preferredColorScheme(.dark)
            .onOpenURL { sharedFileURL in
                appSession.handleSharedFile(sharedFileURL)
                panelRouter.present(.project)
            }
        }
    }
}
