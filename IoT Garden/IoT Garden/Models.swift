import Foundation

/// Response from the bridge's `GET /api/latest`. Every field is optional:
/// the bridge returns only its settings until the first reading arrives.
nonisolated struct LatestReading: Decodable, Equatable {
    var ts: Int?
    var tempC: Double?
    var moisturePct: Double?
    var water: String?
    var waterBelow: Double?
    var wetAbove: Double?
    var tempRaw: Double?
    var moistureRaw: Double?
    var batteryMv: Double?
    var batteryPct: Double?
    var rssi: Double?
    var awakeMs: Double?
    var ageS: Int?

    // Dashboard settings (sent by newer bridges)
    var device: String?
    var staleAfterS: Int?
    var hotThreshold: Double?
    var coolThreshold: Double?
}

/// Response from `GET /api/history`: rows of [time, temp_c, moisture_pct].
nonisolated struct HistoryResponse: Decodable {
    var points: [[Double?]]
}

nonisolated struct HistoryPoint: Identifiable, Equatable {
    let date: Date
    let tempC: Double
    let moisturePct: Double
    var id: Date { date }
}
