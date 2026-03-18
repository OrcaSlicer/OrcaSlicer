import Foundation

final class ViewportSession: ObservableObject {
    @Published var isLookingDownward = true
    @Published var previewModelName: String = "No model loaded"
    @Published var previewStatusText: String = "Load a project to preview"
    @Published var previewDetailText: String = "Portable Metal renderer bridge active"

    func resetCamera() {
        isLookingDownward = true
    }

    func resetPreviewMetadata() {
        previewModelName = "No model loaded"
        previewStatusText = "Load a project to preview"
        previewDetailText = "Portable Metal renderer bridge active"
    }

    func configureBenchyPreviewLoaded() {
        previewModelName = "3DBenchy.3mf"
        previewStatusText = "Preview ready (unsliced)"
        previewDetailText = "Layer estimate pending • 0 toolpaths generated"
    }

    func configureBenchyPreviewSliced() {
        previewModelName = "3DBenchy.3mf"
        previewStatusText = "Slice complete"
        previewDetailText = "0.20 mm layers • 15% infill • 142 layers"
    }

    func applySliceOutput(
        modelName: String,
        statusText: String,
        detailText: String,
        layerCount: Int,
        toolpathCount: Int,
        estimatedPrintTimeSeconds: Int
    ) {
        previewModelName = modelName
        previewStatusText = statusText

        let hours = estimatedPrintTimeSeconds / 3600
        let minutes = (estimatedPrintTimeSeconds % 3600) / 60
        let timeSummary = hours > 0 ? "\(hours)h \(minutes)m" : "\(minutes)m"
        previewDetailText = "\(detailText) • \(toolpathCount) toolpaths • ETA \(timeSummary)"

        if layerCount <= 0 || toolpathCount <= 0 {
            previewDetailText = "Slice output incomplete. Check diagnostics."
        }
    }
}
