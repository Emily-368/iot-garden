import Foundation
import Observation

/// Loads readings from the Pi's dashboard API.
@Observable
final class GardenStore {
    var latest: LatestReading?
    var history: [HistoryPoint] = []
    var errorMessage: String?

    private let decoder: JSONDecoder = {
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        return decoder
    }()

    /// Turns what the user typed into a base URL, adding http:// if missing.
    static func url(from text: String) -> URL? {
        var trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return nil }
        if !trimmed.contains("://") { trimmed = "http://" + trimmed }
        guard let url = URL(string: trimmed),
              let scheme = url.scheme?.lowercased(), ["http", "https"].contains(scheme),
              url.host() != nil else { return nil }
        return url
    }

    func reset() {
        latest = nil
        history = []
        errorMessage = nil
    }

    func loadLatest(from base: URL) async {
        do {
            latest = try await fetch(LatestReading.self, from: base.appending(path: "api/latest"))
            errorMessage = nil
        } catch {
            if !Task.isCancelled { errorMessage = "Can't reach the Pi" }
        }
    }

    func loadHistory(from base: URL, hours: Int) async {
        let url = base.appending(path: "api/history")
            .appending(queryItems: [URLQueryItem(name: "hours", value: String(hours))])
        do {
            let response = try await fetch(HistoryResponse.self, from: url)
            history = response.points.compactMap { row in
                guard row.count >= 3, let time = row[0], let temp = row[1], let moisture = row[2] else {
                    return nil
                }
                return HistoryPoint(date: Date(timeIntervalSince1970: time), tempC: temp, moisturePct: moisture)
            }
        } catch {
            // Keep the last chart, like the web dashboard
        }
    }

    private func fetch<T: Decodable>(_ type: T.Type, from url: URL) async throws -> T {
        var request = URLRequest(url: url)
        request.cachePolicy = .reloadIgnoringLocalCacheData
        request.timeoutInterval = 8
        let (data, response) = try await URLSession.shared.data(for: request)
        guard let http = response as? HTTPURLResponse, http.statusCode == 200 else {
            throw URLError(.badServerResponse)
        }
        return try decoder.decode(type, from: data)
    }
}
