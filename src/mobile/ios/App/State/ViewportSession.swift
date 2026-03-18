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

    func configurePreviewLoaded(projectName: String) {
        previewModelName = projectName
        previewStatusText = "Preview ready (unsliced)"
        previewDetailText = "Layer estimate pending • 0 toolpaths generated"
    }

    func configureBenchyPreviewLoaded() {
        configurePreviewLoaded(projectName: "3DBenchy.3mf")
    }

    func configureBenchyPreviewSliced() {
        previewModelName = "3DBenchy.3mf"
        previewStatusText = "Slice complete"
        previewDetailText = "0.20 mm layers • 15% infill • 142 layers"
    }
}
