import SwiftUI

/// Lets the user change the Pi's dashboard address.
struct SettingsView: View {
    @Binding var serverURL: String
    @Environment(\.dismiss) private var dismiss
    @State private var draft = ""

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("http://ras-4:5000", text: $draft)
                        .autocorrectionDisabled()
                        .urlEntry()
                } header: {
                    Text("Pi dashboard address")
                } footer: {
                    Text("Use http://ras-4:5000 with Tailscale on, or http://ras-4.local:5000 on your home Wi-Fi.")
                }
            }
            .navigationTitle("Settings")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        serverURL = draft.trimmingCharacters(in: .whitespacesAndNewlines)
                        dismiss()
                    }
                    .disabled(GardenStore.url(from: draft) == nil)
                }
            }
            .onAppear { draft = serverURL }
        }
    }
}

private extension View {
    @ViewBuilder
    func urlEntry() -> some View {
        #if os(iOS) || os(visionOS)
        self.keyboardType(.URL).textInputAutocapitalization(.never)
        #else
        self
        #endif
    }
}
