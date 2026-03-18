import Foundation
import Combine

final class ViewportSession: ObservableObject {
    @Published var isLookingDownward = true
    @Published var previewModelName: String = "No model loaded"
    @Published var previewStatusText: String = "Load a project to preview"
    @Published var previewDetailText: String = "Portable Metal renderer bridge active"

    @Published var layerStart: Int = 0 {
        didSet {
            guard !isHydratingPreview, oldValue != layerStart else { return }
            persistPreviewOptions()
        }
    }

    @Published var layerEnd: Int = 186 {
        didSet {
            guard !isHydratingPreview, oldValue != layerEnd else { return }
            persistPreviewOptions()
        }
    }

    @Published var maxLayer: Int = 186

    @Published var coloringMode: PreviewColoringMode = .speed {
        didSet {
            guard !isHydratingPreview, oldValue != coloringMode else { return }
            persistPreviewOptions()
        }
    }

    @Published var showTravelMoves: Bool = true {
        didSet {
            guard !isHydratingPreview, oldValue != showTravelMoves else { return }
            persistPreviewOptions()
        }
    }

    private let store: ProjectProfileStore
    private var cancellables: Set<AnyCancellable> = []
    private var isHydratingPreview = false

    init(store: ProjectProfileStore) {
        self.store = store

        store.$viewPreview
            .receive(on: DispatchQueue.main)
            .sink { [weak self] preview in
                guard let self else { return }
                self.isHydratingPreview = true
                self.layerStart = preview.layerStart
                self.layerEnd = preview.layerEnd
                self.maxLayer = preview.maxLayer
                self.coloringMode = preview.coloringMode
                self.showTravelMoves = preview.travelVisible
                self.isHydratingPreview = false
            }
            .store(in: &cancellables)
    }

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
        layerStart = 0
        layerEnd = 142
        maxLayer = 142
    }

    private func persistPreviewOptions() {
        let updated = ViewPreviewData(
            layerStart: layerStart,
            layerEnd: layerEnd,
            maxLayer: maxLayer,
            coloringMode: coloringMode,
            travelVisible: showTravelMoves
        )
        store.updateViewPreview(updated)
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
