// Render one page of a PDF to a PNG with macOS PDFKit (no other tools needed).
//   swift tools/render_page.swift manual.pdf PAGE out.png [scale] [left top right bottom]
// The optional frame (fractions of the page) crops the output to that part of the page.
import Foundation
import PDFKit
import AppKit
let a = CommandLine.arguments
guard a.count >= 4, let doc = PDFDocument(url: URL(fileURLWithPath: a[1])), let page = doc.page(at: (Int(a[2]) ?? 1) - 1) else {
  FileHandle.standardError.write("usage: render_page.swift manual.pdf PAGE out.png [scale]\n".data(using: .utf8)!); exit(1)
}
let scale = CGFloat(a.count > 4 ? Double(a[4]) ?? 3 : 3)
let r = page.bounds(for: .mediaBox)
var fl = 0.0, ft = 0.0, fr = 1.0, fb = 1.0
if a.count >= 9 { fl = Double(a[5])!; ft = Double(a[6])!; fr = Double(a[7])!; fb = Double(a[8])! }
let w = Int(r.width * scale * CGFloat(fr - fl)), h = Int(r.height * scale * CGFloat(fb - ft))
let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: w, pixelsHigh: h, bitsPerSample: 8, samplesPerPixel: 4,
                           hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
let ctx = NSGraphicsContext.current!.cgContext
ctx.setFillColor(NSColor.white.cgColor); ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
// PDF space runs bottom-up: move the frame's bottom-left corner to the origin, then draw
ctx.translateBy(x: -CGFloat(fl) * r.width * scale, y: -CGFloat(1 - fb) * r.height * scale)
ctx.scaleBy(x: scale, y: scale)
page.draw(with: .mediaBox, to: ctx)
NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: a[3]))
print(w, h)
