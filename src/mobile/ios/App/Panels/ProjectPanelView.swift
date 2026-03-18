import SwiftUI

struct ProjectPanelView: View {
    @ObservedObject var appSession: AppSession

    var body: some View {
        List {
            Section("Project") {
                Button {
                    appSession.beginImportFlow()
                } label: {
                    panelRow(title: "Import model", subtitle: "Load STL / 3MF / OBJ / STEP / AMF", icon: "square.and.arrow.down")
                }
                .buttonStyle(.plain)

                NavigationLink {
                    OpenRecentProjectsView(appSession: appSession)
                } label: {
                    panelRow(title: "Open recent", subtitle: appSession.recentProjectNames.first ?? "No recent projects yet", icon: "clock.arrow.circlepath")
                }

                Button {
                    appSession.beginExportFlow()
                } label: {
                    panelRow(title: "Export", subtitle: "Select G-code profile and share", icon: "square.and.arrow.up")
                }
                .buttonStyle(.plain)
            }

            Section("Build plate") {
                summaryRow("Plate", value: "1 / 1")
                summaryRow("Objects", value: "2 placeholders")
                summaryRow("Estimated time", value: "1h 42m")
            }

            if let exportMetadata = appSession.lastExportResult {
                Section("Last export") {
                    summaryRow("Source", value: exportMetadata.sourceProjectName)
                    summaryRow("Profile", value: exportMetadata.target.rawValue)
                    summaryRow("Artifact", value: exportMetadata.artifactURL.lastPathComponent)
                }
            }
        }
        .navigationTitle("Project")
        .sheet(isPresented: $appSession.pickerImportPresented) {
            ModelImportDocumentPicker { pickedURL in
                appSession.importPickedDocument(pickedURL)
            }
        }
        .confirmationDialog("Select export target", isPresented: $appSession.exportTargetPickerPresented, titleVisibility: .visible) {
            ForEach(AppSession.ExportTarget.allCases) { target in
                Button(target.rawValue) {
                    appSession.runExport(target: target)
                }
            }
        }
        .sheet(isPresented: $appSession.exportShareSheetPresented) {
            if let artifactURL = appSession.exportShareArtifactURL {
                ExportShareSheet(items: [artifactURL])
            }
        }
        .alert(item: $appSession.userMessage) { message in
            Alert(title: Text(message.title), message: Text(message.detail), dismissButton: .default(Text("OK")))
        }
    }

    private func panelRow(title: String, subtitle: String, icon: String) -> some View {
        HStack(spacing: 12) {
            Image(systemName: icon)
                .font(.headline)
                .foregroundStyle(.secondary)
                .frame(width: 22)

            VStack(alignment: .leading, spacing: 6) {
                Text(title)
                    .font(.headline)
                Text(subtitle)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }

            Spacer()
            Image(systemName: "chevron.right")
                .font(.caption.weight(.semibold))
                .foregroundStyle(.tertiary)
        }
        .padding(.vertical, 6)
        .contentShape(Rectangle())
    }

    private func summaryRow(_ name: String, value: String) -> some View {
        HStack {
            Text(name)
            Spacer()
            Text(value)
                .foregroundStyle(.secondary)
        }
    }
}

private struct OpenRecentProjectsView: View {
    @ObservedObject var appSession: AppSession

    var body: some View {
        List {
            if appSession.recentProjectNames.isEmpty {
                Text("No recent projects yet")
                    .foregroundStyle(.secondary)
            } else {
                ForEach(appSession.recentProjectNames, id: \.self) { fileName in
                    Button {
                        appSession.openRecentProject(named: fileName)
                    } label: {
                        HStack {
                            VStack(alignment: .leading, spacing: 4) {
                                Text(fileName)
                                    .font(.body)
                                Text("Tap to validate file path and reopen")
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            }
                            Spacer()
                            Image(systemName: "arrow.uturn.right.circle")
                                .foregroundStyle(.secondary)
                        }
                    }
                    .buttonStyle(.plain)
                }
            }
        }
        .navigationTitle("Open recent")
    }
}
