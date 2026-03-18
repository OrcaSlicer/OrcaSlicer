import SwiftUI

struct FloatingControlOverlay: View {
    @ObservedObject var appSession: AppSession
    @ObservedObject var panelRouter: PanelRouter
    let onSliceTapped: () -> Void

    var body: some View {
        GeometryReader { proxy in
            let safeInsets = proxy.safeAreaInsets

            VStack(spacing: 0) {
                HStack {
                    FloatingActionButton(title: "Project", systemImage: "folder", action: {
                        panelRouter.present(.project)
                    })

                    Spacer()

                    FloatingActionButton(title: "Settings", systemImage: "gearshape", action: {
                        panelRouter.present(.appSettings)
                    })
                }
                .padding(.horizontal, 16)
                .padding(.top, max(safeInsets.top, 12))

                Spacer()

                HStack(alignment: .bottom) {
                    VStack(alignment: .leading, spacing: 12) {
                        FloatingActionButton(title: "Tools", systemImage: "wrench.and.screwdriver", action: {
                            panelRouter.present(.tools)
                        })

                        FloatingActionButton(title: "View", systemImage: "viewfinder", action: {
                            panelRouter.present(.view)
                        })

                        FloatingActionButton(title: "Printer", systemImage: "printer", action: {
                            panelRouter.present(.printer)
                        })
                    }

                    Spacer(minLength: 12)

                    VStack(alignment: .trailing, spacing: 12) {
                        if !appSession.lastActionStatus.isEmpty {
                            Text(appSession.lastActionStatus)
                                .font(.caption.weight(.medium))
                                .padding(.horizontal, 12)
                                .padding(.vertical, 8)
                                .foregroundStyle(.white)
                                .background(Color(red: 0.13, green: 0.14, blue: 0.18).opacity(0.95))
                                .overlay(
                                    RoundedRectangle(cornerRadius: 12, style: .continuous)
                                        .stroke(Color.white.opacity(0.16), lineWidth: 1)
                                )
                                .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
                        }

                        FloatingActionButton(title: "Slice", systemImage: "play.fill", isPrimary: true, action: onSliceTapped)
                    }
                }
                .padding(.horizontal, 16)
                .padding(.bottom, max(safeInsets.bottom, 12) + 8)
            }
        }
    }
}
