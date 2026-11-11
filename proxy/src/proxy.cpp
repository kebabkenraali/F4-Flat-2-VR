// Spike: d3d11.dll proxy that forwards to the real system d3d11.dll.
// - Logs device/swapchain creation and Present calls (fo4stereo_proxy.log).
// - Hooks immediate and deferred contexts, collects vertex-shader constant buffers
//   used by draw calls during a short frame window, and dumps their contents at
//   Present on the immediate context (fo4stereo_cb.bin). Diagnostic only.

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <share.h>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdint>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>
#include "stereo_shm.h"
#include "openvr.h"

typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef void(STDMETHODCALLTYPE* VSSetCBFn)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
typedef void(STDMETHODCALLTYPE* DrawIndexedFn)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE* DrawFn)(ID3D11DeviceContext*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* CreateDeferredFn)(ID3D11Device*, UINT, ID3D11DeviceContext**);
typedef HRESULT(STDMETHODCALLTYPE* MapFn)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                          D3D11_MAPPED_SUBRESOURCE*);
typedef void(STDMETHODCALLTYPE* UnmapFn)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
typedef void(STDMETHODCALLTYPE* UpdateSubFn)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                             const void*, UINT, UINT);
typedef void(STDMETHODCALLTYPE* CopySubFn)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT,
                                           ID3D11Resource*, UINT, const D3D11_BOX*);
typedef void(STDMETHODCALLTYPE* CopyResFn)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);

// ID3D11DeviceContext vtable indices, verified against the Windows SDK d3d11.h
// (ID3D11DeviceContextVtbl): 0-2 IUnknown, 3-6 ID3D11DeviceChild, then:
enum CtxIdx : int {
    kVSSetConstantBuffers = 7,
    kDrawIndexed = 12,
    kDraw = 13,
    kMap = 14,
    kUnmap = 15,
    kCopySubresourceRegion = 46,
    kCopyResource = 47,
    kUpdateSubresource = 48,
};

// The runtime can hand out different interface pointers (ID3D11DeviceContext,
// ...1, ...2, ...3, ...4) with different vtables. Each patched vtable keeps its
// own originals; hooks look up the originals by the caller's vtable.
struct CtxVt {
    void** vtbl;
    VSSetCBFn vs;
    DrawIndexedFn drawIndexed;
    DrawFn draw;
    MapFn map;
    UnmapFn unmap;
    CopySubFn copySub;
    CopyResFn copyRes;
    UpdateSubFn updateSub;
};
static CtxVt g_vts[16];
static volatile LONG g_nvt = 0;

static CtxVt* FindVt(ID3D11DeviceContext* self) {
    void** v = *reinterpret_cast<void***>(self);
    LONG n = g_nvt;
    for (LONG i = 0; i < n; ++i) {
        if (g_vts[i].vtbl == v) return &g_vts[i];
    }
    return &g_vts[0];
}

// Frame window (Present count) in which draws are inspected and buffers collected.
static const LONG CAP_FRAME_START = 300;
static const LONG CAP_FRAME_END = 302;
static const size_t MAX_CANDIDATES = 64;
static const UINT MAX_CB_BYTES = 65536;

static HMODULE g_realD3D11 = nullptr;
static FILE* g_log = nullptr;
static FILE* g_dump = nullptr;
static volatile LONG g_frames = 0;
static PresentFn g_origPresent = nullptr;
static VSSetCBFn g_origVSSetCB = nullptr;
static DrawIndexedFn g_origDrawIndexed = nullptr;
static DrawFn g_origDraw = nullptr;
static CreateDeferredFn g_origCreateDeferred = nullptr;
static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;

static std::mutex g_mtx;                       // guards g_vsCB and g_candidates
static ID3D11Buffer* g_vsCB[14] = {};
static std::vector<ID3D11Buffer*> g_candidates;  // AddRef'd
static std::unordered_set<ID3D11Buffer*> g_candSet;
static std::unordered_set<uint64_t> g_seen;
static std::map<UINT, ID3D11Buffer*> g_staging;

static UINT g_blobCount = 0;
static UINT g_drawImm = 0;
static UINT g_drawDeferred = 0;
static UINT g_deferredCtxCount = 0;
static UINT g_vsCbCalls = 0;
static bool g_dumped = false;

static UINT g_scanCount = 0;
static UINT g_fullDump752 = 0;

static void Log(const char* fmt, ...);

// Diagnostics: find where world-position-like data (|v|>1000) lives in mapped
// buffers, and fully dump the first camera-like 752-byte buffers.
static void DiagBuffer(const char* tag, void* ptr, UINT bytes) {
    float* f = static_cast<float*>(ptr);
    UINT rows = bytes / 16;

    if (bytes == 752 && g_fullDump752 < 2) {
        float x = f[0], y = f[1], z = f[2];
        float len = sqrtf(x * x + y * y + z * z);
        if (len > 0.9f && len < 1.1f && fabsf(z) < 0.05f) {
            g_fullDump752++;
            Log("full dump %s 752B #%u:", tag, g_fullDump752);
            for (UINT r = 0; r < rows; ++r) {
                Log("  r%02u: %12.4f %12.4f %12.4f %12.4f",
                    r, f[r * 4], f[r * 4 + 1], f[r * 4 + 2], f[r * 4 + 3]);
            }
        }
    }

    g_scanCount++;
    if (g_scanCount > 30 && (g_scanCount % 20000) != 0) return;
    std::string hits;
    for (UINT r = 0; r < rows; ++r) {
        if (fabsf(f[r * 4]) > 1000.0f || fabsf(f[r * 4 + 1]) > 1000.0f
                || fabsf(f[r * 4 + 2]) > 1000.0f) {
            char b[80];
            sprintf_s(b, sizeof(b), " r%u=(%.0f,%.0f,%.0f)",
                      r, f[r * 4], f[r * 4 + 1], f[r * 4 + 2]);
            hits += b;
        }
    }
    if (!hits.empty()) {
        Log("posscan %s %uB #%u:%s", tag, bytes, g_scanCount, hits.c_str());
    }
}

// --- stereo camera-shift experiment -------------------------------------------
// Buffers identified by size + near-plane fingerprint (see STATUS.md):
//   752 bytes (47 x float4): camera state, rows 35/36 = camera world position,
//     row 0 = camera rotation R (right axis = row 0 xyz)
//   336 bytes (21 x float4): row 14 = camera world position (same values)
static MapFn g_origMap = nullptr;
static UnmapFn g_origUnmap = nullptr;
static bool g_stereoEnabled = true;
static float g_ampUnits = 2.3f;   // ~IPD/2: 1 FO4 unit ~= 1.43 cm
static float g_periodSec = 6.0f;  // full left->right wobble period
static UINT g_patchCount = 0;
static UINT g_mapCalls = 0;
static UINT g_unmapCalls = 0;
static float g_rightAxis[3] = { 1.0f, 0.0f, 0.0f };

struct MappedRec { ID3D11Resource* res; void* ptr; };
static std::mutex g_mapMtx;
static std::map<ID3D11DeviceContext*, std::vector<MappedRec>> g_mapped;

static UINT g_updCalls = 0;
static UINT g_copySubCalls = 0;
static UINT g_copyResCalls = 0;
// fp = near-plane fingerprint match state for camera-size buffers
static UINT g_fpMiss752 = 0;
static float g_lastD[3] = { 0.0f, 0.0f, 0.0f };
static float g_projK = 1.0f;
static float g_projNear = -15.0f;
static UINT g_scanClip = 0;
// Unpatched camera matrices of the current frame (copied from the 752-byte buffer
// before patching) so copies in other buffers can be found and patched the same way.
static float g_vpOrig[16] = {};
static float g_invOrig[16] = {};
static float g_camOrig[3] = {};
static volatile bool g_haveCam = false;
static std::mutex g_scanMtx;
static std::set<uint64_t> g_scanLogged;
static UINT g_scanVP = 0, g_scanInv = 0, g_scanCam = 0;
static UINT g_patch336 = 0;
static volatile float g_frameShift = 0.0f;
static volatile double g_frameTime = 0.0;
static UINT g_fpMiss336 = 0;
// histogram of unmapped buffer sizes (diagnostics)
static std::mutex g_histoMtx;
static std::map<UINT, UINT> g_unmapSizes;

static LARGE_INTEGER g_qpf = {};
static LARGE_INTEGER g_qpcStart = {};

static void LoadStereoConfig() {
    char dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\');
    if (slash) *(slash + 1) = '\0';
    std::string ini = dir + std::string("fo4stereo.ini");
    char buf[64] = {};
    GetPrivateProfileStringA("stereo", "enabled", "1", buf, sizeof(buf), ini.c_str());
    g_stereoEnabled = atoi(buf) != 0;
    GetPrivateProfileStringA("stereo", "amp_units", "2.3", buf, sizeof(buf), ini.c_str());
    g_ampUnits = static_cast<float>(atof(buf));
    GetPrivateProfileStringA("stereo", "period_sec", "6.0", buf, sizeof(buf), ini.c_str());
    g_periodSec = static_cast<float>(atof(buf));
}

// Detects the camera-state buffer by CONTENT: the camera world position is
// duplicated across the buffer's per-pass slots (posscan: rows 9/21/33/45 in
// the 752-byte buffer), so we patch every row holding the same large world
// coordinate triple. This does not depend on the near-plane value or on fixed
// row indices (the RenderDoc-captured layout differed from live gameplay).
static void PatchCameraBuffer(void* ptr, UINT bytes) {
    // 752-byte camera-state buffer (47 x float4). One row = 4 floats.
    // Layout (RenderDoc analysis, STATUS.md):
    //   rows 0-2  camera rotation R (row 0 = camera right axis)
    //   row 6     (0, 0, 1, -near) with near ~= 15.0006  -> fingerprint
    //   rows 8-11 view-projection VP = P*R (no translation: geometry is camera-relative)
    //   rows 35/36 camera world position
    float* f = static_cast<float*>(ptr);
    auto row = [f](int r) { return f + r * 4; };

    // 336-byte buffer: row 14 = camera world position with w = 4.0 (STATUS.md).
    // Shift it by the same eye offset as the 752-byte camera buffer this frame.
    if (bytes == 336) {
        float* c = row(14);
        if ((fabsf(c[0]) > 1000.0f || fabsf(c[1]) > 1000.0f) && fabsf(c[3] - 4.0f) < 0.01f
                && g_stereoEnabled) {
            c[0] += g_lastD[0]; c[1] += g_lastD[1]; c[2] += g_lastD[2];
            g_patch336++;
        }
        return;
    }
    if (bytes != 752) return;

    const float* r6 = row(6);
    if (!(fabsf(r6[2] - 1.0f) < 0.01f && r6[3] > -15.1f && r6[3] < -14.9f)) {
        g_fpMiss752++;
        // Diagnose: dump a few misses that still carry a camera-sized position
        // (rows 35/36 large) so other camera passes (depth prepass etc.) can be found.
        static UINT s_missDumps = 0;
        const float* c = row(35);
        if (s_missDumps < 4 && (fabsf(c[0]) > 1000.0f || fabsf(c[1]) > 1000.0f)) {
            s_missDumps++;
            Log("fp-miss 752B with camera-like row35 (dump %u):", s_missDumps);
            for (int r = 0; r < 47; ++r) {
                const float* q = row(r);
                Log("  r%02d: %12.4f %12.4f %12.4f %12.4f", r, q[0], q[1], q[2], q[3]);
            }
        }
        return;
    }

    float rx = row(0)[0], ry = row(0)[1], rz = row(0)[2];
    const float l = sqrtf(rx * rx + ry * ry + rz * rz);
    if (l < 0.99f || l > 1.01f) return;
    // Only the world camera: loading-screen / UI cameras sit at the origin.
    if (fabsf(row(35)[0]) < 100.0f && fabsf(row(35)[1]) < 100.0f) return;
    g_projK = row(6)[2];
    g_projNear = row(6)[3];

    if (!g_stereoEnabled || g_ampUnits == 0.0f) return;

    // Same shift for every pass of a frame (updated once per Present), otherwise
    // depth prepass and color pass disagree and produce streaks/ghosting.
    const double t = g_frameTime;
    const float shift = g_frameShift;
    const float dx = shift * rx, dy = shift * ry, dz = shift * rz;
    g_lastD[0] = dx; g_lastD[1] = dy; g_lastD[2] = dz;
    memcpy(g_vpOrig, row(8), sizeof(g_vpOrig));
    memcpy(g_invOrig, row(16), sizeof(g_invOrig));
    g_camOrig[0] = row(35)[0]; g_camOrig[1] = row(35)[1]; g_camOrig[2] = row(35)[2];
    g_haveCam = true;

    // Eye translation: VP' = VP * T(-d)  =>  each VP row's w -= dot(row.xyz, d).
    // Rows 37-40 hold a second copy of VP (verified equal to rows 8-11); patch it
    // the same way only when it really is a copy.
    const bool vpCopy = memcmp(row(37), row(8), 16 * sizeof(float)) == 0;
    for (int r = 8; r <= 11; ++r) {
        float* m = row(r);
        m[3] -= m[0] * dx + m[1] * dy + m[2] * dz;
    }
    if (vpCopy) {
        for (int r = 37; r <= 40; ++r) {
            float* m = row(r);
            m[3] -= m[0] * dx + m[1] * dy + m[2] * dz;
        }
    }
    // Rows 12-15 = inverse view rotation R^T (row 15 = 0,0,0,1). Shaders that rebuild
    // positions as R^T * viewPos get positions relative to the shifted eye; adding d as
    // translation brings them back to the original camera-relative frame.
    {
        const float* r15 = row(15);
        if (fabsf(r15[0]) < 1e-4f && fabsf(r15[1]) < 1e-4f && fabsf(r15[2]) < 1e-4f
                && fabsf(r15[3] - 1.0f) < 1e-4f) {
            // Disabled 11.11.2026: made shadows worse in the in-game test.
            // row(12)[3] += dx; row(13)[3] += dy; row(14)[3] += dz;
        }
    }
    // Rows 16-19 = inverse VP (verified: rows16-19 x rows8-11 = identity). Screen-space
    // effects reconstruct world positions with it. InvVP' = T(d) * InvVP:
    // rows 16/17/18 += d.x/d.y/d.z * row 19.
    {
        const float* w = row(19);
        const float d[3] = { dx, dy, dz };
        for (int i = 0; i < 3; ++i) {
            float* m = row(16 + i);
            m[0] += d[i] * w[0];
            m[1] += d[i] * w[1];
            m[2] += d[i] * w[2];
            m[3] += d[i] * w[3];
        }
    }
    // Keep the camera world position consistent (lighting, fog, specular).
    for (int r = 35; r <= 36; ++r) {
        float* p = row(r);
        if (fabsf(p[0]) > 100.0f || fabsf(p[1]) > 100.0f) {
            p[0] += dx; p[1] += dy; p[2] += dz;
        }
    }

    g_patchCount++;
    if (g_patchCount <= 10 || g_patchCount % 600 == 0) {
        Log("stereo patch #%u t=%.2fs shift=%.2f right=(%.3f %.3f %.3f) cam=(%.1f %.1f %.1f) vp8w=%.3f",
            g_patchCount, t, shift, rx, ry, rz, row(35)[0], row(35)[1], row(35)[2], row(8)[3]);
    }
}

static bool BlockEq(const float* a, const float* b, int n) {
    for (int i = 0; i < n; ++i) {
        const float tol = 1e-3f * (1.0f + fabsf(b[i]));
        if (fabsf(a[i] - b[i]) > tol) return false;
    }
    return true;
}

// Structural fingerprint of the main camera's view-projection (4 rows, row-major):
// rows 0,1,3 have w = 0, row 2 has w = -near (~ -15.0006), row 3 xyz = unit forward,
// row 2 xyz parallel to row 3 xyz. Independent of upload order within a frame.
static bool IsMainVP(const float* q) {
    if (fabsf(q[3]) > 1e-3f || fabsf(q[7]) > 1e-3f || fabsf(q[15]) > 1e-3f) return false;
    if (!(q[11] < -14.9f && q[11] > -15.1f)) return false;
    const float l3 = sqrtf(q[12] * q[12] + q[13] * q[13] + q[14] * q[14]);
    if (l3 < 0.99f || l3 > 1.01f) return false;
    const float l2 = sqrtf(q[8] * q[8] + q[9] * q[9] + q[10] * q[10]);
    if (l2 < 0.99f || l2 > 1.01f) return false;
    const float dot = (q[8] * q[12] + q[9] * q[13] + q[10] * q[14]) / (l2 * l3);
    return dot > 0.999f;
}

// Any matrix of the form VP * W (W affine) keeps the projection's signature:
// row2.xyz = k * row3.xyz and row2.w = k * row3.w + near, with k / near taken
// from the main camera (near ~ -15.0006). Light cameras use other near values.
static bool IsMainClipMatrix(const float* q) {
    const float l3 = fabsf(q[12]) + fabsf(q[13]) + fabsf(q[14]);
    if (l3 < 1e-6f) return false;
    const float k = g_projK, n = g_projNear;
    for (int i = 0; i < 3; ++i) {
        const float e = k * q[12 + i];
        if (fabsf(q[8 + i] - e) > 1e-4f * (1.0f + fabsf(e))) return false;
    }
    const float ew = k * q[15] + n;
    return fabsf(q[11] - ew) < 1e-3f * (1.0f + fabsf(ew));
}

static void LogScanOnce(UINT bytes, int r, int kind, const char* what) {
    const uint64_t key = (static_cast<uint64_t>(bytes) << 16) | (static_cast<uint64_t>(r) << 4) | kind;
    std::lock_guard<std::mutex> lock(g_scanMtx);
    if (g_scanLogged.insert(key).second && g_scanLogged.size() < 200) {
        Log("scan match: %uB row %d = %s", bytes, r, what);
    }
}

// Finds copies of this frame's camera matrices / position in other constant
// buffers and applies the same eye shift. Logs each (size,row,kind) once.
static void ScanOtherBuffer(void* ptr, UINT bytes) {
    if (!g_haveCam || !g_stereoEnabled || bytes == 752 || bytes < 64 || bytes > 16384) return;
    float* f = static_cast<float*>(ptr);
    const int rows = static_cast<int>(bytes / 16);
    const float dx = g_lastD[0], dy = g_lastD[1], dz = g_lastD[2];
    for (int r = 0; r < rows; ++r) {
        float* q = f + r * 4;
        if (r + 4 <= rows && IsMainClipMatrix(q)) {
            // WVP' = VP * T(-d) * W = WVP + c in the translation column, c = -VP.xyz . d
            for (int k = 0; k < 4; ++k) {
                const float* v = g_vpOrig + k * 4;
                q[k * 4 + 3] -= v[0] * dx + v[1] * dy + v[2] * dz;
            }
            g_scanClip++;
            LogScanOnce(bytes, r, 1, "clip matrix VP*W (patched)");
            r += 3;
            continue;
        }
        if (r + 4 <= rows && BlockEq(q, g_invOrig, 16)) {
            const float* w = q + 12;
            const float d[3] = { dx, dy, dz };
            for (int i = 0; i < 3; ++i) {
                float* m = q + i * 4;
                m[0] += d[i] * w[0]; m[1] += d[i] * w[1]; m[2] += d[i] * w[2]; m[3] += d[i] * w[3];
            }
            g_scanInv++;
            LogScanOnce(bytes, r, 2, "inverse VP copy (patched)");
            r += 3;
            continue;
        }
        if ((fabsf(g_camOrig[0]) > 1000.0f || fabsf(g_camOrig[1]) > 1000.0f)
                && fabsf(q[0] - g_camOrig[0]) < 50.0f && fabsf(q[1] - g_camOrig[1]) < 50.0f
                && fabsf(q[2] - g_camOrig[2]) < 50.0f) {
            q[0] += dx; q[1] += dy; q[2] += dz;
            g_scanCam++;
            LogScanOnce(bytes, r, 3, "camera position copy (patched)");
        }
    }
}

static HRESULT STDMETHODCALLTYPE HookedMap(ID3D11DeviceContext* self, ID3D11Resource* res, UINT subres,
                                           D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* out) {
    g_mapCalls++;
    HRESULT hr = FindVt(self)->map(self, res, subres, type, flags, out);
    if (SUCCEEDED(hr) && out && out->pData) {
        std::lock_guard<std::mutex> lock(g_mapMtx);
        g_mapped[self].push_back({ res, out->pData });
    }
    return hr;
}

static void STDMETHODCALLTYPE HookedUnmap(ID3D11DeviceContext* self, ID3D11Resource* res, UINT subres) {
    void* ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mapMtx);
        auto it = g_mapped.find(self);
        if (it != g_mapped.end()) {
            auto& v = it->second;
            for (size_t i = 0; i < v.size(); ++i) {
                if (v[i].res == res) {
                    ptr = v[i].ptr;
                    v[i] = v.back();
                    v.pop_back();
                    break;
                }
            }
        }
    }
    g_unmapCalls++;
    if (res) {
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        res->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            ID3D11Buffer* buf = static_cast<ID3D11Buffer*>(res);
            D3D11_BUFFER_DESC d = {};
            buf->GetDesc(&d);
            {
                std::lock_guard<std::mutex> lock(g_histoMtx);
                g_unmapSizes[d.ByteWidth]++;
            }
            if (ptr) {
                // patch before unmap completes so the draw sees patched data
                PatchCameraBuffer(ptr, d.ByteWidth);
                ScanOtherBuffer(ptr, d.ByteWidth);
                if (d.ByteWidth >= 256 && d.ByteWidth <= 1024) {
                    DiagBuffer("umap", ptr, d.ByteWidth);
                }
            }
        }
    }
    FindVt(self)->unmap(self, res, subres);
}

// CopySubresourceRegion/CopyResource are the remaining ways the runtime updates
// a buffer. We cannot modify GPU-side source data, but knowing that camera-size
// buffers are copy targets redirects the patching strategy.
static void STDMETHODCALLTYPE HookedCopySub(ID3D11DeviceContext* self, ID3D11Resource* dst, UINT dstSub,
                                            UINT dx, UINT dy, UINT dz, ID3D11Resource* src, UINT srcSub,
                                            const D3D11_BOX* box) {
    g_copySubCalls++;
    if (dst && (g_copySubCalls % 64) == 0) {  // sample every 64th call
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        dst->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC d = {};
            static_cast<ID3D11Buffer*>(dst)->GetDesc(&d);
            if (d.ByteWidth == 752 || d.ByteWidth == 336) {
                Log("CopySub -> camera-size buffer %u dst=%p src=%p box=%p (call #%u)",
                    d.ByteWidth, (void*)dst, (void*)src, (const void*)box, g_copySubCalls);
            }
        }
    }
    FindVt(self)->copySub(self, dst, dstSub, dx, dy, dz, src, srcSub, box);
}

static void STDMETHODCALLTYPE HookedCopyRes(ID3D11DeviceContext* self, ID3D11Resource* dst, ID3D11Resource* src) {
    g_copyResCalls++;
    if (dst && (g_copyResCalls % 64) == 0) {
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        dst->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC d = {};
            static_cast<ID3D11Buffer*>(dst)->GetDesc(&d);
            if (d.ByteWidth == 752 || d.ByteWidth == 336) {
                Log("CopyResource -> camera-size buffer %u dst=%p src=%p (call #%u)",
                    d.ByteWidth, (void*)dst, (void*)src, g_copyResCalls);
            }
        }
    }
    FindVt(self)->copyRes(self, dst, src);
}

// Some engines upload constant buffers with UpdateSubresource instead of Map.
// Patch a private copy of the source data so the game's memory is untouched.
static void STDMETHODCALLTYPE HookedUpdateSub(ID3D11DeviceContext* self, ID3D11Resource* dst, UINT dstSub,
                                              const D3D11_BOX* box, const void* src, UINT rowPitch,
                                              UINT depthPitch) {
    g_updCalls++;
    if (dst && src && !box) {
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        dst->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC d = {};
            static_cast<ID3D11Buffer*>(dst)->GetDesc(&d);
            if (d.ByteWidth == 752 || d.ByteWidth == 336) {
                alignas(16) unsigned char tmp[752];
                memcpy(tmp, src, d.ByteWidth);
                PatchCameraBuffer(tmp, d.ByteWidth);
                FindVt(self)->updateSub(self, dst, dstSub, box, tmp, rowPitch, depthPitch);
                return;
            }
        }
    }
    FindVt(self)->updateSub(self, dst, dstSub, box, src, rowPitch, depthPitch);
}

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

static std::string GameDir() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0';
    return path;
}

static void OpenLogs() {
    if (g_log) return;
    std::string dir = GameDir();
    // _SH_DENYNO lets other tools read the files while the game runs.
    g_log = _fsopen((dir + "fo4stereo_proxy.log").c_str(), "w", _SH_DENYNO);
    g_dump = _fsopen((dir + "fo4stereo_cb.bin").c_str(), "wb", _SH_DENYNO);
    Log("fo4stereo proxy loaded");
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_qpcStart);
    LoadStereoConfig();
    Log("stereo experiment: enabled=%d amp_units=%.3f period_sec=%.2f",
        g_stereoEnabled ? 1 : 0, g_ampUnits, g_periodSec);
}

static uint64_t Fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static ID3D11Buffer* GetStaging(UINT size) {
    auto it = g_staging.find(size);
    if (it != g_staging.end()) return it->second;
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = size;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* b = nullptr;
    if (FAILED(g_dev->CreateBuffer(&d, nullptr, &b))) b = nullptr;
    g_staging[size] = b;
    return b;
}

// Runs on the immediate-context thread (Present) only.
static void DumpCandidates() {
    if (!g_dump || !g_ctx || !g_dev) return;
    std::vector<ID3D11Buffer*> list;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        list = g_candidates;
    }
    Log("Dumping %zu candidate CBs (immediate draws=%u deferred draws=%u vsCbCalls=%u deferredCtx=%u)",
        list.size(), g_drawImm, g_drawDeferred, g_vsCbCalls, g_deferredCtxCount);
    for (ID3D11Buffer* b : list) {
        D3D11_BUFFER_DESC d;
        b->GetDesc(&d);
        if (d.ByteWidth == 0 || d.ByteWidth > MAX_CB_BYTES) continue;
        ID3D11Buffer* st = GetStaging(d.ByteWidth);
        if (!st) continue;
        g_ctx->CopyResource(st, b);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (FAILED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) continue;
        const uint8_t* p = static_cast<const uint8_t*>(m.pData);
        uint64_t h = Fnv1a(p, d.ByteWidth);
        if (g_seen.insert(h).second) {
            uint32_t hdr[4] = { 0, 0, d.ByteWidth, 0 };
            fwrite(hdr, sizeof(hdr), 1, g_dump);
            fwrite(p, 1, d.ByteWidth, g_dump);
            g_blobCount++;
        }
        g_ctx->Unmap(st, 0);
    }
    fflush(g_dump);
    Log("Dump done: blobs=%u", g_blobCount);
}

// --- OpenVR output -----------------------------------------------------------
// The F4SE plugin alternates the engine camera between the eyes (+-IPD/2) and
// writes the eye into shared memory. Here every presented frame is copied into
// that eye's texture and both eye textures are submitted to the SteamVR compositor.
// openvr_api.dll is loaded dynamically from SteamVR; no import library is needed.
// Config [vr]: enabled, eye_delay (0/1), swap_eyes (0/1), wait_poses (0/1).
typedef uint32_t(__cdecl* VRInitFn)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
typedef void*(__cdecl* VRGetIfaceFn)(const char*, vr::EVRInitError*);
typedef bool(__cdecl* VRIsHmdFn)();

static bool g_vrEnabled = false;
static int g_vrEyeDelay = 0;
static bool g_vrSwap = false;
static bool g_vrWaitPoses = true;
static bool g_vrTried = false;
static vr::IVRCompositor* g_vrComp = nullptr;
static ID3D11Texture2D* g_eyeTex[2] = { nullptr, nullptr };
static UINT g_eyeW = 0, g_eyeH = 0;
static DXGI_FORMAT g_eyeFmt = DXGI_FORMAT_UNKNOWN;
static StereoShm* g_shm = nullptr;
static LONG g_vrPrevEye = 0;
static UINT g_vrSubmits = 0, g_vrSubmitErr = 0, g_vrSubmitOk = 0;
static int g_vrLastErr = 0;

static void LoadVrConfig() {
    char dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\');
    if (slash) *(slash + 1) = '\0';
    std::string ini = dir + std::string("fo4stereo.ini");
    char buf[64] = {};
    GetPrivateProfileStringA("vr", "enabled", "0", buf, sizeof(buf), ini.c_str());
    g_vrEnabled = atoi(buf) != 0;
    GetPrivateProfileStringA("vr", "eye_delay", "0", buf, sizeof(buf), ini.c_str());
    g_vrEyeDelay = atoi(buf);
    GetPrivateProfileStringA("vr", "swap_eyes", "0", buf, sizeof(buf), ini.c_str());
    g_vrSwap = atoi(buf) != 0;
    GetPrivateProfileStringA("vr", "wait_poses", "1", buf, sizeof(buf), ini.c_str());
    g_vrWaitPoses = atoi(buf) != 0;
    Log("vr config: enabled=%d eye_delay=%d swap_eyes=%d wait_poses=%d",
        g_vrEnabled ? 1 : 0, g_vrEyeDelay, g_vrSwap ? 1 : 0, g_vrWaitPoses ? 1 : 0);
}

static void VrInitOnce() {
    if (g_vrTried) return;
    g_vrTried = true;
    LoadVrConfig();
    g_shm = OpenStereoShm();
    Log("stereo shm %s", g_shm ? "open" : "FAILED");
    if (!g_vrEnabled) return;

    HMODULE vrdll = LoadLibraryA("openvr_api.dll");
    if (!vrdll) vrdll = LoadLibraryA("C:\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\openvr_api.dll");
    if (!vrdll) { Log("vr: openvr_api.dll not found"); return; }
    auto isHmd = reinterpret_cast<VRIsHmdFn>(GetProcAddress(vrdll, "VR_IsHmdPresent"));
    auto init = reinterpret_cast<VRInitFn>(GetProcAddress(vrdll, "VR_InitInternal2"));
    auto getIface = reinterpret_cast<VRGetIfaceFn>(GetProcAddress(vrdll, "VR_GetGenericInterface"));
    if (!init || !getIface) { Log("vr: exports missing"); return; }
    Log("vr: hmd present=%d", isHmd ? (isHmd() ? 1 : 0) : -1);
    vr::EVRInitError err = vr::VRInitError_None;
    init(&err, vr::VRApplication_Scene, nullptr);
    if (err != vr::VRInitError_None) { Log("vr: init failed err=%d", (int)err); return; }
    g_vrComp = static_cast<vr::IVRCompositor*>(getIface(vr::IVRCompositor_Version, &err));
    if (!g_vrComp || err != vr::VRInitError_None) {
        Log("vr: compositor %s failed err=%d", vr::IVRCompositor_Version, (int)err);
        g_vrComp = nullptr;
        return;
    }
    Log("vr: compositor ready (%s)", vr::IVRCompositor_Version);
}

static void VrEnsureTextures(ID3D11Texture2D* bb) {
    D3D11_TEXTURE2D_DESC d = {};
    bb->GetDesc(&d);
    if (g_eyeTex[0] && d.Width == g_eyeW && d.Height == g_eyeH && d.Format == g_eyeFmt) return;
    for (auto& t : g_eyeTex) { if (t) { t->Release(); t = nullptr; } }
    D3D11_TEXTURE2D_DESC e = d;
    e.MipLevels = 1;
    e.ArraySize = 1;
    e.SampleDesc.Count = 1;
    e.SampleDesc.Quality = 0;
    e.Usage = D3D11_USAGE_DEFAULT;
    e.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    e.CPUAccessFlags = 0;
    // Shareable so the SteamVR compositor can open it (error 106 SharedTexturesNotSupported otherwise).
    e.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    for (auto& t : g_eyeTex) {
        if (FAILED(g_dev->CreateTexture2D(&e, nullptr, &t))) { Log("vr: eye texture create failed"); return; }
    }
    g_eyeW = d.Width; g_eyeH = d.Height; g_eyeFmt = d.Format;
    Log("vr: eye textures %ux%u fmt=%d samples=%u", d.Width, d.Height, (int)d.Format, d.SampleDesc.Count);
}

// Called from Present before the original Present.
static void VrOnPresent(IDXGISwapChain* sc) {
    VrInitOnce();
    if (g_shm) InterlockedIncrement(&g_shm->presents);
    if (!g_vrComp || !g_dev || !g_ctx) return;

    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb) return;
    VrEnsureTextures(bb);
    if (!g_eyeTex[0]) { bb->Release(); return; }

    LONG eye = g_shm ? g_shm->eye : 0;
    if (g_vrEyeDelay == 1) { LONG cur = eye; eye = g_vrPrevEye; g_vrPrevEye = cur; }
    if (g_vrSwap) eye ^= 1;
    eye &= 1;

    D3D11_TEXTURE2D_DESC d = {};
    bb->GetDesc(&d);
    if (d.SampleDesc.Count > 1) g_ctx->ResolveSubresource(g_eyeTex[eye], 0, bb, 0, d.Format);
    else g_ctx->CopyResource(g_eyeTex[eye], bb);
    bb->Release();

    if (g_vrWaitPoses) {
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
        g_vrComp->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    }
    for (int i = 0; i < 2; ++i) {
        vr::Texture_t tex = { g_eyeTex[i], vr::TextureType_DirectX, vr::ColorSpace_Auto };
        vr::EVRCompositorError e = g_vrComp->Submit(i == 0 ? vr::Eye_Left : vr::Eye_Right, &tex);
        if (e != vr::VRCompositorError_None) {
            g_vrLastErr = (int)e;
            if (g_vrSubmitErr++ < 10) Log("vr: submit eye %d error %d", i, (int)e);
        } else {
            g_vrSubmitOk++;
        }
    }
    g_vrComp->PostPresentHandoff();
    if (g_vrSubmits++ < 3 || g_vrSubmits % 600 == 0) {
        Log("vr: frame %u eye=%ld seq=%ld submitOk=%u submitErr=%u lastErr=%d", g_vrSubmits, eye,
            g_shm ? g_shm->seq : -1, g_vrSubmitOk, g_vrSubmitErr, g_vrLastErr);
    }
}

static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* self, UINT sync, UINT flags) {
    LONG n = InterlockedIncrement(&g_frames);
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const double t = static_cast<double>(now.QuadPart - g_qpcStart.QuadPart)
                         / static_cast<double>(g_qpf.QuadPart);
        g_frameTime = t;
        g_frameShift = static_cast<float>(g_ampUnits * sin(2.0 * 3.14159265358979323846 * t / g_periodSec));
    }
    if (n == 1 || n % 600 == 0) {
        // top unmapped buffer sizes (camera buffers are in the 300..800 byte range)
        std::map<UINT, UINT> top;
        {
            std::lock_guard<std::mutex> lock(g_histoMtx);
            top = g_unmapSizes;
        }
        std::string sizes;
        for (const auto& kv : top) {
            if (kv.first < 256 || kv.first > 1024) continue;
            char b[48];
            sprintf_s(b, sizeof(b), " %u:%u", kv.first, kv.second);
            sizes += b;
        }
        Log("Present frame=%ld sync=%u flags=0x%x draws(imm=%u def=%u) vsCb=%u map=%u unmap=%u upd=%u"
            " copySub=%u copyRes=%u patches=%u p336=%u scan(clip=%u vp=%u inv=%u cam=%u) fpMiss(752=%u 336=%u) vtbls=%ld sizes:%s",
            n, sync, flags, g_drawImm, g_drawDeferred, g_vsCbCalls, g_mapCalls, g_unmapCalls,
            g_updCalls, g_copySubCalls, g_copyResCalls, g_patchCount, g_patch336, g_scanClip, g_scanVP, g_scanInv, g_scanCam, g_fpMiss752, g_fpMiss336,
            g_nvt, sizes.c_str());
    }
    if (n == CAP_FRAME_END && !g_dumped) {
        g_dumped = true;
        DumpCandidates();
    }
    VrOnPresent(self);
    return g_origPresent(self, sync, flags);
}

static void STDMETHODCALLTYPE HookedVSSetCB(ID3D11DeviceContext* self, UINT start, UINT num,
                                             ID3D11Buffer* const* bufs) {
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_vsCbCalls++;
        for (UINT i = 0; i < num; ++i) {
            UINT slot = start + i;
            if (slot >= 14) break;
            ID3D11Buffer* nb = bufs ? bufs[i] : nullptr;
            if (nb) nb->AddRef();
            if (g_vsCB[slot]) g_vsCB[slot]->Release();
            g_vsCB[slot] = nb;
        }
    }
    FindVt(self)->vs(self, start, num, bufs);
}

// Called from any thread on each draw. Collects bound VS CBs during the frame window.
static void CollectOnDraw(ID3D11DeviceContext* self) {
    if (self == g_ctx) g_drawImm++; else g_drawDeferred++;
    LONG f = g_frames;
    if (g_dumped || f < CAP_FRAME_START || f >= CAP_FRAME_END) return;
    std::lock_guard<std::mutex> lock(g_mtx);
    for (UINT s = 0; s < 14; ++s) {
        ID3D11Buffer* b = g_vsCB[s];
        if (!b || g_candSet.count(b)) continue;
        if (g_candidates.size() >= MAX_CANDIDATES) return;
        b->AddRef();
        g_candidates.push_back(b);
        g_candSet.insert(b);
    }
}

static void STDMETHODCALLTYPE HookedDrawIndexed(ID3D11DeviceContext* self, UINT idx, UINT start, INT base) {
    CollectOnDraw(self);
    FindVt(self)->drawIndexed(self, idx, start, base);
}

static void STDMETHODCALLTYPE HookedDraw(ID3D11DeviceContext* self, UINT vtx, UINT start) {
    CollectOnDraw(self);
    FindVt(self)->draw(self, vtx, start);
}

static void HookContext(ID3D11DeviceContext* ctx);

static HRESULT STDMETHODCALLTYPE HookedCreateDeferred(ID3D11Device* self, UINT flags,
                                                      ID3D11DeviceContext** out) {
    HRESULT hr = g_origCreateDeferred(self, flags, out);
    if (SUCCEEDED(hr) && out && *out) {
        g_deferredCtxCount++;
        Log("CreateDeferredContext -> %p", (void*)*out);
        // Hook the deferred context's vtable too (no-op if it is shared with the immediate one).
        HookContext(*out);
    }
    return hr;
}

static void HookVtableEntry(void** vtbl, int idx, void* replacement, void** original) {
    DWORD oldProt = 0;
    if (!VirtualProtect(&vtbl[idx], sizeof(void*), PAGE_READWRITE, &oldProt)) {
        Log("VirtualProtect failed idx=%d err=%lu", idx, GetLastError());
        return;
    }
    if (!*original) *original = vtbl[idx];
    vtbl[idx] = replacement;
    VirtualProtect(&vtbl[idx], sizeof(void*), oldProt, &oldProt);
}

static void HookSwapChain(IDXGISwapChain* sc) {
    void** vtbl = *reinterpret_cast<void***>(sc);
    // IDXGISwapChain: IUnknown(3) + IDXGIObject(4) + IDXGIDeviceSubObject(1) => Present = 8
    HookVtableEntry(vtbl, 8, reinterpret_cast<void*>(&HookedPresent),
                    reinterpret_cast<void**>(&g_origPresent));
    Log("Hooked Present on swapchain %p", (void*)sc);
}

static void SetVtEntry(void** vtbl, int idx, void* replacement) {
    DWORD oldProt = 0;
    if (!VirtualProtect(&vtbl[idx], sizeof(void*), PAGE_READWRITE, &oldProt)) {
        Log("VirtualProtect failed idx=%d err=%lu", idx, GetLastError());
        return;
    }
    vtbl[idx] = replacement;
    VirtualProtect(&vtbl[idx], sizeof(void*), oldProt, &oldProt);
}

static std::mutex g_hookMtx;

// Patch one vtable once, remembering its own originals.
static void HookOneVtable(void* iface, const char* name) {
    if (!iface) return;
    std::lock_guard<std::mutex> lock(g_hookMtx);
    void** vtbl = *reinterpret_cast<void***>(iface);
    LONG n = g_nvt;
    for (LONG i = 0; i < n; ++i) {
        if (g_vts[i].vtbl == vtbl) return;  // already patched
    }
    if (n >= static_cast<LONG>(sizeof(g_vts) / sizeof(g_vts[0]))) return;
    // A vtable entry that already points at our hook means another interface shares
    // this code path; skip so we never record our own hook as an "original".
    if (vtbl[kMap] == reinterpret_cast<void*>(&HookedMap)) return;

    CtxVt& vt = g_vts[n];
    vt.vtbl = vtbl;
    vt.vs = reinterpret_cast<VSSetCBFn>(vtbl[kVSSetConstantBuffers]);
    vt.drawIndexed = reinterpret_cast<DrawIndexedFn>(vtbl[kDrawIndexed]);
    vt.draw = reinterpret_cast<DrawFn>(vtbl[kDraw]);
    vt.map = reinterpret_cast<MapFn>(vtbl[kMap]);
    vt.unmap = reinterpret_cast<UnmapFn>(vtbl[kUnmap]);
    vt.copySub = reinterpret_cast<CopySubFn>(vtbl[kCopySubresourceRegion]);
    vt.copyRes = reinterpret_cast<CopyResFn>(vtbl[kCopyResource]);
    vt.updateSub = reinterpret_cast<UpdateSubFn>(vtbl[kUpdateSubresource]);
    InterlockedExchange(&g_nvt, n + 1);  // publish before patching

    SetVtEntry(vtbl, kVSSetConstantBuffers, reinterpret_cast<void*>(&HookedVSSetCB));
    SetVtEntry(vtbl, kDrawIndexed, reinterpret_cast<void*>(&HookedDrawIndexed));
    SetVtEntry(vtbl, kDraw, reinterpret_cast<void*>(&HookedDraw));
    SetVtEntry(vtbl, kMap, reinterpret_cast<void*>(&HookedMap));
    SetVtEntry(vtbl, kUnmap, reinterpret_cast<void*>(&HookedUnmap));
    SetVtEntry(vtbl, kCopySubresourceRegion, reinterpret_cast<void*>(&HookedCopySub));
    SetVtEntry(vtbl, kCopyResource, reinterpret_cast<void*>(&HookedCopyRes));
    SetVtEntry(vtbl, kUpdateSubresource, reinterpret_cast<void*>(&HookedUpdateSub));
    Log("Hooked %s vtable %p (iface %p)", name, (void*)vtbl, iface);
}

static void HookContext(ID3D11DeviceContext* ctx) {
    HookOneVtable(ctx, "ID3D11DeviceContext");
    // The game may QueryInterface for newer context interfaces; their vtables can differ.
    struct { REFIID iid; const char* name; } ifaces[] = {
        { __uuidof(ID3D11DeviceContext1), "ID3D11DeviceContext1" },
        { __uuidof(ID3D11DeviceContext2), "ID3D11DeviceContext2" },
        { __uuidof(ID3D11DeviceContext3), "ID3D11DeviceContext3" },
        { __uuidof(ID3D11DeviceContext4), "ID3D11DeviceContext4" },
    };
    for (const auto& i : ifaces) {
        void* p = nullptr;
        if (SUCCEEDED(ctx->QueryInterface(i.iid, &p)) && p) {
            HookOneVtable(p, i.name);
            static_cast<IUnknown*>(p)->Release();
        }
    }
}

static void HookDevice(ID3D11Device* dev) {
    void** vtbl = *reinterpret_cast<void***>(dev);
    // ID3D11Device: IUnknown(3) => CreateDeferredContext = 27
    HookVtableEntry(vtbl, 27, reinterpret_cast<void*>(&HookedCreateDeferred),
                    reinterpret_cast<void**>(&g_origCreateDeferred));
    Log("Hooked device %p (CreateDeferredContext)", (void*)dev);
}

static bool LoadReal() {
    if (g_realD3D11) return true;
    char sysdir[MAX_PATH];
    GetSystemDirectoryA(sysdir, MAX_PATH);
    strcat_s(sysdir, "\\d3d11.dll");
    g_realD3D11 = LoadLibraryA(sysdir);
    return g_realD3D11 != nullptr;
}


// OpenVR needs the game's device to come from a DXGI 1.1 factory ("Failed to create
// sync texture. Ensure application was built using DXGI 1.1 or later"). Fallout 4
// enumerates its adapter with a DXGI 1.0 factory, so swap in the same physical
// adapter (matched by LUID) from CreateDXGIFactory1. Returns an AddRef'd adapter or
// nullptr to keep the game's own.
static IDXGIAdapter* GetDxgi11Adapter(IDXGIAdapter* in, D3D_DRIVER_TYPE& driverType) {
    if (!g_vrEnabled) return nullptr;
    LUID want = {};
    bool haveWant = false;
    if (in) {
        DXGI_ADAPTER_DESC d = {};
        if (SUCCEEDED(in->GetDesc(&d))) { want = d.AdapterLuid; haveWant = true; }
    } else if (driverType != D3D_DRIVER_TYPE_HARDWARE) {
        return nullptr;
    }
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f))) || !f) {
        Log("dxgi11: CreateDXGIFactory1 failed");
        return nullptr;
    }
    IDXGIAdapter1* found = nullptr;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d = {};
        a->GetDesc1(&d);
        const bool match = haveWant ? (d.AdapterLuid.LowPart == want.LowPart && d.AdapterLuid.HighPart == want.HighPart)
                                    : (i == 0);
        if (match && !found) {
            found = a;
            char name[128] = {};
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), nullptr, nullptr);
            Log("dxgi11: using adapter %u '%s' from DXGI 1.1 factory", i, name);
        } else {
            a->Release();
        }
    }
    f->Release();
    if (found && !in) driverType = D3D_DRIVER_TYPE_UNKNOWN;  // required when an adapter is passed
    return found;
}

extern "C" HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* levels, UINT numLevels, UINT sdkVersion,
    const DXGI_SWAP_CHAIN_DESC* scDesc, IDXGISwapChain** outSwapChain,
    ID3D11Device** outDevice, D3D_FEATURE_LEVEL* outLevel, ID3D11DeviceContext** outContext)
{
    OpenLogs();
    if (!LoadReal()) { Log("Failed to load real d3d11.dll"); return E_FAIL; }
    auto fn = reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(
        GetProcAddress(g_realD3D11, "D3D11CreateDeviceAndSwapChain"));
    if (!fn) return E_FAIL;

    LoadVrConfig();
    IDXGIAdapter* a11 = GetDxgi11Adapter(adapter, driverType);
    HRESULT hr = fn(a11 ? a11 : adapter, driverType, software, flags, levels, numLevels, sdkVersion,
                    scDesc, outSwapChain, outDevice, outLevel, outContext);
    if (a11) a11->Release();
    Log("D3D11CreateDeviceAndSwapChain hr=0x%08lx flags=0x%x dxgi11=%d", hr, flags, a11 ? 1 : 0);
    if (SUCCEEDED(hr) && outDevice && *outDevice) {
        g_dev = *outDevice;
        HookDevice(g_dev);
    }
    if (SUCCEEDED(hr) && outContext && *outContext) {
        g_ctx = *outContext;
        HookContext(g_ctx);
    }
    if (SUCCEEDED(hr) && outSwapChain && *outSwapChain) {
        HookSwapChain(*outSwapChain);
    }
    return hr;
}

extern "C" HRESULT WINAPI D3D11CreateDevice(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* levels, UINT numLevels, UINT sdkVersion,
    ID3D11Device** outDevice, D3D_FEATURE_LEVEL* outLevel, ID3D11DeviceContext** outContext)
{
    OpenLogs();
    if (!LoadReal()) return E_FAIL;
    auto fn = reinterpret_cast<decltype(&D3D11CreateDevice)>(
        GetProcAddress(g_realD3D11, "D3D11CreateDevice"));
    if (!fn) return E_FAIL;
    LoadVrConfig();
    IDXGIAdapter* a11 = GetDxgi11Adapter(adapter, driverType);
    HRESULT hr = fn(a11 ? a11 : adapter, driverType, software, flags, levels, numLevels, sdkVersion,
                    outDevice, outLevel, outContext);
    if (a11) a11->Release();
    Log("D3D11CreateDevice hr=0x%08lx flags=0x%x dxgi11=%d", hr, flags, a11 ? 1 : 0);
    if (SUCCEEDED(hr) && outDevice && *outDevice) {
        g_dev = *outDevice;
        HookDevice(g_dev);
    }
    if (SUCCEEDED(hr) && outContext && *outContext) {
        g_ctx = *outContext;
        HookContext(g_ctx);
    }
    return hr;
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
    }
    return TRUE;
}
