import Foundation

struct ScreenshotLaunchConfiguration {
    static let modeEnvironmentKey = "ORCASLICER_IOS_SCREENSHOT_MODE"
    static let sceneEnvironmentKey = "ORCASLICER_IOS_SCREENSHOT_SCENE"
    static let settleMillisecondsEnvironmentKey = "ORCASLICER_IOS_SCREENSHOT_SETTLE_MS"

    enum RequestedScene: String, CaseIterable {
        case root
        case project
        case tools
        case sliceSettings = "slice-settings"
        case printer
        case view
        case appSettings = "app-settings"

        var panel: PanelKind? {
            switch self {
            case .root:
                return nil
            case .project:
                return .project
            case .tools:
                return .tools
            case .sliceSettings:
                return .sliceSettings
            case .printer:
                return .printer
            case .view:
                return .view
            case .appSettings:
                return .appSettings
            }
        }
    }

    let enabled: Bool
    let requestedScene: RequestedScene
    let settleDelayMilliseconds: Int

    static var disabled: ScreenshotLaunchConfiguration {
        ScreenshotLaunchConfiguration(enabled: false, requestedScene: .root, settleDelayMilliseconds: 0)
    }

    static func fromEnvironment(_ environment: [String: String] = ProcessInfo.processInfo.environment) -> ScreenshotLaunchConfiguration {
        guard isTruthy(environment[modeEnvironmentKey]) else {
            return .disabled
        }

        let sceneRawValue = environment[sceneEnvironmentKey] ?? RequestedScene.root.rawValue
        guard let requestedScene = RequestedScene(rawValue: sceneRawValue) else {
            let valid = RequestedScene.allCases.map(\.rawValue).joined(separator: ", ")
            fatalError("Invalid \(sceneEnvironmentKey)=\(sceneRawValue). Expected one of: \(valid)")
        }

        let settleDelayMilliseconds = parsedSettleDelay(environment[settleMillisecondsEnvironmentKey])

        return ScreenshotLaunchConfiguration(
            enabled: true,
            requestedScene: requestedScene,
            settleDelayMilliseconds: settleDelayMilliseconds
        )
    }

    private static func isTruthy(_ value: String?) -> Bool {
        guard let normalized = value?.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() else {
            return false
        }

        return normalized == "1" || normalized == "true" || normalized == "yes"
    }

    private static func parsedSettleDelay(_ value: String?) -> Int {
        guard let value, let milliseconds = Int(value) else {
            return 350
        }

        return max(0, milliseconds)
    }
}
