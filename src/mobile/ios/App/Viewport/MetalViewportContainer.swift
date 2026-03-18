import SwiftUI

struct MetalViewportContainer: UIViewRepresentable {
    @ObservedObject var viewportSession: ViewportSession

    func makeUIView(context: Context) -> OrcaMetalViewportView {
        let view = OrcaMetalViewportView(frame: .zero)
        view.setLookingDownward(viewportSession.isLookingDownward)
        return view
    }

    func updateUIView(_ uiView: OrcaMetalViewportView, context: Context) {
        uiView.setLookingDownward(viewportSession.isLookingDownward)
    }
}
