// Turns artwork into a macOS app icon: crops to the dark artwork (dropping a light background,
// even a checkerboard baked into a JPEG), then centres it in a 1024 px canvas as a 824 px rounded
// square with transparent corners (Apple's icon grid). Only works for dark artwork.
// Usage: swift tools/make_icon.swift <artwork.png> resources/AppIcon.png

import AppKit

let args = CommandLine.arguments
let source = NSImage(contentsOfFile: args[1])!
var rect = NSRect(origin: .zero, size: source.size)
let cg = source.cgImage(forProposedRect: &rect, context: nil, hints: nil)!
let w = cg.width, h = cg.height
let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                    space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
let px = ctx.data!.bindMemory(to: UInt8.self, capacity: w * h * 4)

// Bounding box of the dark artwork (rows counted from the top). Light pixels are background.
var minX = w, minY = h, maxX = -1, maxY = -1
for y in 0..<h { for x in 0..<w {
    let i = ((h - 1 - y) * w + x) * 4
    let r = Int(px[i]), g = Int(px[i+1]), b = Int(px[i+2]), a = Int(px[i+3])
    if a > 40 && r < 110 && g < 110 && b < 110 { minX = min(minX, x); maxX = max(maxX, x); minY = min(minY, y); maxY = max(maxY, y) }
}}
print("content box:", minX, minY, maxX, maxY)
let side = max(maxX - minX + 1, maxY - minY + 1)
let crop = cg.cropping(to: CGRect(x: minX, y: minY, width: maxX - minX + 1, height: maxY - minY + 1))!

// macOS app icon grid: 1024 canvas, 824 px rounded square centred, corner radius ~185.
let size = 1024, art: CGFloat = 824
let out = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: size * 4,
                    space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
out.interpolationQuality = .high
let box = CGRect(x: (CGFloat(size) - art) / 2, y: (CGFloat(size) - art) / 2, width: art, height: art)
out.addPath(CGPath(roundedRect: box, cornerWidth: 185, cornerHeight: 185, transform: nil))
out.clip()
out.setFillColor(CGColor(red: 0.13, green: 0.13, blue: 0.14, alpha: 1))
out.fill(box)
let scale = art / CGFloat(side)
let dw = CGFloat(crop.width) * scale, dh = CGFloat(crop.height) * scale
out.draw(crop, in: CGRect(x: box.midX - dw / 2, y: box.midY - dh / 2, width: dw, height: dh).insetBy(dx: -8, dy: -8))
let rep = NSBitmapImageRep(cgImage: out.makeImage()!)
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: args[2]))
print("wrote", args[2])
