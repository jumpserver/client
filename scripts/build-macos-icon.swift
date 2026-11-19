// Rebuild the Dock/Finder icon without changing the in-app brand artwork.
// Usage: swift scripts/build-macos-icon.swift resources/icon.png resources
import Foundation
import CoreGraphics
import ImageIO

guard CommandLine.arguments.count == 3 else {
    fatalError("Usage: build-macos-icon.swift source.png output-directory")
}
let sourceURL = URL(fileURLWithPath: CommandLine.arguments[1])
let output = URL(fileURLWithPath: CommandLine.arguments[2], isDirectory: true)
guard let source = CGImageSourceCreateWithURL(sourceURL as CFURL, nil),
      let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else {
    fatalError("Cannot read source icon")
}
let temporary = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
let iconset = temporary.appendingPathComponent("OSMClient.iconset")
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
defer { try? FileManager.default.removeItem(at: temporary) }

func render(_ pixels: Int, to url: URL) throws {
    guard let context = CGContext(data: nil, width: pixels, height: pixels,
                                  bitsPerComponent: 8, bytesPerRow: 0,
                                  space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else {
        fatalError("Cannot create icon canvas")
    }
    context.interpolationQuality = .high
    let canvas = CGFloat(pixels)
    let tile = CGRect(x: canvas * 0.1, y: canvas * 0.1, width: canvas * 0.8, height: canvas * 0.8)
    let radius = canvas * 0.21
    let outline = CGPath(roundedRect: tile, cornerWidth: radius, cornerHeight: radius, transform: nil)
    context.saveGState()
    context.setShadow(offset: CGSize(width: 0, height: -canvas * 0.018), blur: canvas * 0.035,
                      color: CGColor(gray: 0, alpha: 0.25))
    context.setFillColor(gray: 0.76, alpha: 1)
    context.addPath(outline)
    context.fillPath()
    context.restoreGState()

    context.saveGState()
    context.addPath(outline)
    context.clip()
    guard let gradient = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(),
                                    colors: [CGColor(gray: 0.88, alpha: 1), CGColor(gray: 0.72, alpha: 1)] as CFArray,
                                    locations: [0, 1]) else {
        fatalError("Cannot create icon background")
    }
    context.drawLinearGradient(gradient, start: CGPoint(x: tile.midX, y: tile.maxY),
                               end: CGPoint(x: tile.midX, y: tile.minY), options: [])
    context.setStrokeColor(gray: 1, alpha: 0.55)
    context.setLineWidth(canvas * 0.012)
    context.addPath(outline)
    context.strokePath()
    context.restoreGState()

    let scale = canvas * 0.55 / CGFloat(max(image.width, image.height))
    let size = CGSize(width: CGFloat(image.width) * scale, height: CGFloat(image.height) * scale)
    context.draw(image, in: CGRect(x: (CGFloat(pixels) - size.width) / 2,
                                   y: (CGFloat(pixels) - size.height) / 2,
                                   width: size.width, height: size.height))
    guard let rendered = context.makeImage(),
          let destination = CGImageDestinationCreateWithURL(url as CFURL, "public.png" as CFString, 1, nil) else {
        fatalError("Cannot write icon")
    }
    CGImageDestinationAddImage(destination, rendered, nil)
    guard CGImageDestinationFinalize(destination) else { fatalError("Cannot finish icon") }
}

for points in [16, 32, 128, 256, 512] {
    for density in [1, 2] {
        let suffix = density == 2 ? "@2x" : ""
        try render(points * density, to: iconset.appendingPathComponent("icon_\(points)x\(points)\(suffix).png"))
    }
}
let process = Process()
process.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
process.arguments = ["-c", "icns", "-o", output.appendingPathComponent("icon.icns").path, iconset.path]
try process.run()
process.waitUntilExit()
guard process.terminationStatus == 0 else { fatalError("iconutil failed") }
print("Generated macOS ICNS with rounded gray background")
