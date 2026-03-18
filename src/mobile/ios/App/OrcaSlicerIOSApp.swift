import SwiftUI
import UIKit

@main
struct OrcaSlicerIOSApp: App {
    @StateObject private var projectProfileStore: ProjectProfileStore
    @StateObject private var appSession: AppSession
    @StateObject private var viewportSession: ViewportSession
    @StateObject private var panelRouter = PanelRouter()
    @StateObject private var toolState: ToolPanelState
    @StateObject private var sliceSettingsState: SliceSettingsState
    @StateObject private var machineProfileState: MachineProfileState

    private let screenshotLaunch: ScreenshotLaunchConfiguration

    init() {
        let store = ProjectProfileStore()
        _projectProfileStore = StateObject(wrappedValue: store)
        _appSession = StateObject(wrappedValue: AppSession(store: store))
        _viewportSession = StateObject(wrappedValue: ViewportSession(store: store))
        _toolState = StateObject(wrappedValue: ToolPanelState(store: store))
        _sliceSettingsState = StateObject(wrappedValue: SliceSettingsState(store: store))
        _machineProfileState = StateObject(wrappedValue: MachineProfileState(store: store))

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
