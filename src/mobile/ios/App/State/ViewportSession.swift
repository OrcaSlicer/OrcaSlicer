import CoreGraphics
import Foundation

struct ViewportCameraState {
    let viewMatrix: [Double]
    let projectionMatrix: [Double]
    let isLookingDownward: Bool
}

final class ViewportSession: ObservableObject {
    @Published var isLookingDownward = true
    @Published var previewModelName: String = "No model loaded"
    @Published var previewStatusText: String = "Load a project to preview"
    @Published var previewDetailText: String = "Portable Metal renderer bridge active"

    @Published private(set) var cameraYawRadians: Double = 0
    @Published private(set) var cameraPitchRadians: Double = 0.95
    @Published private(set) var cameraZoom: Double = 1.0
    @Published private(set) var cameraPanX: Double = 0
    @Published private(set) var cameraPanY: Double = 0

    func resetCamera() {
        cameraYawRadians = 0
        cameraPitchRadians = 0.95
        cameraZoom = 1.0
        cameraPanX = 0
        cameraPanY = 0
        isLookingDownward = true
    }

    func applyPan(delta: CGSize, viewportSize: CGSize) {
        let dominantAxis = max(viewportSize.width, viewportSize.height)
        guard dominantAxis > 0 else { return }

        let worldUnitsPerPoint = 6.0 / (Double(dominantAxis) * cameraZoom)
        cameraPanX -= Double(delta.width) * worldUnitsPerPoint
        cameraPanY += Double(delta.height) * worldUnitsPerPoint
    }

    func applyPinch(scale: CGFloat) {
        guard scale.isFinite, scale > 0 else { return }
        let nextZoom = cameraZoom * Double(scale)
        cameraZoom = min(max(nextZoom, 0.45), 6.0)
    }

    func applyRotation(deltaRadians: CGFloat) {
        guard deltaRadians.isFinite else { return }
        cameraYawRadians += Double(deltaRadians)
    }

    func cameraState(viewportSize: CGSize) -> ViewportCameraState {
        let width = max(Double(viewportSize.width), 1)
        let height = max(Double(viewportSize.height), 1)

        let clampedPitch = min(max(cameraPitchRadians, -1.45), 1.45)
        let targetX = cameraPanX
        let targetY = cameraPanY
        let targetZ = 0.0

        let orbitDistance = 7.5 / cameraZoom
        let cosPitch = cos(clampedPitch)
        let eyeX = targetX + orbitDistance * cosPitch * sin(cameraYawRadians)
        let eyeY = targetY - orbitDistance * cosPitch * cos(cameraYawRadians)
        let eyeZ = targetZ + orbitDistance * sin(clampedPitch)

        let view = makeLookAtMatrix(
            eye: (eyeX, eyeY, eyeZ),
            center: (targetX, targetY, targetZ),
            up: (0, 0, 1)
        )
        let projection = makePerspectiveMatrix(
            fovYRadians: 0.9,
            aspectRatio: width / height,
            nearZ: 0.1,
            farZ: 400
        )

        let forwardZ = targetZ - eyeZ
        let downward = forwardZ < 0
        isLookingDownward = downward

        return ViewportCameraState(
            viewMatrix: view,
            projectionMatrix: projection,
            isLookingDownward: downward
        )
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

    private func makeLookAtMatrix(
        eye: (Double, Double, Double),
        center: (Double, Double, Double),
        up: (Double, Double, Double)
    ) -> [Double] {
        let fx = center.0 - eye.0
        let fy = center.1 - eye.1
        let fz = center.2 - eye.2
        let fInvLen = 1.0 / max(sqrt(fx * fx + fy * fy + fz * fz), 0.00001)
        let fnx = fx * fInvLen
        let fny = fy * fInvLen
        let fnz = fz * fInvLen

        let sx = fny * up.2 - fnz * up.1
        let sy = fnz * up.0 - fnx * up.2
        let sz = fnx * up.1 - fny * up.0
        let sInvLen = 1.0 / max(sqrt(sx * sx + sy * sy + sz * sz), 0.00001)
        let snx = sx * sInvLen
        let sny = sy * sInvLen
        let snz = sz * sInvLen

        let ux = sny * fnz - snz * fny
        let uy = snz * fnx - snx * fnz
        let uz = snx * fny - sny * fnx

        return [
            snx, ux, -fnx, 0,
            sny, uy, -fny, 0,
            snz, uz, -fnz, 0,
            -(snx * eye.0 + sny * eye.1 + snz * eye.2),
            -(ux * eye.0 + uy * eye.1 + uz * eye.2),
            fnx * eye.0 + fny * eye.1 + fnz * eye.2,
            1
        ]
    }

    private func makePerspectiveMatrix(
        fovYRadians: Double,
        aspectRatio: Double,
        nearZ: Double,
        farZ: Double
    ) -> [Double] {
        let f = 1.0 / tan(fovYRadians / 2)
        let zRange = nearZ - farZ

        return [
            f / aspectRatio, 0, 0, 0,
            0, f, 0, 0,
            0, 0, (farZ + nearZ) / zRange, -1,
            0, 0, (2 * farZ * nearZ) / zRange, 0
        ]
    }
}
