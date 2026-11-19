import CoreVideo
import Foundation
import Metal

/// The SDR colour contract (plan §6.7) as GPU kernels. Host and client compile the *same* source, so the
/// forward (BGRA -> NV12) and inverse (NV12 -> RGBA) matrices, range handling and chroma siting cannot drift.
///
///   capture   BGRA8, interpreted as sRGB (sRGB/Rec.709 primaries, sRGB transfer)
///   convert   BT.709 matrix, full range, chroma siting left (HEVC default, chroma_location 0)
///   signal    primaries 1, transfer 13, matrix 1, video_full_range_flag 1, chroma location 0
///   decode    full-range bi-planar output matching the VUI (no implicit conversion)
///   render    inverse BT.709 into an sRGB texture; values stay gamma-encoded until the sampler decodes them
public enum ColorPipelineShaders {
    public static let source = #"""
    #include <metal_stdlib>
    using namespace metal;

    struct YUVToRGBParams {
        float yOffset;      // 0 (full) or 16/255 (video)
        float yScale;       // 1 (full) or 255/219
        float cOffset;      // 128/255
        float cScale;       // 1 (full) or 255/224
        float crR;          // 2(1-Kr)
        float cbG;          // 2Kb(1-Kb)/Kg
        float crG;          // 2Kr(1-Kr)/Kg
        float cbB;          // 2(1-Kb)
        float2 chromaOffset; // luma->chroma sample offset in chroma texels (siting)
        uint linearOut;     // 1: write linear light (target is an sRGB texture written directly)
        uint pad;
    };

    struct RGBToYUVParams {
        float kr; float kg; float kb;
        float yOffset; float yScale;     // stored = yOffset + Y * yScale
        float cOffset; float cScale;     // stored = cOffset + C * cScale
        float cbDiv; float crDiv;        // Cb = (B - Y) / cbDiv, Cr = (R - Y) / crDiv
        uint siting;                     // 0 left, 1 center
    };

    static inline float srgb_to_linear(float c) {
        return c <= 0.04045f ? c / 12.92f : pow((c + 0.055f) / 1.055f, 2.4f);
    }

    kernel void wvd_nv12_to_rgba(texture2d<float, access::read>   yTex   [[texture(0)]],
                                 texture2d<float, access::sample> cTex   [[texture(1)]],
                                 texture2d<float, access::write>  outTex [[texture(2)]],
                                 constant YUVToRGBParams& p [[buffer(0)]],
                                 uint2 gid [[thread_position_in_grid]]) {
        if (gid.x >= outTex.get_width() || gid.y >= outTex.get_height()) { return; }
        constexpr sampler bilinear(coord::normalized, address::clamp_to_edge, filter::linear);
        float y = (yTex.read(gid).r - p.yOffset) * p.yScale;
        float2 cc = (float2(gid) * 0.5f + p.chromaOffset + 0.5f) / float2(cTex.get_width(), cTex.get_height());
        float2 c = (cTex.sample(bilinear, cc).rg - p.cOffset) * p.cScale;
        float3 rgb = saturate(float3(y + p.crR * c.y,
                                     y - p.cbG * c.x - p.crG * c.y,
                                     y + p.cbB * c.x));
        if (p.linearOut != 0) {
            rgb = float3(srgb_to_linear(rgb.r), srgb_to_linear(rgb.g), srgb_to_linear(rgb.b));
        }
        outTex.write(float4(rgb, 1.0f), gid);
    }

    // One thread per chroma sample (2x2 luma block). Horizontal chroma filter [1 2 1]/4 centred on the
    // even luma column (left siting); vertical 2-tap average (chroma vertically between the two rows).
    kernel void wvd_bgra_to_nv12(texture2d<float, access::read>  src  [[texture(0)]],
                                 texture2d<float, access::write> yOut [[texture(1)]],
                                 texture2d<float, access::write> cOut [[texture(2)]],
                                 constant RGBToYUVParams& p [[buffer(0)]],
                                 uint2 gid [[thread_position_in_grid]]) {
        uint cw = cOut.get_width(), ch = cOut.get_height();
        if (gid.x >= cw || gid.y >= ch) { return; }
        int W = int(src.get_width()), H = int(src.get_height());
        int x0 = int(gid.x) * 2, y0 = int(gid.y) * 2;
        float3 k = float3(p.kr, p.kg, p.kb);
        float3 csum = float3(0.0f);
        for (int dy = 0; dy < 2; dy++) {
            int yy = min(y0 + dy, H - 1);
            float3 a = src.read(uint2(max(x0 - 1, 0), yy)).rgb;
            float3 b = src.read(uint2(min(x0, W - 1), yy)).rgb;
            float3 c = src.read(uint2(min(x0 + 1, W - 1), yy)).rgb;
            if (y0 + dy < int(yOut.get_height())) {
                if (x0 < int(yOut.get_width()))     { yOut.write(float4(p.yOffset + dot(b, k) * p.yScale), uint2(x0, y0 + dy)); }
                if (x0 + 1 < int(yOut.get_width())) { yOut.write(float4(p.yOffset + dot(c, k) * p.yScale), uint2(x0 + 1, y0 + dy)); }
            }
            csum += (p.siting == 0) ? (0.25f * a + 0.5f * b + 0.25f * c) : (0.5f * b + 0.5f * c);
        }
        float3 rgb = csum * 0.5f;
        float Y = dot(rgb, k);
        float cb = (rgb.b - Y) / p.cbDiv;
        float cr = (rgb.r - Y) / p.crDiv;
        cOut.write(float4(p.cOffset + cb * p.cScale, p.cOffset + cr * p.cScale, 0.0f, 0.0f), gid);
    }

    // Test pattern for colour/orientation validation on the headset (plan §6.7 validation set).
    kernel void wvd_test_pattern(texture2d<float, access::write> outTex [[texture(0)]],
                                 constant uint& frame [[buffer(0)]],
                                 uint2 gid [[thread_position_in_grid]]) {
        uint W = outTex.get_width(), H = outTex.get_height();
        if (gid.x >= W || gid.y >= H) { return; }
        float u = float(gid.x) / float(W), v = float(gid.y) / float(H);
        float3 c;
        if (v < 0.25f) {                 // neutral ramp, 32 steps
            c = float3(floor(u * 32.0f) / 31.0f);
        } else if (v < 0.5f) {           // saturated bars
            uint bar = uint(u * 8.0f);
            c = float3((bar >> 2) & 1, (bar >> 1) & 1, bar & 1);
        } else if (v < 0.75f) {          // 1-px patterns: alternating columns / rows / checker
            uint x = gid.x, y = gid.y;
            float t = (u < 0.33f) ? float(x & 1) : (u < 0.66f) ? float(y & 1) : float((x ^ y) & 1);
            c = float3(t);
        } else {                         // near-black and near-white steps
            float s = floor(u * 16.0f);
            c = (gid.y < H * 7 / 8) ? float3(s / 255.0f) : float3(1.0f - s / 255.0f);
        }
        // Orientation marker: red square top-left.
        if (gid.x < W / 32 && gid.y < W / 32) { c = float3(1, 0, 0); }
        outTex.write(float4(c, 1.0f), gid);
    }
    """#
}

/// Matrix constants for the contract.
public struct YCbCrMatrix: Sendable, Equatable {
    public var kr: Float
    public var kb: Float
    public var kg: Float { 1 - kr - kb }

    public static let bt709 = YCbCrMatrix(kr: 0.2126, kb: 0.0722)
    public static let bt601 = YCbCrMatrix(kr: 0.299, kb: 0.114)

    public static func forCodePoint(_ matrix: UInt32) -> YCbCrMatrix {
        switch matrix {
        case 5, 6: return .bt601
        default: return .bt709
        }
    }
}

public final class ColorPipeline: @unchecked Sendable {
    public enum PipelineError: Error {
        case noDevice
        case library(String)
        case textureCache(CVReturn)
        case texture(CVReturn)
        case unsupportedPixelFormat(OSType)
    }

    public let device: MTLDevice
    public let queue: MTLCommandQueue
    let toRGB: MTLComputePipelineState
    let toNV12: MTLComputePipelineState
    let testPattern: MTLComputePipelineState
    let textureCache: CVMetalTextureCache

    /// Compiles the kernels once. Construct early (e.g. at app launch) so the first frame pays nothing.
    public init(device: MTLDevice? = MTLCreateSystemDefaultDevice()) throws {
        guard let device else { throw PipelineError.noDevice }
        self.device = device
        guard let queue = device.makeCommandQueue() else { throw PipelineError.noDevice }
        queue.label = "wvd.color"
        self.queue = queue
        let options = MTLCompileOptions()
        options.mathMode = .safe
        let library: MTLLibrary
        do {
            library = try device.makeLibrary(source: ColorPipelineShaders.source, options: options)
        } catch {
            throw PipelineError.library(String(describing: error))
        }
        func pipeline(_ name: String) throws -> MTLComputePipelineState {
            guard let fn = library.makeFunction(name: name) else { throw PipelineError.library("missing \(name)") }
            return try device.makeComputePipelineState(function: fn)
        }
        toRGB = try pipeline("wvd_nv12_to_rgba")
        toNV12 = try pipeline("wvd_bgra_to_nv12")
        testPattern = try pipeline("wvd_test_pattern")
        var cache: CVMetalTextureCache?
        let status = CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, device, nil, &cache)
        guard status == kCVReturnSuccess, let cache else { throw PipelineError.textureCache(status) }
        textureCache = cache
    }

    /// Wraps one plane of an IOSurface-backed pixel buffer as a Metal texture (zero copy).
    public func texture(for buffer: CVPixelBuffer, plane: Int, format: MTLPixelFormat) throws -> (MTLTexture, CVMetalTexture) {
        let w = CVPixelBufferIsPlanar(buffer) ? CVPixelBufferGetWidthOfPlane(buffer, plane) : CVPixelBufferGetWidth(buffer)
        let h = CVPixelBufferIsPlanar(buffer) ? CVPixelBufferGetHeightOfPlane(buffer, plane) : CVPixelBufferGetHeight(buffer)
        var cvTex: CVMetalTexture?
        let status = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, textureCache, buffer, nil, format, w, h, plane, &cvTex)
        guard status == kCVReturnSuccess, let cvTex, let tex = CVMetalTextureGetTexture(cvTex) else { throw PipelineError.texture(status) }
        return (tex, cvTex)
    }

    func threadgroups(_ pso: MTLComputePipelineState) -> MTLSize {
        let w = pso.threadExecutionWidth
        return MTLSize(width: w, height: max(1, pso.maxTotalThreadsPerThreadgroup / w), depth: 1)
    }

    static func chromaOffset(location: UInt32) -> SIMD2<Float> {
        // H.273 chroma_sample_loc_type: 0 left, 1 center, 2 top-left, 3 top, 4 bottom-left, 5 bottom.
        switch location {
        case 1: return SIMD2(-0.25, -0.25)
        case 2: return SIMD2(0, 0)
        case 3: return SIMD2(-0.25, 0)
        case 4: return SIMD2(0, -0.5)
        case 5: return SIMD2(-0.25, -0.5)
        default: return SIMD2(0, -0.25)
        }
    }

    /// Metal views of a bi-planar 4:2:0 buffer. Keep it alive until the command buffer using it completes.
    public struct NV12Planes: @unchecked Sendable {
        public let y: MTLTexture
        public let cbcr: MTLTexture
        let keep: [CVMetalTexture]
    }

    /// The fallible half of the conversion, so callers can do it before committing to a destination
    /// (e.g. before `LowLevelTexture.replace(using:)`, after which the texture must be written).
    public func nv12Planes(_ buffer: CVPixelBuffer) throws -> NV12Planes {
        let (yTex, yCV) = try texture(for: buffer, plane: 0, format: .r8Unorm)
        let (cTex, cCV) = try texture(for: buffer, plane: 1, format: .rg8Unorm)
        return NV12Planes(y: yTex, cbcr: cTex, keep: [yCV, cCV])
    }

    /// Encodes NV12 -> RGBA into `target` (level 0). `linearOut` must be true when `target` is an sRGB
    /// format written directly, false when writing gamma-encoded values (unorm view of an sRGB texture).
    /// Returns the CoreVideo textures; keep them alive until the command buffer completes.
    public func encodeNV12ToRGBA(_ buffer: CVPixelBuffer, color: ColorInfo, into target: MTLTexture, linearOut: Bool,
                                 commandBuffer: MTLCommandBuffer) throws -> [CVMetalTexture] {
        let planes = try nv12Planes(buffer)
        encodeNV12ToRGBA(planes, color: color, into: target, linearOut: linearOut, commandBuffer: commandBuffer)
        return planes.keep
    }

    /// Returns false if Metal could not create an encoder (nothing was written).
    @discardableResult
    public func encodeNV12ToRGBA(_ planes: NV12Planes, color: ColorInfo, into target: MTLTexture, linearOut: Bool,
                                 commandBuffer: MTLCommandBuffer) -> Bool {
        let yTex = planes.y, cTex = planes.cbcr
        let m = YCbCrMatrix.forCodePoint(color.matrix)
        struct Params {
            var yOffset: Float, yScale: Float, cOffset: Float, cScale: Float
            var crR: Float, cbG: Float, crG: Float, cbB: Float
            var chromaOffset: SIMD2<Float>
            var linearOut: UInt32, pad: UInt32
        }
        let full = color.fullRange
        var p = Params(yOffset: full ? 0 : 16.0 / 255.0, yScale: full ? 1 : 255.0 / 219.0,
                       cOffset: 128.0 / 255.0, cScale: full ? 1 : 255.0 / 224.0,
                       crR: 2 * (1 - m.kr), cbG: 2 * m.kb * (1 - m.kb) / m.kg, crG: 2 * m.kr * (1 - m.kr) / m.kg, cbB: 2 * (1 - m.kb),
                       chromaOffset: Self.chromaOffset(location: color.chromaLocation), linearOut: linearOut ? 1 : 0, pad: 0)
        guard let enc = commandBuffer.makeComputeCommandEncoder() else { return false }
        enc.label = "wvd.nv12->rgba"
        enc.setComputePipelineState(toRGB)
        enc.setTexture(yTex, index: 0)
        enc.setTexture(cTex, index: 1)
        enc.setTexture(target, index: 2)
        enc.setBytes(&p, length: MemoryLayout<Params>.stride, index: 0)
        enc.dispatchThreads(MTLSize(width: target.width, height: target.height, depth: 1), threadsPerThreadgroup: threadgroups(toRGB))
        enc.endEncoding()
        return true
    }

    /// Encodes BGRA -> NV12 (host preprocessing, plan §4.6 "shared compute").
    public func encodeBGRAToNV12(source: CVPixelBuffer, destination: CVPixelBuffer, color: ColorInfo,
                                 commandBuffer: MTLCommandBuffer) throws -> [CVMetalTexture] {
        let (src, srcCV) = try texture(for: source, plane: 0, format: .bgra8Unorm)
        let (yTex, yCV) = try texture(for: destination, plane: 0, format: .r8Unorm)
        let (cTex, cCV) = try texture(for: destination, plane: 1, format: .rg8Unorm)
        try encodeBGRAToNV12(source: src, y: yTex, cbcr: cTex, color: color, commandBuffer: commandBuffer)
        return [srcCV, yCV, cCV]
    }

    public func encodeBGRAToNV12(source: MTLTexture, y: MTLTexture, cbcr: MTLTexture, color: ColorInfo, commandBuffer: MTLCommandBuffer) throws {
        let m = YCbCrMatrix.forCodePoint(color.matrix)
        struct Params {
            var kr: Float, kg: Float, kb: Float
            var yOffset: Float, yScale: Float, cOffset: Float, cScale: Float
            var cbDiv: Float, crDiv: Float
            var siting: UInt32
        }
        let full = color.fullRange
        var p = Params(kr: m.kr, kg: m.kg, kb: m.kb,
                       yOffset: full ? 0 : 16.0 / 255.0, yScale: full ? 1 : 219.0 / 255.0,
                       cOffset: 128.0 / 255.0, cScale: full ? 1 : 224.0 / 255.0,
                       cbDiv: 2 * (1 - m.kb), crDiv: 2 * (1 - m.kr), siting: color.chromaLocation == 1 ? 1 : 0)
        guard let enc = commandBuffer.makeComputeCommandEncoder() else { return }
        enc.label = "wvd.bgra->nv12"
        enc.setComputePipelineState(toNV12)
        enc.setTexture(source, index: 0)
        enc.setTexture(y, index: 1)
        enc.setTexture(cbcr, index: 2)
        enc.setBytes(&p, length: MemoryLayout<Params>.stride, index: 0)
        enc.dispatchThreads(MTLSize(width: cbcr.width, height: cbcr.height, depth: 1), threadsPerThreadgroup: threadgroups(toNV12))
        enc.endEncoding()
    }

    public func encodeTestPattern(into target: MTLTexture, frame: UInt32, commandBuffer: MTLCommandBuffer) {
        guard let enc = commandBuffer.makeComputeCommandEncoder() else { return }
        var f = frame
        enc.setComputePipelineState(testPattern)
        enc.setTexture(target, index: 0)
        enc.setBytes(&f, length: 4, index: 0)
        enc.dispatchThreads(MTLSize(width: target.width, height: target.height, depth: 1), threadsPerThreadgroup: threadgroups(testPattern))
        enc.endEncoding()
    }

    /// Full mip chain for text legibility at a distance (plan §6.4: mips + anisotropic sampling).
    @discardableResult
    public func encodeMipmaps(_ texture: MTLTexture, commandBuffer: MTLCommandBuffer) -> Bool {
        guard texture.mipmapLevelCount > 1 else { return true }
        guard let blit = commandBuffer.makeBlitCommandEncoder() else { return false }
        blit.label = "wvd.mips"
        blit.generateMipmaps(for: texture)
        blit.endEncoding()
        return true
    }

    public static func mipLevels(width: Int, height: Int) -> Int {
        Int(log2(Double(max(width, height))).rounded(.down)) + 1
    }
}
