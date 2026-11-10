import SwiftUI
import Charts

struct ContentView: View {
    @Environment(\.colorScheme) private var scheme
    @AppStorage("serverURL") private var serverURL = "http://ras-4:5000"
    @State private var store = GardenStore()
    @State private var rangeHours = 24
    @State private var showSettings = false

    private var palette: Palette { Palette(scheme: scheme) }
    private var baseURL: URL? { GardenStore.url(from: serverURL) }
    private var latest: LatestReading? { store.latest }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                header.padding(.bottom, 14)
                temperaturePanel
                moistureSection.padding(.top, 22)
                historySection.padding(.top, 30)
                rawSection.padding(.top, 26)
                settingsButton.padding(.top, 20)
            }
            .frame(maxWidth: 540)
            .padding(.horizontal, 16)
            .padding(.vertical, 18)
            .frame(maxWidth: .infinity)
        }
        .background(palette.bg.ignoresSafeArea())
        .foregroundStyle(palette.ink)
        .refreshable { await refreshAll() }
        .task(id: serverURL) { await pollLatest() }
        .task(id: "\(serverURL)|\(rangeHours)") { await pollHistory() }
        .sheet(isPresented: $showSettings) {
            SettingsView(serverURL: $serverURL)
        }
    }

    // MARK: - Header

    private var header: some View {
        HStack(alignment: .firstTextBaseline, spacing: 12) {
            Text("🌱 IoT Garden")
                .font(.title3.bold())
                .lineLimit(1)
                .minimumScaleFactor(0.8)
                .frame(maxWidth: .infinity, alignment: .leading)
            Text(deviceText)
                .font(.footnote)
                .foregroundStyle(palette.muted)
                .multilineTextAlignment(.center)
                .monospacedDigit()
                .frame(maxWidth: 110)
            Text(status.text)
                .font(.footnote)
                .fontWeight(status.warning ? .bold : .regular)
                .foregroundStyle(status.warning ? palette.warn : palette.muted)
                .multilineTextAlignment(.trailing)
                .frame(maxWidth: .infinity, alignment: .trailing)
        }
    }

    private var deviceText: String {
        guard let latest else { return "" }
        // Older bridges don't send "device"; a battery reading means the ESP32
        let device = latest.device ?? (latest.batteryPct != nil ? "ESP32" : "")
        guard device == "ESP32" else { return device }
        return "ESP32: Battery \(percentText(latest.batteryPct))%"
    }

    private var status: (text: String, warning: Bool) {
        if let error = store.errorMessage { return (error, true) }
        guard let latest else { return ("Connecting to the Pi…", false) }
        guard latest.ts != nil, let age = latest.ageS else {
            return ("No readings yet. Check the garden sensor.", true)
        }
        if age > (latest.staleAfterS ?? 30) {
            return ("Last reading \(ago(age)) ago. Check the garden sensor.", true)
        }
        return ("Updated \(ago(age)) ago", false)
    }

    // MARK: - Temperature

    private var temperaturePanel: some View {
        HStack(alignment: .top, spacing: 4) {
            Text(latest?.tempC.map { String(format: "%.1f", $0) } ?? "--")
                .font(.system(size: 74, weight: .bold))
                .tracking(-2)
                .monospacedDigit()
            Text("°C")
                .font(.system(size: 26))
                .opacity(0.85)
                .padding(.top, 6)
        }
        .foregroundStyle(.white)
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(22)
        .background {
            // Fade only the colour, so the number doesn't cross-fade
            RoundedRectangle(cornerRadius: 22)
                .fill(bandColor)
                .animation(.easeInOut(duration: 0.8), value: bandColor)
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel("Temperature")
    }

    private var bandColor: Color {
        guard let temp = latest?.tempC else { return palette.muted }
        if temp >= (latest?.hotThreshold ?? 30) { return palette.hot }
        if temp >= (latest?.coolThreshold ?? 24) { return palette.ideal }
        return palette.cool
    }

    // MARK: - Soil moisture

    private var moistureSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .firstTextBaseline) {
                Text("Soil moisture").font(.headline)
                Spacer()
                HStack(alignment: .firstTextBaseline, spacing: 2) {
                    Text(percentText(latest?.moisturePct))
                        .font(.system(size: 32, weight: .bold))
                        .monospacedDigit()
                    Text("%").foregroundStyle(palette.muted)
                }
            }
            SoilBar(
                moisture: latest?.moisturePct,
                waterBelow: latest?.waterBelow,
                wetAbove: latest?.wetAbove,
                palette: palette
            )
            wateringCard
        }
    }

    @ViewBuilder
    private var wateringCard: some View {
        if let latest, let copy = wateringCopy(for: latest) {
            HStack(spacing: 0) {
                Rectangle().fill(copy.color).frame(width: 6)
                VStack(alignment: .leading, spacing: 2) {
                    Text(copy.title).font(.title3.bold())
                    Text(copy.detail).font(.subheadline).foregroundStyle(palette.muted)
                }
                .padding(.vertical, 12)
                .padding(.horizontal, 14)
                Spacer(minLength: 0)
            }
            .background {
                ZStack {
                    palette.panel
                    copy.color.opacity(0.14)
                }
            }
            .clipShape(RoundedRectangle(cornerRadius: 12))
            .padding(.top, 4)
            .accessibilityElement(children: .combine)
        }
    }

    private func wateringCopy(for reading: LatestReading) -> (title: String, detail: String, color: Color)? {
        let below = percentText(reading.waterBelow)
        let above = percentText(reading.wetAbove)
        switch reading.water {
        case "dry": return ("Needs watering", "Soil is below \(below)%. Water the herbs.", palette.dry)
        case "ok": return ("Okay, no water needed", "Water when the soil drops below \(below)%.", palette.ideal)
        case "wet": return ("Too wet", "Soil is above \(above)%. Hold off watering until it dries out.", palette.water)
        default: return nil
        }
    }

    // MARK: - History

    private var historySection: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("History").font(.headline)
                Spacer()
                Picker("Time range", selection: $rangeHours) {
                    Text("6 h").tag(6)
                    Text("24 h").tag(24)
                    Text("7 days").tag(168)
                }
                .pickerStyle(.segmented)
                .frame(maxWidth: 220)
            }
            chartCard("Temperature (°C)") {
                Chart(store.history) { point in
                    LineMark(x: .value("Time", point.date), y: .value("Temperature", point.tempC))
                        .foregroundStyle(palette.tempLine)
                        .lineStyle(StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round))
                }
            }
            chartCard("Soil moisture (%). Dashed lines mark the watering thresholds.") {
                Chart {
                    ForEach(store.history) { point in
                        LineMark(x: .value("Time", point.date), y: .value("Moisture", point.moisturePct))
                            .foregroundStyle(palette.water)
                            .lineStyle(StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round))
                    }
                    ForEach(thresholds, id: \.self) { value in
                        RuleMark(y: .value("Threshold", value))
                            .foregroundStyle(palette.muted)
                            .lineStyle(StrokeStyle(lineWidth: 1.5, dash: [5, 4]))
                    }
                }
            }
        }
    }

    private var thresholds: [Double] {
        [latest?.waterBelow, latest?.wetAbove].compactMap { $0 }
    }

    private func chartCard<C: View>(_ title: String, @ViewBuilder chart: () -> C) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title)
                .font(.subheadline)
                .foregroundStyle(palette.muted)
            if store.history.count < 2 {
                Text("The chart fills in as readings are logged.")
                    .font(.caption)
                    .foregroundStyle(palette.muted)
                    .frame(maxWidth: .infinity, minHeight: 150, alignment: .leading)
            } else {
                chart()
                    .chartYScale(domain: .automatic(includesZero: false))
                    .chartYAxis {
                        AxisMarks(position: .leading, values: .automatic(desiredCount: 4)) { _ in
                            AxisGridLine().foregroundStyle(palette.grid)
                            AxisValueLabel().foregroundStyle(palette.muted)
                        }
                    }
                    .chartXAxis {
                        AxisMarks(values: .automatic(desiredCount: 3)) { _ in
                            AxisValueLabel(format: xAxisFormat).foregroundStyle(palette.muted)
                        }
                    }
                    .frame(height: 150)
            }
        }
        .padding(12)
        .background(palette.panel, in: RoundedRectangle(cornerRadius: 14))
    }

    private var xAxisFormat: Date.FormatStyle {
        rangeHours >= 24 ? .dateTime.weekday(.abbreviated).hour() : .dateTime.hour().minute()
    }

    // MARK: - Raw readings

    private var rawSection: some View {
        DisclosureGroup("Raw sensor readings") {
            VStack(alignment: .leading, spacing: 8) {
                Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 4) {
                    rawRow("Temperature sensor", latest?.tempRaw)
                    rawRow("Moisture sensor", latest?.moistureRaw)
                    if latest?.batteryMv != nil {
                        rawRow("Battery (mV)", latest?.batteryMv)
                    }
                }
                Text("Use these values when filling in calibration.h. They update with each new reading.")
                    .font(.footnote)
            }
            .padding(.top, 8)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .font(.subheadline)
        .foregroundStyle(palette.muted)
        .tint(palette.muted)
    }

    private func rawRow(_ label: String, _ value: Double?) -> some View {
        GridRow {
            Text(label)
            Text(value.map { String(Int($0.rounded())) } ?? "--")
                .bold()
                .monospacedDigit()
                .foregroundStyle(palette.ink)
        }
    }

    private var settingsButton: some View {
        Button {
            showSettings = true
        } label: {
            Label("Pi address: \(serverURL)", systemImage: "gearshape")
                .font(.footnote)
        }
        .tint(palette.muted)
    }

    // MARK: - Loading

    private func pollLatest() async {
        store.reset()
        while !Task.isCancelled {
            if let baseURL { await store.loadLatest(from: baseURL) }
            try? await Task.sleep(for: .seconds(5))
        }
    }

    private func pollHistory() async {
        while !Task.isCancelled {
            if let baseURL { await store.loadHistory(from: baseURL, hours: rangeHours) }
            try? await Task.sleep(for: .seconds(60))
        }
    }

    private func refreshAll() async {
        guard let baseURL else { return }
        await store.loadLatest(from: baseURL)
        await store.loadHistory(from: baseURL, hours: rangeHours)
    }

    // MARK: - Formatting

    private func percentText(_ value: Double?) -> String {
        value.map { String(Int($0.rounded())) } ?? "--"
    }

    private func ago(_ seconds: Int) -> String {
        if seconds < 60 { return "\(seconds) s" }
        if seconds < 3600 { return "\(Int((Double(seconds) / 60).rounded())) min" }
        return "\(Int((Double(seconds) / 3600).rounded())) h"
    }
}

#Preview {
    ContentView()
}
