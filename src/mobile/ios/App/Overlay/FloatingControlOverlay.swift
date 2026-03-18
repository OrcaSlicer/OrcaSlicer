import SwiftUI

struct FloatingControlOverlay: View {
    @ObservedObject var appSession: AppSession
    @ObservedObject var panelRouter: PanelRouter
    let onSliceTapped: () -> Void
    let onCancelSliceTapped: () -> Void

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
                        statusBubble

                        HStack(spacing: 8) {
                            if appSession.isSliceRunning {
                                FloatingActionButton(title: "Cancel", systemImage: "xmark.circle.fill", action: onCancelSliceTapped)
                            }

                            FloatingActionButton(
                                title: appSession.isSliceRunning ? "Slicing…" : "Slice",
                                systemImage: appSession.isSliceRunning ? "hourglass" : "play.fill",
                                isPrimary: true,
                                action: onSliceTapped
                            )
                            .disabled(appSession.isSliceRunning)
                            .opacity(appSession.isSliceRunning ? 0.55 : 1)
                        }
                    }
                }
                .padding(.horizontal, 16)
                .padding(.bottom, max(safeInsets.bottom, 12) + 8)
            }
        }
    }

    @ViewBuilder
    private var statusBubble: some View {
        if appSession.isSliceRunning {
            VStack(alignment: .leading, spacing: 6) {
                Text("\(Int(appSession.slicingProgressPercent))% • \(appSession.slicingMessage)")
                    .font(.caption.weight(.semibold))
                ProgressView(value: appSession.slicingProgressPercent, total: 100)
                    .tint(.orange)
                    .frame(width: 220)
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            .foregroundStyle(.white)
            .background(Color(red: 0.13, green: 0.14, blue: 0.18).opacity(0.95))
            .overlay(
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .stroke(Color.white.opacity(0.16), lineWidth: 1)
            )
            .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
        } else if !appSession.slicingMessage.isEmpty,
                  appSession.slicingState == .failure || appSession.slicingState == .success {
            VStack(alignment: .leading, spacing: 4) {
                Text(appSession.slicingMessage)
                    .font(.caption.weight(.semibold))
                if appSession.slicingState == .failure, !appSession.slicingDiagnosticsLog.isEmpty {
                    Text(appSession.slicingDiagnosticsLog)
                        .font(.caption2)
                        .lineLimit(3)
                        .foregroundStyle(.white.opacity(0.7))
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            .foregroundStyle(.white)
            .background(Color(red: 0.13, green: 0.14, blue: 0.18).opacity(0.95))
            .overlay(
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .stroke(Color.white.opacity(0.16), lineWidth: 1)
            )
            .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
        } else if !appSession.lastActionStatus.isEmpty {
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
    }
}
