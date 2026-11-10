import SwiftUI

/// Striped soil bar with the moisture level and watering threshold marks.
struct SoilBar: View {
    let moisture: Double?
    let waterBelow: Double?
    let wetAbove: Double?
    let palette: Palette

    private let barHeight: CGFloat = 18

    var body: some View {
        GeometryReader { geo in
            let width = geo.size.width
            ZStack(alignment: .topLeading) {
                Canvas { context, size in
                    let bar = Path(roundedRect: CGRect(origin: .zero, size: size), cornerRadius: size.height / 2)
                    context.clip(to: bar)
                    context.fill(bar, with: .color(palette.soil.opacity(0.9)))

                    // Diagonal stripes, like the web dashboard
                    var stripes = Path()
                    var x = -size.height
                    while x < size.width {
                        stripes.move(to: CGPoint(x: x, y: size.height))
                        stripes.addLine(to: CGPoint(x: x + 6, y: size.height))
                        stripes.addLine(to: CGPoint(x: x + 6 + size.height, y: 0))
                        stripes.addLine(to: CGPoint(x: x + size.height, y: 0))
                        stripes.closeSubpath()
                        x += 12
                    }
                    context.fill(stripes, with: .color(palette.bg.opacity(0.2)))

                    let level = min(max(moisture ?? 0, 0), 100) / 100
                    context.fill(
                        Path(CGRect(x: 0, y: 0, width: size.width * level, height: size.height)),
                        with: .color(palette.water)
                    )
                    for mark in [waterBelow, wetAbove].compactMap({ $0 }) {
                        let markX = size.width * mark / 100
                        context.fill(
                            Path(CGRect(x: markX - 1, y: 0, width: 2, height: size.height)),
                            with: .color(.white.opacity(0.85))
                        )
                    }
                }
                .frame(height: barHeight)
                .animation(.easeInOut(duration: 0.8), value: moisture)

                if let waterBelow {
                    markLabel("Water below \(Int(waterBelow))%", at: waterBelow, width: width)
                }
                if let wetAbove {
                    markLabel("Too wet above \(Int(wetAbove))%", at: wetAbove, width: width)
                }
            }
        }
        .frame(height: barHeight + 20)
        .accessibilityHidden(true)
    }

    private func markLabel(_ text: String, at percent: Double, width: CGFloat) -> some View {
        Text(text)
            .font(.caption2)
            .foregroundStyle(palette.muted)
            .fixedSize()
            .position(x: width * min(max(percent, 15), 85) / 100, y: barHeight + 10)
    }
}
