import SwiftUI

struct RootViewportScreen: View {
    @ObservedObject var appSession: AppSession
    @ObservedObject var viewportSession: ViewportSession
    @ObservedObject var panelRouter: PanelRouter
    @ObservedObject var toolState: ToolPanelState
    @ObservedObject var sliceSettingsState: SliceSettingsState
    @ObservedObject var machineProfileState: MachineProfileState
    let screenshotLaunch: ScreenshotLaunchConfiguration

    @State private var hasAppliedScreenshotRoute = false

    var body: some View {
        ZStack {
            MetalViewportContainer(viewportSession: viewportSession)
                .ignoresSafeArea()

            ViewportPlaceholderOverlay(viewportSession: viewportSession)
                .ignoresSafeArea()
                .allowsHitTesting(false)

            VStack {
                ProjectStatusChip(
                    projectName: appSession.recentProjectNames.first ?? "No project loaded",
                    printerName: machineProfileState.printerName
                )
                .padding(.top, 56)
                Spacer()
            }
            .padding(.horizontal, 20)
            .allowsHitTesting(false)

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
        .onAppear(perform: applyScreenshotRouteIfNeeded)
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
        appSession.lastActionStatus = "Slice settings ready"
        panelRouter.present(.sliceSettings)
    }

    private func applyScreenshotRouteIfNeeded() {
        guard screenshotLaunch.enabled, !hasAppliedScreenshotRoute else {
            return
        }

        hasAppliedScreenshotRoute = true
        let delay = Double(screenshotLaunch.settleDelayMilliseconds) / 1000

        DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
            applyScreenshotSceneState()
            if let panel = screenshotLaunch.requestedScene.panel {
                panelRouter.present(panel)
            } else {
                panelRouter.dismiss()
            }
            NSLog("OrcaSlicerIOS screenshot route ready for scene=%@", screenshotLaunch.requestedScene.rawValue)
        }
    }

    private func applyScreenshotSceneState() {
        switch screenshotLaunch.requestedScene {
        case .root:
            viewportSession.resetPreviewMetadata()
            appSession.lastActionStatus = "Screenshot scene: root"
        case .benchyPreview:
            viewportSession.configureBenchyPreviewLoaded()
            appSession.recentProjectNames = ["3DBenchy.3mf", "PhoneStand.stl"]
            appSession.lastActionStatus = "Loaded 3DBenchy.3mf into preview"
        case .benchySliced:
            viewportSession.configureBenchyPreviewSliced()
            appSession.recentProjectNames = ["3DBenchy.3mf", "PhoneStand.stl"]
            appSession.lastActionStatus = "Slice complete for 3DBenchy.3mf"
        case .project, .tools, .sliceSettings, .printer, .view, .appSettings:
            viewportSession.resetPreviewMetadata()
            appSession.lastActionStatus = "Screenshot scene: \(screenshotLaunch.requestedScene.rawValue)"
        }
    }
}

private struct ViewportPlaceholderOverlay: View {
    @ObservedObject var viewportSession: ViewportSession

    var body: some View {
        ZStack {
            LinearGradient(
                colors: [
                    Color(red: 0.07, green: 0.09, blue: 0.12),
                    Color(red: 0.03, green: 0.04, blue: 0.06)
                ],
                startPoint: .top,
                endPoint: .bottom
            )

            RoundedRectangle(cornerRadius: 24, style: .continuous)
                .strokeBorder(Color.white.opacity(0.24), lineWidth: 1)
                .background(
                    RoundedRectangle(cornerRadius: 24, style: .continuous)
                        .fill(Color.black.opacity(0.25))
                )
                .overlay {
                    GridPattern(
                        modelName: viewportSession.previewModelName,
                        statusText: viewportSession.previewStatusText,
                        detailText: viewportSession.previewDetailText
                    )
                        .clipShape(RoundedRectangle(cornerRadius: 24, style: .continuous))
                }
                .padding(.horizontal, 20)
                .padding(.vertical, 120)
        }
    }
}

private struct GridPattern: View {
    let modelName: String
    let statusText: String
    let detailText: String

    var body: some View {
        GeometryReader { proxy in
            Path { path in
                let spacing: CGFloat = 28
                let width = proxy.size.width
                let height = proxy.size.height

                var x: CGFloat = 0
                while x <= width {
                    path.move(to: CGPoint(x: x, y: 0))
                    path.addLine(to: CGPoint(x: x, y: height))
                    x += spacing
                }

                var y: CGFloat = 0
                while y <= height {
                    path.move(to: CGPoint(x: 0, y: y))
                    path.addLine(to: CGPoint(x: width, y: y))
                    y += spacing
                }
            }
            .stroke(Color.white.opacity(0.08), lineWidth: 0.6)

            Circle()
                .strokeBorder(Color.cyan.opacity(0.28), lineWidth: 1.2)
                .frame(width: min(proxy.size.width, proxy.size.height) * 0.45)
                .position(x: proxy.size.width / 2, y: proxy.size.height / 2)

            VStack(spacing: 8) {
                Image(systemName: "cube.transparent")
                    .font(.system(size: 34, weight: .regular))
                    .foregroundStyle(.white.opacity(0.72))
                Text("Viewport placeholder")
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(.white.opacity(0.72))
                Text(modelName)
                    .font(.subheadline.weight(.medium))
                    .foregroundStyle(.cyan.opacity(0.78))
                Text(statusText)
                    .font(.caption)
                    .foregroundStyle(.white.opacity(0.5))
                Text(detailText)
                    .font(.caption2)
                    .foregroundStyle(.white.opacity(0.45))
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }
}

private struct ProjectStatusChip: View {
    let projectName: String
    let printerName: String

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: "shippingbox")
                .foregroundStyle(.white.opacity(0.8))
            VStack(alignment: .leading, spacing: 2) {
                Text(projectName)
                    .font(.subheadline.weight(.semibold))
                    .lineLimit(1)
                Text(printerName)
                    .font(.caption)
                    .foregroundStyle(.white.opacity(0.58))
                    .lineLimit(1)
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .background(Color.black.opacity(0.55))
        .overlay(
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .stroke(Color.white.opacity(0.14), lineWidth: 1)
        )
        .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
    }
}
