import SwiftUI

struct SliceSettingsPanelView: View {
    @ObservedObject var sliceSettingsState: SliceSettingsState

    var body: some View {
        List {
            Section("Quality") {
                Picker("Preset", selection: $sliceSettingsState.qualityPreset) {
                    ForEach(SliceSettingsState.QualityPreset.allCases, id: \.self) { preset in
                        Text(preset.rawValue).tag(preset)
                    }
                }

                Stepper(value: $sliceSettingsState.infillPercent, in: 0 ... 100, step: 5) {
                    Text("Infill: \(sliceSettingsState.infillPercent)%")
                }

                Toggle("Supports", isOn: $sliceSettingsState.supportsEnabled)
            }

            Section("Material / Profile") {
                summaryRow("Material", value: sliceSettingsState.materialName)
                summaryRow("Profile", value: sliceSettingsState.profileName)

                Stepper(value: $sliceSettingsState.nozzleTemperatureC, in: 160 ... 320, step: 5) {
                    Text("Nozzle temp: \(sliceSettingsState.nozzleTemperatureC)°C")
                }

                Stepper(value: $sliceSettingsState.bedTemperatureC, in: 0 ... 140, step: 5) {
                    Text("Bed temp: \(sliceSettingsState.bedTemperatureC)°C")
                }

                Button("Save to profile") {
                    sliceSettingsState.saveToProfileService()
                }

                if !sliceSettingsState.validationMessage.isEmpty {
                    Text(sliceSettingsState.validationMessage)
                        .font(.footnote)
                        .foregroundStyle(.red)
                }
            }
        }
        .navigationTitle("Slice Settings")
        .onAppear {
            sliceSettingsState.loadFromProfileService()
        }
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
