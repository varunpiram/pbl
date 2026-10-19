import AppKit

let output = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
let updating = CommandLine.arguments.contains("--update")
let name = updating ? "UpdateIcon" : "AppIcon"
let iconset = output.appendingPathComponent(name + ".iconset", isDirectory: true)
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
let cream = NSColor(srgbRed: 245 / 255, green: 240 / 255, blue: 230 / 255, alpha: 1)
let background = updating ? NSColor(srgbRed: 174 / 255, green: 57 / 255, blue: 48 / 255, alpha: 1) :
    cream
let foreground = updating ? cream : NSColor(
    srgbRed: 51 / 255,
    green: 48 / 255,
    blue: 43 / 255,
    alpha: 1
)
for size in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let pixels = size * scale
        let bitmap = NSBitmapImageRep(
            bitmapDataPlanes: nil,
            pixelsWide: pixels,
            pixelsHigh: pixels,
            bitsPerSample: 8,
            samplesPerPixel: 4,
            hasAlpha: true,
            isPlanar: false,
            colorSpaceName: .deviceRGB,
            bytesPerRow: 0,
            bitsPerPixel: 0
        )!
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
        let transform = AffineTransform(scale: CGFloat(pixels) / 1024)
        (transform as NSAffineTransform).concat()
        background.setFill()
        NSBezierPath(
            roundedRect: NSRect(x: 100, y: 100, width: 824, height: 824),
            xRadius: 184,
            yRadius: 184
        ).fill()
        foreground.setStroke()
        let ring = NSBezierPath()
        ring.appendArc(
            withCenter: NSPoint(x: 512, y: 512),
            radius: 218,
            startAngle: 108,
            endAngle: 432
        )
        ring.lineWidth = 44
        ring.lineCapStyle = .round
        ring.stroke()
        NSGraphicsContext.restoreGraphicsState()
        let data = bitmap.representation(using: .png, properties: [:])!
        let suffix = scale == 2 ? "@2x" : ""
        try data.write(to: iconset.appendingPathComponent("icon_\(size)x\(size)\(suffix).png"))
        if pixels == 1024 {
            try data.write(to: output.appendingPathComponent(name + ".png"))
        }
    }
}
