import SwiftUI
import UIKit

@main
struct OrcaSlicerIOSApp: App {
    @StateObject private var appSession = AppSession()
    @StateObject private var viewportSession = ViewportSession()
    @StateObject private var panelRouter = PanelRouter()
    @StateObject private var toolState = ToolPanelState()
    @StateObject private var sliceSettingsState = SliceSettingsState()
    @StateObject private var machineProfileState = MachineProfileState()

    private let screenshotLaunch: ScreenshotLaunchConfiguration

    init() {
        let launchConfig = ScreenshotLaunchConfiguration.fromEnvironment()
        screenshotLaunch = launchConfig
        if launchConfig.enabled {
            UIView.setAnimationsEnabled(false)
        }
    }

    var body: some Scene {
        WindowGroup {
            RootViewportScreen(
                appSession: appSession,
                viewportSession: viewportSession,
                panelRouter: panelRouter,
                toolState: toolState,
                sliceSettingsState: sliceSettingsState,
                machineProfileState: machineProfileState,
                screenshotLaunch: screenshotLaunch
            )
            .preferredColorScheme(.dark)
            .onOpenURL { sharedFileURL in
                appSession.handleSharedFile(sharedFileURL)
                panelRouter.present(.project)
            }
        }
    }
}
