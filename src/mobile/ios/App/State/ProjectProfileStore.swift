import Foundation
import Combine
import UIKit

#if canImport(Network)
import Network
#endif

struct SlicePresetOption: Identifiable, Hashable {
    let id: String
    let title: String
}

struct SliceSettingsData {
    var presetID: String
    var infillPercent: Int
    var supportsEnabled: Bool
    var materialName: String
    var profileName: String
    var nozzleTemperatureC: Int
    var bedTemperatureC: Int
}

enum ConnectivityState: String {
    case online
    case offline
    case unknown

    var title: String {
        switch self {
        case .online: return "Online"
        case .offline: return "Offline"
        case .unknown: return "Unknown"
        }
    }
}

struct MachineProfileData {
    var printerName: String
    var nozzleSummary: String
    var materialSummary: String
    var lanConnectivity: ConnectivityState
    var cloudSync: ConnectivityState
    var firmwareVersion: String
}

enum PreviewColoringMode: String, CaseIterable {
    case speed = "Speed"
    case featureType = "Feature Type"
    case lineWidth = "Line Width"
}

struct ViewPreviewData {
    var layerStart: Int
    var layerEnd: Int
    var maxLayer: Int
    var coloringMode: PreviewColoringMode
    var travelVisible: Bool
}

struct ToolPanelData {
    var activeToolID: String
    var snapToBed: Bool
    var uniformScale: Bool
}

struct RuntimeDiagnosticsData {
    var renderBackend: String
    var backendVersion: String
    var buildSummary: String
    var runtimeSummary: String
    var portabilityAPI: String
    var debugLogs: String
}

enum SliceSettingsValidationError: LocalizedError {
    case infillOutOfRange
    case nozzleTemperatureOutOfRange
    case bedTemperatureOutOfRange

    var errorDescription: String? {
        switch self {
        case .infillOutOfRange:
            return "Infill must be between 0% and 100%."
        case .nozzleTemperatureOutOfRange:
            return "Nozzle temperature must be between 160°C and 320°C."
        case .bedTemperatureOutOfRange:
            return "Bed temperature must be between 0°C and 140°C."
        }
    }
}

final class ProjectProfileStore: ObservableObject {
    @Published private(set) var availablePresets: [SlicePresetOption] = []
    @Published private(set) var sliceSettings: SliceSettingsData
    @Published private(set) var machineProfile: MachineProfileData
    @Published private(set) var viewPreview: ViewPreviewData
    @Published private(set) var toolPanel: ToolPanelData
    @Published private(set) var diagnostics: RuntimeDiagnosticsData

    #if canImport(Network)
    private let pathMonitor = NWPathMonitor()
    private let pathQueue = DispatchQueue(label: "com.orcaslicer.ios.network-monitor")
    #endif

    init() {
        NSLog("ProjectProfileStore.init")
        let appVersion = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "Unknown"
        let buildNumber = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "Unknown"

        availablePresets = [
            SlicePresetOption(id: "draft", title: "Draft"),
            SlicePresetOption(id: "balanced", title: "Balanced"),
            SlicePresetOption(id: "quality", title: "Quality")
        ]

        sliceSettings = SliceSettingsData(
            presetID: "balanced",
            infillPercent: 15,
            supportsEnabled: false,
            materialName: "PLA Basic",
            profileName: "0.20mm Standard",
            nozzleTemperatureC: 220,
            bedTemperatureC: 60
        )

        machineProfile = MachineProfileData(
            printerName: "No printer selected",
            nozzleSummary: "0.4 mm nozzle",
            materialSummary: "PLA / Generic",
            lanConnectivity: .unknown,
            cloudSync: .offline,
            firmwareVersion: "Unknown"
        )

        viewPreview = ViewPreviewData(
            layerStart: 0,
            layerEnd: 186,
            maxLayer: 186,
            coloringMode: .speed,
            travelVisible: true
        )

        toolPanel = ToolPanelData(activeToolID: "move", snapToBed: true, uniformScale: true)

        diagnostics = RuntimeDiagnosticsData(
            renderBackend: "Metal",
            backendVersion: "IOSMetalRenderBackend",
            buildSummary: "v\(appVersion) (\(buildNumber))",
            runtimeSummary: "\(UIDevice.current.systemName) \(UIDevice.current.systemVersion) • \(ProcessInfo.processInfo.operatingSystemVersionString)",
            portabilityAPI: "Connected",
            debugLogs: "Scene routing enabled"
        )

        hydrateFromRecentProjectName(nil)
        startConnectivityMonitoring()
    }

    func hydrateFromRecentProjectName(_ projectName: String?) {
        guard let projectName, !projectName.isEmpty else {
            machineProfile.printerName = "No printer selected"
            return
        }

        if projectName.lowercased().contains("benchy") {
            machineProfile.printerName = "Bambu Lab X1 Carbon"
            machineProfile.firmwareVersion = "01.07.02.00"
            machineProfile.materialSummary = "PLA / Generic"
        } else {
            machineProfile.printerName = "OrcaSlicer Project Profile"
            machineProfile.firmwareVersion = "Unknown"
        }
    }

    func updateToolPanel(activeToolID: String? = nil, snapToBed: Bool? = nil, uniformScale: Bool? = nil) {
        if let activeToolID {
            toolPanel.activeToolID = activeToolID
        }
        if let snapToBed {
            toolPanel.snapToBed = snapToBed
        }
        if let uniformScale {
            toolPanel.uniformScale = uniformScale
        }
    }

    func saveSliceSettings(_ updated: SliceSettingsData) throws {
        guard (0 ... 100).contains(updated.infillPercent) else {
            throw SliceSettingsValidationError.infillOutOfRange
        }

        guard (160 ... 320).contains(updated.nozzleTemperatureC) else {
            throw SliceSettingsValidationError.nozzleTemperatureOutOfRange
        }

        guard (0 ... 140).contains(updated.bedTemperatureC) else {
            throw SliceSettingsValidationError.bedTemperatureOutOfRange
        }

        sliceSettings = updated
    }

    func updateViewPreview(_ updated: ViewPreviewData) {
        let normalizedEnd = min(max(updated.layerEnd, updated.layerStart), updated.maxLayer)
        let normalizedStart = min(max(updated.layerStart, 0), normalizedEnd)

        viewPreview = ViewPreviewData(
            layerStart: normalizedStart,
            layerEnd: normalizedEnd,
            maxLayer: updated.maxLayer,
            coloringMode: updated.coloringMode,
            travelVisible: updated.travelVisible
        )
    }

    private func startConnectivityMonitoring() {
        #if canImport(Network)
        pathMonitor.pathUpdateHandler = { [weak self] path in
            DispatchQueue.main.async {
                guard let self else { return }
                self.machineProfile.lanConnectivity = path.status == .satisfied ? .online : .offline
                if path.status != .satisfied {
                    self.machineProfile.cloudSync = .offline
                } else if self.machineProfile.cloudSync == .offline {
                    self.machineProfile.cloudSync = .unknown
                }
            }
        }
        pathMonitor.start(queue: pathQueue)
        #else
        machineProfile.lanConnectivity = .unknown
        machineProfile.cloudSync = .unknown
        #endif
    }
}
