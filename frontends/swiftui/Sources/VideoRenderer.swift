/*
 * VideoRenderer.swift — hiển thị video: rc_frame (YUV I420 sw / NV12 hwdec) → texture Metal →
 * shader YUV→RGB, letterbox giữ tỉ lệ. Frame tới trên thread nội bộ của core được copy vào
 * FrameStore (có khóa) rồi upload trên thread vẽ của MTKView. Tái hiện render.c của front-end GTK.
 */
import Metal
import MetalKit
import RCCore
import simd

/// Kho một frame chờ upload — copy planes giữ nguyên stride của decoder (một memcpy mỗi plane).
final class FrameStore {
    private let lock = NSLock()
    private(set) var width = 0
    private(set) var height = 0
    private(set) var nv12 = false
    private(set) var fullRange = false
    private(set) var bt709 = false
    private var planes: [[UInt8]] = [[], [], []]
    private var strides = [0, 0, 0]
    private var pending = false
    private var sizeChanged = false

    /// Gọi trên thread nội bộ của core.
    func store(_ f: RCFrameRef) {
        lock.lock()
        defer { lock.unlock() }
        sizeChanged = sizeChanged || (width != f.width || height != f.height)
        width = f.width
        height = f.height
        nv12 = (f.pixfmt == RC_PIX_NV12)
        fullRange = f.fullRange
        bt709 = f.bt709

        copyPlane(0, f.data.0, f.linesize.0, height)
        if nv12 {
            copyPlane(1, f.data.1, f.linesize.1, height / 2) // UV đan xen: h/2 dòng
        } else {
            copyPlane(1, f.data.1, f.linesize.1, height / 2)
            copyPlane(2, f.data.2, f.linesize.2, height / 2)
        }
        pending = true
    }

    private func copyPlane(_ i: Int, _ src: UnsafePointer<UInt8>?, _ stride: Int, _ rows: Int) {
        guard let src = src, stride > 0, rows > 0 else { return }
        let need = stride * rows
        if planes[i].count != need { planes[i] = [UInt8](repeating: 0, count: need) }
        planes[i].withUnsafeMutableBytes { dst in
            dst.baseAddress!.copyMemory(from: src, byteCount: need)
        }
        strides[i] = stride
    }

    /// Lấy snapshot để upload (main thread). Trả nil nếu không có frame mới.
    struct Snapshot {
        let width, height: Int
        let nv12, fullRange, bt709, sizeChanged: Bool
        let planes: [[UInt8]]
        let strides: [Int]
    }
    func takePending() -> Snapshot? {
        lock.lock()
        defer { lock.unlock() }
        guard pending, width > 0, height > 0 else { return nil }
        pending = false
        let sc = sizeChanged
        sizeChanged = false
        return Snapshot(width: width, height: height, nv12: nv12, fullRange: fullRange,
                        bt709: bt709, sizeChanged: sc, planes: planes, strides: strides)
    }

    var videoSize: (Int, Int) {
        lock.lock(); defer { lock.unlock() }
        return (width, height)
    }
}

/// Ma trận YUV→RGB (cột-major) + y_off từ chuẩn màu; limited-range giãn Y/chroma về full.
/// Sai công thức range là nguyên nhân ảnh "phủ sương" (đen thành xám). Khớp render.c.
func colorMatrix(bt709: Bool, fullRange: Bool) -> (simd_float3x3, Float) {
    let kr: Float = bt709 ? 0.2126 : 0.299
    let kb: Float = bt709 ? 0.0722 : 0.114
    let kg = 1 - kr - kb
    let cr = 2 * (1 - kr)
    let cb = 2 * (1 - kb)
    let ys: Float = fullRange ? 1 : 255.0 / 219.0
    let cs: Float = fullRange ? 1 : 255.0 / 224.0
    let yOff: Float = fullRange ? 0 : 16.0 / 255.0
    // Cột: col0=(ys,ys,ys), col1=(0,-cs*cb*kb/kg,cs*cb), col2=(cs*cr,-cs*cr*kr/kg,0)
    let col0 = SIMD3<Float>(ys, ys, ys)
    let col1 = SIMD3<Float>(0, -cs * cb * kb / kg, cs * cb)
    let col2 = SIMD3<Float>(cs * cr, -cs * cr * kr / kg, 0)
    return (simd_float3x3(columns: (col0, col1, col2)), yOff)
}

private struct Uniforms {
    var cmat: simd_float3x3
    var yOff: Float
    var nv12: Int32
}

private let shaderSource = """
#include <metal_stdlib>
using namespace metal;

struct Uniforms { float3x3 cmat; float y_off; int nv12; };
struct VOut { float4 pos [[position]]; float2 uv; };

vertex VOut v_main(uint vid [[vertex_id]]) {
    float2 pos[4] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(1,1) };
    float2 uv[4]  = { float2(0,1),  float2(1,1),  float2(0,0),  float2(1,0) };
    VOut o; o.pos = float4(pos[vid], 0, 1); o.uv = uv[vid]; return o;
}

fragment float4 f_main(VOut in [[stage_in]],
                       texture2d<float> ytex [[texture(0)]],
                       texture2d<float> utex [[texture(1)]],
                       texture2d<float> vtex [[texture(2)]],
                       constant Uniforms& u [[buffer(0)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    float3 yuv;
    yuv.x = ytex.sample(s, in.uv).r - u.y_off;
    if (u.nv12 == 1) {
        yuv.yz = utex.sample(s, in.uv).rg - float2(0.5);
    } else {
        yuv.y = utex.sample(s, in.uv).r - 0.5;
        yuv.z = vtex.sample(s, in.uv).r - 0.5;
    }
    return float4(u.cmat * yuv, 1.0);
}
"""

final class VideoRenderer: NSObject, MTKViewDelegate {
    let device: MTLDevice
    private let queue: MTLCommandQueue
    private var pipeline: MTLRenderPipelineState?
    private var textures: [MTLTexture?] = [nil, nil, nil]
    private var texW = [0, 0, 0]
    private var texH = [0, 0, 0]
    private var texNV12 = false
    let store = FrameStore()
    var onSizeChanged: ((Int, Int) -> Void)?

    init?(mtkView: MTKView) {
        guard let dev = mtkView.device ?? MTLCreateSystemDefaultDevice(),
              let q = dev.makeCommandQueue() else { return nil }
        device = dev
        queue = q
        super.init()
        mtkView.device = dev
        mtkView.colorPixelFormat = .bgra8Unorm
        mtkView.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        mtkView.framebufferOnly = true
        do {
            let lib = try dev.makeLibrary(source: shaderSource, options: nil)
            let desc = MTLRenderPipelineDescriptor()
            desc.vertexFunction = lib.makeFunction(name: "v_main")
            desc.fragmentFunction = lib.makeFunction(name: "f_main")
            desc.colorAttachments[0].pixelFormat = mtkView.colorPixelFormat
            pipeline = try dev.makeRenderPipelineState(descriptor: desc)
        } catch {
            NSLog("[ui] tạo pipeline Metal lỗi: \(error)")
            return nil
        }
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    private func ensureTexture(_ i: Int, _ w: Int, _ h: Int, _ fmt: MTLPixelFormat) {
        if let t = textures[i], texW[i] == w, texH[i] == h, t.pixelFormat == fmt { return }
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: fmt, width: max(w, 1),
                                                         height: max(h, 1), mipmapped: false)
        d.usage = .shaderRead
        textures[i] = device.makeTexture(descriptor: d)
        texW[i] = w; texH[i] = h
    }

    private func upload(_ snap: FrameStore.Snapshot) {
        let w = snap.width, h = snap.height
        // Y plane
        ensureTexture(0, w, h, .r8Unorm)
        replace(0, snap.planes[0], snap.strides[0], w, h)
        if snap.nv12 {
            ensureTexture(1, w / 2, h / 2, .rg8Unorm)
            // Chroma UV đan xen: texture RG8 (w/2 × h/2), bytesPerRow = stride byte của plane UV.
            replace(1, snap.planes[1], snap.strides[1], w / 2, h / 2, bytesPerRow: snap.strides[1])
        } else {
            ensureTexture(1, w / 2, h / 2, .r8Unorm)
            ensureTexture(2, w / 2, h / 2, .r8Unorm)
            replace(1, snap.planes[1], snap.strides[1], w / 2, h / 2)
            replace(2, snap.planes[2], snap.strides[2], w / 2, h / 2)
        }
        texNV12 = snap.nv12
    }

    private func replace(_ i: Int, _ data: [UInt8], _ stride: Int, _ w: Int, _ h: Int,
                         bytesPerRow: Int? = nil) {
        guard let tex = textures[i], !data.isEmpty, w > 0, h > 0 else { return }
        let region = MTLRegionMake2D(0, 0, w, h)
        data.withUnsafeBytes { ptr in
            tex.replace(region: region, mipmapLevel: 0, withBytes: ptr.baseAddress!,
                        bytesPerRow: bytesPerRow ?? stride)
        }
    }

    func draw(in view: MTKView) {
        if let snap = store.takePending() {
            upload(snap)
            if snap.sizeChanged { onSizeChanged?(snap.width, snap.height) }
        }
        guard let pipeline = pipeline, texW[0] > 0,
              let drawable = view.currentDrawable,
              let rpd = view.currentRenderPassDescriptor,
              let cmd = queue.makeCommandBuffer(),
              let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else {
            return
        }

        // Letterbox: giữ tỉ lệ video trong drawable (pixel).
        let (vw, vh) = store.videoSize
        let fbw = Double(view.drawableSize.width), fbh = Double(view.drawableSize.height)
        if vw > 0, vh > 0, fbw > 0, fbh > 0 {
            let winA = fbw / fbh, vidA = Double(vw) / Double(vh)
            var vpw = fbw, vph = fbh
            if winA > vidA { vph = fbh; vpw = fbh * vidA } else { vpw = fbw; vph = fbw / vidA }
            enc.setViewport(MTLViewport(originX: (fbw - vpw) / 2, originY: (fbh - vph) / 2,
                                        width: vpw, height: vph, znear: 0, zfar: 1))
        }

        enc.setRenderPipelineState(pipeline)
        enc.setFragmentTexture(textures[0], index: 0)
        enc.setFragmentTexture(textures[1], index: 1)
        enc.setFragmentTexture(textures[texNV12 ? 1 : 2], index: 2)
        let (cmat, yOff) = colorMatrix(bt709: store.bt709, fullRange: store.fullRange)
        var u = Uniforms(cmat: cmat, yOff: yOff, nv12: texNV12 ? 1 : 0)
        enc.setFragmentBytes(&u, length: MemoryLayout<Uniforms>.stride, index: 0)
        enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        enc.endEncoding()
        cmd.present(drawable)
        cmd.commit()
    }
}
