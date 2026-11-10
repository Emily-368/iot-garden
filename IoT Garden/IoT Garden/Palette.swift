import SwiftUI

/// The web dashboard's colours, in light and dark versions.
struct Palette {
    let scheme: ColorScheme

    var bg: Color       { pick(0xE9EEE6, 0x121612) }
    var panel: Color    { pick(0xF8FAF6, 0x1B211B) }
    var ink: Color      { pick(0x1C261C, 0xE5EAE1) }
    var muted: Color    { pick(0x5A6657, 0x93A08F) }
    var grid: Color     { pick(0xD3DBCD, 0x2B342B) }
    var hot: Color      { pick(0xC23B2B, 0xD9503F) }
    var ideal: Color    { pick(0x2E8547, 0x3A9A57) }
    var cool: Color     { pick(0x2D5FB3, 0x4073C7) }
    var soil: Color     { pick(0x7A5A3F, 0xA07D5E) }
    var water: Color    { pick(0x2B7889, 0x4AAABD) }
    var tempLine: Color { pick(0xB5562A, 0xE0834F) }
    var warn: Color     { pick(0x9A6512, 0xE2A74A) }
    var dry: Color      { pick(0xB8650F, 0xE8963C) }

    private func pick(_ light: UInt32, _ dark: UInt32) -> Color {
        Color(hex: scheme == .dark ? dark : light)
    }
}

extension Color {
    init(hex: UInt32) {
        self.init(
            red: Double((hex >> 16) & 0xFF) / 255,
            green: Double((hex >> 8) & 0xFF) / 255,
            blue: Double(hex & 0xFF) / 255
        )
    }
}
