import Foundation

final class ViewportSession: ObservableObject {
    @Published var isLookingDownward = true

    func resetCamera() {
        isLookingDownward = true
    }
}
