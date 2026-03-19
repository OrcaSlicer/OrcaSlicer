import SwiftUI

struct MetalViewportContainer: UIViewRepresentable {
    @ObservedObject var viewportSession: ViewportSession

    func makeCoordinator() -> Coordinator {
        Coordinator(viewportSession: viewportSession)
    }

    func makeUIView(context: Context) -> OrcaMetalViewportView {
        NSLog("MetalViewportContainer.makeUIView")
        let view = OrcaMetalViewportView(frame: .zero)
        context.coordinator.attach(to: view)
        viewportSession.updateRendererAvailability(
            isAvailable: view.isRendererReady(),
            statusText: view.rendererInitializationSummary()
        )
        NSLog(
            "MetalViewportContainer.makeUIView rendererReady=%@ status=%@ bounds=%@ scale=%.2f",
            view.isRendererReady() ? "true" : "false",
            view.rendererInitializationSummary(),
            String(describing: view.bounds),
            view.contentScaleFactor
        )
        syncCamera(view)
        return view
    }

    func updateUIView(_ uiView: OrcaMetalViewportView, context: Context) {
        NSLog(
            "MetalViewportContainer.updateUIView rendererReady=%@ status=%@ bounds=%@ scale=%.2f",
            uiView.isRendererReady() ? "true" : "false",
            uiView.rendererInitializationSummary(),
            String(describing: uiView.bounds),
            uiView.contentScaleFactor
        )
        context.coordinator.viewportSession = viewportSession
        viewportSession.updateRendererAvailability(
            isAvailable: uiView.isRendererReady(),
            statusText: uiView.rendererInitializationSummary()
        )
        syncCamera(uiView)
    }

    private func syncCamera(_ uiView: OrcaMetalViewportView) {
        let camera = viewportSession.cameraState(viewportSize: uiView.bounds.size)
        camera.viewMatrix.withUnsafeBufferPointer { viewBuffer in
            camera.projectionMatrix.withUnsafeBufferPointer { projectionBuffer in
                guard let viewPtr = viewBuffer.baseAddress, let projectionPtr = projectionBuffer.baseAddress else {
                    return
                }

                uiView.setCameraWithViewMatrix(
                    viewPtr,
                    projectionMatrix: projectionPtr,
                    isLookingDownward: camera.isLookingDownward
                )
            }
        }
    }

    final class Coordinator: NSObject, UIGestureRecognizerDelegate {
        var viewportSession: ViewportSession
        private weak var viewportView: OrcaMetalViewportView?

        private var panRecognizer: UIPanGestureRecognizer?
        private var pinchRecognizer: UIPinchGestureRecognizer?
        private var rotationRecognizer: UIRotationGestureRecognizer?

        init(viewportSession: ViewportSession) {
            self.viewportSession = viewportSession
        }

        func attach(to view: OrcaMetalViewportView) {
            viewportView = view

            let pan = UIPanGestureRecognizer(target: self, action: #selector(handlePan(_:)))
            pan.delegate = self
            view.addGestureRecognizer(pan)
            panRecognizer = pan

            let pinch = UIPinchGestureRecognizer(target: self, action: #selector(handlePinch(_:)))
            pinch.delegate = self
            view.addGestureRecognizer(pinch)
            pinchRecognizer = pinch

            let rotation = UIRotationGestureRecognizer(target: self, action: #selector(handleRotation(_:)))
            rotation.delegate = self
            view.addGestureRecognizer(rotation)
            rotationRecognizer = rotation
        }

        func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer, shouldRecognizeSimultaneouslyWith otherGestureRecognizer: UIGestureRecognizer) -> Bool {
            true
        }

        @objc
        private func handlePan(_ recognizer: UIPanGestureRecognizer) {
            guard let view = viewportView else { return }
            let translation = recognizer.translation(in: view)
            recognizer.setTranslation(.zero, in: view)
            let panDelta = CGSize(width: translation.x, height: translation.y)
            viewportSession.applyPan(delta: panDelta, viewportSize: view.bounds.size)
            pushCameraState(into: view)
        }

        @objc
        private func handlePinch(_ recognizer: UIPinchGestureRecognizer) {
            guard let view = viewportView else { return }
            viewportSession.applyPinch(scale: recognizer.scale)
            recognizer.scale = 1
            pushCameraState(into: view)
        }

        @objc
        private func handleRotation(_ recognizer: UIRotationGestureRecognizer) {
            guard let view = viewportView else { return }
            viewportSession.applyRotation(deltaRadians: recognizer.rotation)
            recognizer.rotation = 0
            pushCameraState(into: view)
        }

        private func pushCameraState(into view: OrcaMetalViewportView) {
            let camera = viewportSession.cameraState(viewportSize: view.bounds.size)
            camera.viewMatrix.withUnsafeBufferPointer { viewBuffer in
                camera.projectionMatrix.withUnsafeBufferPointer { projectionBuffer in
                    guard let viewPtr = viewBuffer.baseAddress, let projectionPtr = projectionBuffer.baseAddress else {
                        return
                    }

                    view.setCameraWithViewMatrix(
                        viewPtr,
                        projectionMatrix: projectionPtr,
                        isLookingDownward: camera.isLookingDownward
                    )
                }
            }
        }
    }
}
