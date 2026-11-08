// Run on macOS, including inside Henge: swift tests/test_pvg_video.swift
// Writes a generated NV12 pattern with Metal and verifies both planes from the CPU.
import CoreVideo
import Metal

let width = 640
let height = 480
let attributes = [
    kCVPixelBufferIOSurfacePropertiesKey: [:],
    kCVPixelBufferMetalCompatibilityKey: true
] as CFDictionary
var pixelBuffer: CVPixelBuffer?
guard let device = MTLCreateSystemDefaultDevice(),
      CVPixelBufferCreate(nil, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                          attributes, &pixelBuffer) == kCVReturnSuccess,
      let pixelBuffer else {
    fatalError("Metal or an NV12 pixel buffer is unavailable")
}

var cache: CVMetalTextureCache?
guard CVMetalTextureCacheCreate(nil, nil, device, nil, &cache) == kCVReturnSuccess,
      let cache else {
    fatalError("Could not create the Metal texture cache")
}
var lumaView: CVMetalTexture?
var chromaView: CVMetalTexture?
guard CVMetalTextureCacheCreateTextureFromImage(nil, cache, pixelBuffer, nil,
                                               .r8Unorm, width, height, 0, &lumaView) == kCVReturnSuccess,
      CVMetalTextureCacheCreateTextureFromImage(nil, cache, pixelBuffer, nil,
                                               .rg8Unorm, width / 2, height / 2, 1, &chromaView) == kCVReturnSuccess,
      let lumaView, let chromaView,
      let luma = CVMetalTextureGetTexture(lumaView),
      let chroma = CVMetalTextureGetTexture(chromaView) else {
    fatalError("Could not create Metal views of the video planes")
}

let library = try device.makeLibrary(source: """
    #include <metal_stdlib>
    using namespace metal;
    kernel void fill(texture2d<float, access::write> luma [[texture(0)]],
                     texture2d<float, access::write> chroma [[texture(1)]],
                     uint2 p [[thread_position_in_grid]]) {
        luma.write(float4(93.0 / 255.0, 0, 0, 1), p);
        if (p.x < chroma.get_width() && p.y < chroma.get_height())
            chroma.write(float4(71.0 / 255.0, 181.0 / 255.0, 0, 1), p);
    }
    kernel void fillPackedPlane(texture2d<uint, access::write> plane [[texture(0)]],
                                constant uint &value [[buffer(0)]],
                                uint2 p [[thread_position_in_grid]]) {
        plane.write(uint4(value), p);
    }
    """, options: nil)
guard let function = library.makeFunction(name: "fill"),
      let queue = device.makeCommandQueue(),
      let command = queue.makeCommandBuffer(),
      let encoder = command.makeComputeCommandEncoder() else {
    fatalError("Could not encode the video plane writes")
}
let pipeline = try device.makeComputePipelineState(function: function)
encoder.setComputePipelineState(pipeline)
encoder.setTexture(luma, index: 0)
encoder.setTexture(chroma, index: 1)
encoder.dispatchThreads(MTLSize(width: width, height: height, depth: 1),
                        threadsPerThreadgroup: MTLSize(width: 16, height: 16, depth: 1))
encoder.endEncoding()
command.commit()
func check(_ label: String, after command: MTLCommandBuffer, luma expectedLuma: UInt8,
           cb: UInt8 = 71, cr: UInt8 = 181) -> Int {
    command.waitUntilCompleted()
    guard command.status == .completed,
          CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly) == kCVReturnSuccess else {
        fatalError("Could not read the GPU result \(String(describing: command.error))")
    }
    defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly) }
    var errors = 0
    for plane in 0..<2 {
        let bytes = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, plane)!.assumingMemoryBound(to: UInt8.self)
        let stride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, plane)
        let rows = CVPixelBufferGetHeightOfPlane(pixelBuffer, plane)
        var mismatches = 0
        for y in 0..<rows {
            for x in 0..<width {
                let expected = plane == 0 ? expectedLuma : (x.isMultiple(of: 2) ? cb : cr)
                if bytes[y * stride + x] != expected { mismatches += 1 }
            }
        }
        errors += mismatches
        print("\(label) plane \(plane) had \(mismatches) mismatches across \(rows * width) bytes")
    }
    return errors
}

var errors = check("Standard NV12", after: command, luma: 93)
guard let surface = CVPixelBufferGetIOSurface(pixelBuffer)?.takeUnretainedValue(),
      let packedFunction = library.makeFunction(name: "fillPackedPlane") else {
    fatalError("Could not prepare the packed video writes")
}
let packedPipeline = try device.makeComputePipelineState(function: packedFunction)
func writePackedPlane(_ index: Int, value: UInt32) -> MTLCommandBuffer {
    let rows = CVPixelBufferGetHeightOfPlane(pixelBuffer, index)
    let packed = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r32Uint, width: width / 4,
                                                         height: rows, mipmapped: false)
    packed.storageMode = .shared
    packed.usage = .shaderWrite
    guard let plane = device.makeTexture(descriptor: packed, iosurface: surface, plane: index),
          let command = queue.makeCommandBuffer(),
          let encoder = command.makeComputeCommandEncoder() else {
        fatalError("Could not encode the packed video writes")
    }
    var word = value
    encoder.setComputePipelineState(packedPipeline)
    encoder.setTexture(plane, index: 0)
    encoder.setBytes(&word, length: MemoryLayout<UInt32>.size, index: 0)
    encoder.dispatchThreads(MTLSize(width: width / 4, height: rows, depth: 1),
                            threadsPerThreadgroup: MTLSize(width: 16, height: 16, depth: 1))
    encoder.endEncoding()
    command.commit()
    return command
}
errors += check("Packed luma", after: writePackedPlane(0, value: 0x60606060), luma: 96)
errors += check("Packed chroma", after: writePackedPlane(1, value: 0x9b679b67), luma: 96, cb: 103, cr: 155)
exit(errors == 0 ? 0 : 1)
