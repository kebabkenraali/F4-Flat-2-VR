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
#include "fsr1_shaders.h"
#include <shlobj.h>
#include <d3dcompiler.h>
#if FO4STEREO_HAS_RENDERDOC_API
#include <renderdoc_app.h>
#endif

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
typedef void(STDMETHODCALLTYPE* OMSetRTFn)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                           ID3D11DepthStencilView*);
typedef void(STDMETHODCALLTYPE* OMSetRTUavFn)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                              ID3D11DepthStencilView*, UINT, UINT,
                                              ID3D11UnorderedAccessView* const*, const UINT*);

// ID3D11DeviceContext vtable indices, verified against the Windows SDK d3d11.h
// (ID3D11DeviceContextVtbl): 0-2 IUnknown, 3-6 ID3D11DeviceChild, then:
enum CtxIdx : int {
    kVSSetConstantBuffers = 7,
    kDrawIndexed = 12,
    kDraw = 13,
    kMap = 14,
    kUnmap = 15,
    kOMSetRenderTargets = 33,
    kOMSetRenderTargetsAndUAVs = 34,
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
    OMSetRTFn omSetRT;
    OMSetRTUavFn omSetRTUav;
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
static volatile float g_gameTanX = 0.0f;  // game tan(half horizontal FOV)
static volatile float g_gameTanY = 0.0f;  // game tan(half vertical FOV)
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
    // rows 4/5 x/y = 1/tan(half FOV) of the game projection (measured 1.1698 / 2.1187 at 2052x1133)
    if (row(4)[0] > 0.05f && row(5)[1] > 0.05f) {
        g_gameTanX = 1.0f / row(4)[0];
        g_gameTanY = 1.0f / row(5)[1];
    }
    g_projNear = row(6)[3];

    if (!g_stereoEnabled || g_ampUnits == 0.0f) return;
    // Defense in depth: a NaN/inf shift would write NaN view-projections into
    // the world's constant buffers (world vanishes to a gray void, only the
    // character+weapon survive, since they use different buffers).
    if (!std::isfinite(g_frameShift)) return;

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
}

#pragma pack(push, 1)
struct BmpFileHeader {
    uint16_t type;
    uint32_t size;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t offBits;
};

struct BmpInfoHeader {
    uint32_t size;
    int32_t width;
    int32_t height;
    uint16_t planes;
    uint16_t bitCount;
    uint32_t compression;
    uint32_t sizeImage;
    int32_t xPelsPerMeter;
    int32_t yPelsPerMeter;
    uint32_t clrUsed;
    uint32_t clrImportant;
};
#pragma pack(pop)

static bool SaveTextureBmp(ID3D11Texture2D* source, const std::string& path) {
    if (!source || !g_dev || !g_ctx) {
        Log("vr: capture failed: missing texture or D3D device");
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    source->GetDesc(&desc);
    const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS
        || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM
        || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS
        || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM
        || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool bgrx = desc.Format == DXGI_FORMAT_B8G8R8X8_TYPELESS
        || desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM
        || desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    if ((!rgba && !bgra && !bgrx) || desc.ArraySize != 1 || desc.MipLevels != 1
            || desc.SampleDesc.Count != 1 || desc.Width == 0 || desc.Height == 0) {
        Log("vr: capture failed: unsupported texture format=%u size=%ux%u array=%u mips=%u samples=%u",
            static_cast<unsigned>(desc.Format), desc.Width, desc.Height, desc.ArraySize, desc.MipLevels,
            desc.SampleDesc.Count);
        return false;
    }

    const uint64_t imageBytes = static_cast<uint64_t>(desc.Width) * desc.Height * 4;
    if (imageBytes > 0xFFFFFFFFull - sizeof(BmpFileHeader) - sizeof(BmpInfoHeader)) {
        Log("vr: capture failed: texture is too large (%ux%u)", desc.Width, desc.Height);
        return false;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    HRESULT hr = g_dev->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr) || !staging) {
        Log("vr: capture failed: staging texture creation hr=0x%08x", static_cast<unsigned>(hr));
        return false;
    }

    g_ctx->CopyResource(staging, source);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr) || !mapped.pData) {
        Log("vr: capture failed: staging map hr=0x%08x rowPitch=%u", static_cast<unsigned>(hr), mapped.RowPitch);
        staging->Release();
        return false;
    }
    if (mapped.RowPitch < desc.Width * 4) {
        Log("vr: capture failed: invalid rowPitch=%u for width=%u", mapped.RowPitch, desc.Width);
        g_ctx->Unmap(staging, 0);
        staging->Release();
        return false;
    }

    BmpFileHeader fileHeader = {};
    fileHeader.type = 0x4D42;
    fileHeader.size = static_cast<uint32_t>(sizeof(fileHeader) + sizeof(BmpInfoHeader) + imageBytes);
    fileHeader.offBits = static_cast<uint32_t>(sizeof(fileHeader) + sizeof(BmpInfoHeader));
    BmpInfoHeader infoHeader = {};
    infoHeader.size = sizeof(infoHeader);
    infoHeader.width = static_cast<int32_t>(desc.Width);
    infoHeader.height = -static_cast<int32_t>(desc.Height);
    infoHeader.planes = 1;
    infoHeader.bitCount = 32;
    infoHeader.sizeImage = static_cast<uint32_t>(imageBytes);

    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "wb") != 0 || !file) {
        g_ctx->Unmap(staging, 0);
        staging->Release();
        Log("vr: capture failed: cannot open %s", path.c_str());
        return false;
    }

    bool ok = fwrite(&fileHeader, sizeof(fileHeader), 1, file) == 1
        && fwrite(&infoHeader, sizeof(infoHeader), 1, file) == 1;
    std::vector<uint8_t> row(static_cast<size_t>(desc.Width) * 4);
    const auto* pixels = static_cast<const uint8_t*>(mapped.pData);
    for (UINT y = 0; ok && y < desc.Height; ++y) {
        const uint8_t* src = pixels + static_cast<size_t>(y) * mapped.RowPitch;
        if (rgba) {
            for (UINT x = 0; x < desc.Width; ++x) {
                row[x * 4 + 0] = src[x * 4 + 2];
                row[x * 4 + 1] = src[x * 4 + 1];
                row[x * 4 + 2] = src[x * 4 + 0];
                row[x * 4 + 3] = src[x * 4 + 3];
            }
        } else {
            memcpy(row.data(), src, static_cast<size_t>(desc.Width) * 4);
            if (bgrx) {
                for (UINT x = 0; x < desc.Width; ++x) row[x * 4 + 3] = 0xFF;
            }
        }
        ok = fwrite(row.data(), row.size(), 1, file) == 1;
    }
    fclose(file);
    g_ctx->Unmap(staging, 0);
    staging->Release();
    if (!ok) {
        DeleteFileA(path.c_str());
        Log("vr: capture failed: write error for %s", path.c_str());
        return false;
    }
    Log("vr: captured %ux%u texture format=%u to %s", desc.Width, desc.Height,
        static_cast<unsigned>(desc.Format), path.c_str());
    return true;
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
// Config [vr]: enabled, eye_delay (0/1), swap_eyes (0/1), wait_poses (0/1),
// flat_after_frames, screen_width_m, screen_distance_m. Menus/loading go to a 2D overlay screen.
typedef uint32_t(__cdecl* VRInitFn)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
typedef void*(__cdecl* VRGetIfaceFn)(const char*, vr::EVRInitError*);
typedef bool(__cdecl* VRIsHmdFn)();

static bool g_vrEnabled = false;
static int g_vrEyeDelay = 0;
static bool g_vrSwap = false;
static bool g_vrWaitPoses = true;
static int g_vrFlatAfter = 20;          // presents without camera updates -> 2D screen mode
static float g_vrOverlayWidth = 3.0f;   // metres
static float g_vrOverlayDist = 2.0f;    // metres in front of the seated origin
static bool g_vrTried = false;
static vr::IVRCompositor* g_vrComp = nullptr;
static vr::IVROverlay* g_vrOverlay = nullptr;
static vr::VROverlayHandle_t g_overlay = vr::k_ulOverlayHandleInvalid;
static bool g_overlayShown = false;
static ID3D11Texture2D* g_eyeTex[2] = { nullptr, nullptr };
static ID3D11Texture2D* g_flatTex = nullptr;
static ID3D11Texture2D* g_blackTex = nullptr;
static DXGI_FORMAT g_eyeFmt = DXGI_FORMAT_UNKNOWN;
static StereoShm* g_shm = nullptr;
static UINT g_vrSubmits = 0, g_vrSubmitErr = 0, g_vrSubmitOk = 0;
static int g_vrLastErr = 0;
static LONG g_lastSeq = -1;
static int g_seqStill = 0;
static int g_flatPrev = -1;
static UINT g_poseWrites = 0;
static bool g_vrUseBounds = true;   // [vr] fov_bounds
static int g_vrCaptureAimFrames = 0;   // [vr] capture_aim_frames (0=off 1=pair 2=burst)
static int g_renderDocCaptureKey = 122;    // [vr] renderdoc_capture_key (VK_F11)
static bool g_monoAim = false;      // first-person aim: both frames are the same image
static bool g_vrNoVsync = true;     // [vr] no_vsync
static bool g_vrAutoConfigure = true;  // [vr] auto_configure
static float g_coverH = 0.0f;          // headset cover tan(half horizontal FOV)
static float g_vrAimRatio = 0.95f;     // [vr] aim_ratio: game FOV narrower than this x headset -> aiming (mono)
// Head view offsets ([vr] head_yaw_offset_deg / head_pitch_offset_deg, [vr] recenter_key).
// OpenVR's seated ResetZeroPose resets position and yaw but NOT pitch: a recenter done
// while looking slightly down leaves the whole game view permanently tilted. These
// offsets (radians) are baked into the pose written to shared memory, and into the
// 2D screen transform, so the game view and the overlay stay aligned.
static float g_yawOff = 0.0f, g_pitchOff = 0.0f;
static int g_recenterKey = 0;          // VK code; 0 = disabled
static float g_vrPipboyRatio = 0.5f;   // [vr] pipboy_ratio: narrower than this -> Pip-Boy (2D screen)
static bool g_vrPipboy2D = true;       // [vr] pipboy_2d
static bool g_vrOtherCams2D = true;    // [vr] other_cams_2d: VATS, dialogue, 3rd person... on the 2D screen
// [vr] submit_with_pose: submit every eye texture together with the HMD pose it was RENDERED with
// (the pose of the WaitGetPoses call before the frame was built). Without it the compositor assumes
// both textures were rendered at the newest pose; the eye that was refreshed one tick earlier then
// lags by a frame during head motion and the two eyes disagree (double image in first person).
static bool g_vrSubmitPose = true;
static vr::HmdMatrix34_t g_lastHmdPose = {};   // raw pose from the previous WaitGetPoses
static bool g_lastHmdPoseOk = false;
static vr::HmdMatrix34_t g_eyePose[2] = {};    // pose each eye texture's content was rendered with
static bool g_eyePoseOk[2] = { false, false };
static LONG g_zoomPrev = -1, g_camStatePrev = -1;
static unsigned g_vrCamMask = (1u << 0) | (1u << 7);  // [vr] vr_cam_states: states shown in VR (0 first person, 7 third person)
static int g_boundsLogged = 0;
// Projected Pip-Boy (as Fallout4VR's bAlwaysUseProjectedPipboy): the game renders the Pip-Boy
// UI into its own render target (Fallout4Prefs.ini [Display] uPipboyTargetWidth/Height), which
// is mapped onto the Pip-Boy model's screen. The proxy captures that target at creation and
// shows it on the 2D screen instead of the zoomed full frame.
static bool g_vrPipProjected = true;    // [vr] pipboy_projected
static float g_vrPipWidth = 0.8f;       // [vr] pipboy_screen_width_m
static float g_vrPipDist = 2.0f;        // [vr] pipboy_screen_distance_m
static float g_vrPipHeight = -0.35f;    // [vr] pipboy_screen_height_m (relative to eye level)
static int g_vrPipIndex = 0;            // [vr] pipboy_target_index: which matching target to show
static UINT g_pipTargetW = 1752, g_pipTargetH = 1400;
static SRWLOCK g_pipLock = SRWLOCK_INIT;
static ID3D11Texture2D* g_pipCand[8] = {};
static int g_pipCandCount = 0;
static ID3D11Texture2D* g_pipTex = nullptr;   // shareable copy for the overlay
static ID3D11Texture2D* g_pipTexSrc = nullptr;
static bool g_overlayIsPip = false;
static bool g_overlayIsAds = false;          // overlay currently placed as the head-locked ADS view
static bool g_overlayIsDlg = false;          // overlay currently placed as the dialogue panel (cropped)
static bool g_vrDlgPanel = true;             // [vr] dialogue_panel: magnified dialogue options panel
static bool g_dlgGate = false;               // [vr] dialogue_panel_gate: hide the panel when no options are on screen (flickers)
static bool g_vrDlgFull = false;            // [vr] dialogue_panel_full: 1 = whole frame as a flat head-locked screen
static float g_vrDlgDist = 1.2f, g_vrDlgWidth = 1.4f, g_vrDlgY = -0.05f;  // panel distance / width / height (m)
static float g_dlgCrop[4] = { 0.33f, 0.79f, 0.67f, 0.90f };               // crop u0,v0,u1,v1: the options band only
static bool g_vrReticle = true;               // [vr] reticle: own head-locked reticle overlay
static float g_reticleDist = 2.5f;            // [vr] reticle_dist_m
static float g_reticleDeg = 1.2f;             // [vr] reticle_deg (total angular size)
static vr::VROverlayHandle_t g_reticleOverlay = vr::k_ulOverlayHandleInvalid;
static ID3D11Texture2D* g_reticleTex = nullptr;
static bool g_reticleShown = false;
static bool g_reticleInitTried = false;
static bool g_hudPlaceholder = false;     // [vr] hud_placeholder: element names as text panels (layout work)
static bool g_hudFrame = false;           // [vr] hud_frame: all panels placed inside one frame
static float g_frameYaw = 0.0f, g_framePitch = -5.0f, g_frameWDeg = 100.0f, g_frameHDeg = 50.0f;
static float g_hudFx[13], g_hudFy[13];    // normalised position of each element inside the frame


// HUD panels (see the HUD section below): HP, ammo and compass cropped from the game's HUD.
struct HudPanel {
    const char* key;
    float crop[4];                 // u0, v0, u1, v1 of the eye frame
    float yawDeg, pitchDeg, widthM;
    vr::VROverlayHandle_t ov;
    ID3D11Texture2D* tex;          // shared texture the overlay shows
    ID3D11Texture2D* stage;        // CPU-readable copy of the crop
    UINT w, h;
    DXGI_FORMAT fmt;
    bool overlayTried, shown;
    bool enabled;                  // [vr] hud_<name>_enabled
};
static HudPanel g_hud[13] = {
    { "fo4stereo.hud.hp", { 0.00f, 0.00f, 1.00f, 1.00f }, 0.0f, -5.0f, 0.30f, vr::k_ulOverlayHandleInvalid, nullptr, nullptr, 0, 0, DXGI_FORMAT_UNKNOWN, false, false, true },
    { "fo4stereo.hud.ammo", { 0.00f, 0.00f, 1.00f, 1.00f }, 0.0f, -5.0f, 0.30f, vr::k_ulOverlayHandleInvalid, nullptr, nullptr, 0, 0, DXGI_FORMAT_UNKNOWN, false, false, true },
    { "fo4stereo.hud.compass", { 0.00f, 0.00f, 1.00f, 1.00f }, 0.0f, -5.0f, 0.30f, vr::k_ulOverlayHandleInvalid, nullptr, nullptr, 0, 0, DXGI_FORMAT_UNKNOWN, false, false, true },
    { "fo4stereo.hud.effects", { 0.00f, 0.00f, 1.00f, 1.00f }, 0.0f, -5.0f, 0.30f, vr::k_ulOverlayHandleInvalid, nullptr, nullptr, 0, 0, DXGI_FORMAT_UNKNOWN, false, false, true },
    { "fo4stereo.hud.location", { 0.00f, 0.00f, 1.00f, 1.00f }, 0.0f, -5.0f, 0.30f, vr::k_ulOverlayHandleInvalid, nullptr, nullptr, 0, 0, DXGI_FORMAT_UNKNOWN, false, false, true },
    { "fo4stereo.hud.notify", { 0.00f, 0.00f, 1.00f, 1.00f }, 0.0f, -5.0f, 0.30f, vr::k_ulOverlayHandleInvalid, nullptr, nullptr, 0, 0, DXGI_FORMAT_UNKNOWN, false, false, true },
    { "fo4stereo.hud.prompt", { 0.00f, 0.00f, 1.00f, 1.00f }, 0.0f, -5.0f, 0.30f, vr::k_ulOverlayHandleInvalid, nullptr, nullptr, 0, 0, DXGI_FORMAT_UNKNOWN, false, false, true },
    { "fo4stereo.hud.explosive", { 0.00f, 0.00f, 1.00f, 1.00f }, 0.0f, -5.0f, 0.30f, vr::k_ulOverlayHandleInvalid, nullptr, nullptr, 0, 0, DXGI_FORMAT_UNKNOWN, false, false, true },
    { "fo4stereo.hud.crit", { 0.00f, 0.00f, 1.00f, 1.00f }, 0.0f, -5.0f, 0.30f, vr::k_ulOverlayHandleInvalid, nullptr, nullptr, 0, 0, DXGI_FORMAT_UNKNOWN, false, false, true },
    { "fo4stereo.hud.ap", { 0.00f, 0.00f, 1.00f, 1.00f }, 0.0f, -5.0f, 0.30f, vr::k_ulOverlayHandleInvalid, nullptr, nullptr, 0, 0, DXGI_FORMAT_UNKNOWN, false, false, true },
    { "fo4stereo.hud.rads", { 0.00f, 0.00f, 1.00f, 1.00f }, 0.0f, -5.0f, 0.30f, vr::k_ulOverlayHandleInvalid, nullptr, nullptr, 0, 0, DXGI_FORMAT_UNKNOWN, false, false, true },
    { "fo4stereo.hud.flash", { 0.00f, 0.00f, 1.00f, 1.00f }, 0.0f, -5.0f, 0.30f, vr::k_ulOverlayHandleInvalid, nullptr, nullptr, 0, 0, DXGI_FORMAT_UNKNOWN, false, false, true },
    { "fo4stereo.hud.enemy", { 0.00f, 0.00f, 1.00f, 1.00f }, 0.0f, -5.0f, 0.30f, vr::k_ulOverlayHandleInvalid, nullptr, nullptr, 0, 0, DXGI_FORMAT_UNKNOWN, false, false, true },
};
static bool g_hudPanelsOn = true;     // [vr] hud_panels
static bool g_hudMask = true;         // [vr] hud_mask: remove the HUD from the world image
static float g_hudDist = 1.2f;        // [vr] hud_dist_m
static int g_hudBgAlpha = 0;          // [vr] hud_bg_alpha: panel background opacity (0 = text only)
static ID3D11Texture2D* g_hudSrc = nullptr;  // eye texture written this present (fresh, unmasked)
static float g_reticleYawDeg = 0.0f, g_reticlePitchDeg = 0.0f;  // [vr] reticle_yaw_deg / reticle_pitch_deg: shift to match the bullet path
static bool g_vrAdsOverlay = true;         // [vr] ads_overlay (legacy): 1 = start in overlay mode
static LONG g_adsMode = 1;                   // [vr] ads_mode: 0 mono eye textures, 1 head-locked overlay, 2 stereo (Insert cycles)
static float g_vrAdsOverlayDist = 4.0f;      // [vr] ads_overlay_distance_m
static ID3D11RenderTargetView* g_pipRtv[32] = {};
static int g_pipRtvCand[32] = {};
static volatile LONG g_pipRtvCount = 0;
static volatile LONG g_pipDrawnAt = -1000;   // present count when a Pip-Boy target was last bound
static volatile LONG g_pipDrawnIdx = -1;     // candidate index that was bound
static volatile LONG g_pipEverDrawn = 0;

// --- FSR1 spatial upscale ([vr] upscale=1) ---
// Upscales the game backbuffer to the headset's recommended per-eye resolution with
// AMD FSR1 (EASU + RCAS, non-temporal, no frame history -> safe for the alternate-eye
// stereo). Eye textures are created at the upscaled size; the per-eye texture-bounds
// cropping (VrEyeBounds) is in relative UV space, so it stays valid.
static bool g_vrUpscale = false;        // [vr] upscale
static float g_vrUpSharp = 0.2f;        // [vr] upscale_sharpness (0 = maximum, N = N stops softer)
static bool g_upFail = false;           // FSR1 init failed once: fall back to plain copies
static UINT g_hmdRecW = 0, g_hmdRecH = 0;  // headset recommended per-eye size
static UINT g_upW = 0, g_upH = 0;          // upscaled size (from bb aspect x recommended)
static DXGI_FORMAT g_upFmt = DXGI_FORMAT_UNKNOWN;  // eye/mid texture format (UAV-capable)
static UINT g_bbW = 0, g_bbH = 0, g_bbFmt = 0;     // backbuffer dims of the current textures
static ID3D11ComputeShader* g_upEasu = nullptr;
static ID3D11ComputeShader* g_upRcas = nullptr;
static ID3D11Buffer* g_upCb = nullptr;
static ID3D11Texture2D* g_upMidA = nullptr;        // EASU output / RCAS input (SRV+UAV)
static ID3D11ShaderResourceView* g_upMidSrv = nullptr;
static ID3D11UnorderedAccessView* g_upMidUav = nullptr;
static ID3D11Texture2D* g_upMidB = nullptr;        // RCAS output, copied into the eye texture
static ID3D11UnorderedAccessView* g_upMidBUav = nullptr;
static ID3D11Texture2D* g_upSrc = nullptr;         // copy of the backbuffer (SRV-bindable)
static ID3D11ShaderResourceView* g_upSrcSrv = nullptr;
static ID3D11SamplerState* g_upSamp = nullptr;      // linear clamp for the EASU gathers (s0)
static UINT g_upSrcW = 0, g_upSrcH = 0, g_upSrcF = 0;
static HWND g_gameHwnd = nullptr;                   // swapchain window, for focus/mouse handling

// Places element i inside the frame so that the whole element (its half width in degrees) stays within
// the frame edges. fx/fy are mapped onto the reduced range [left+half, right-half] x [bottom+half, top-half].
static void HudFrameAngles(int i, float& outYaw, float& outPitch) {
    const float L = g_frameYaw - g_frameWDeg * 0.5f, R = g_frameYaw + g_frameWDeg * 0.5f;
    const float T = g_framePitch + g_frameHDeg * 0.5f, B = g_framePitch - g_frameHDeg * 0.5f;
    const float halfDeg = atan2f(g_hud[i].widthM * 0.5f, g_hudDist) * 57.2957795f;
    const float loY = L + halfDeg, hiY = R - halfDeg;
    const float loP = B + halfDeg, hiP = T - halfDeg;
    outYaw = (hiY > loY) ? loY + g_hudFx[i] * (hiY - loY) : g_frameYaw;
    outPitch = (hiP > loP) ? hiP - g_hudFy[i] * (hiP - loP) : g_framePitch;
}
static std::string g_iniPath;  // the settings file in use: MCM settings if present, else fo4stereo.ini

static void LoadVrConfig() {
    char dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\');
    if (slash) *(slash + 1) = '\0';
    // MCM writes its settings to Data\MCM\Settings\F4Flat2VR.ini (same keys as fo4stereo.ini).
    // When that file exists it is the source of truth, otherwise fo4stereo.ini is used.
    const std::string mcmIni = std::string(dir) + "Data\\MCM\\Settings\\F4Flat2VR.ini";
    const bool useMcm = GetFileAttributesA(mcmIni.c_str()) != INVALID_FILE_ATTRIBUTES;
    std::string ini = useMcm ? mcmIni : std::string(dir) + "fo4stereo.ini";
    g_iniPath = ini;
    char buf[64] = {};
    GetPrivateProfileStringA("vr", "enabled", "0", buf, sizeof(buf), ini.c_str());
    g_vrEnabled = atoi(buf) != 0;
    GetPrivateProfileStringA("vr", "eye_delay", "0", buf, sizeof(buf), ini.c_str());
    g_vrEyeDelay = atoi(buf);
    GetPrivateProfileStringA("vr", "swap_eyes", "0", buf, sizeof(buf), ini.c_str());
    g_vrSwap = atoi(buf) != 0;
    GetPrivateProfileStringA("vr", "wait_poses", "1", buf, sizeof(buf), ini.c_str());
    g_vrWaitPoses = atoi(buf) != 0;
    GetPrivateProfileStringA("vr", "flat_after_frames", "20", buf, sizeof(buf), ini.c_str());
    g_vrFlatAfter = atoi(buf);
    GetPrivateProfileStringA("vr", "screen_width_m", "3.0", buf, sizeof(buf), ini.c_str());
    g_vrOverlayWidth = static_cast<float>(atof(buf));
    GetPrivateProfileStringA("vr", "screen_distance_m", "2.0", buf, sizeof(buf), ini.c_str());
    g_vrOverlayDist = static_cast<float>(atof(buf));
    GetPrivateProfileStringA("vr", "fov_bounds", "1", buf, sizeof(buf), ini.c_str());
    g_vrUseBounds = atoi(buf) != 0;
    GetPrivateProfileStringA("vr", "capture_aim_frames", "0", buf, sizeof(buf), ini.c_str());
    g_vrCaptureAimFrames = atoi(buf);
    GetPrivateProfileStringA("vr", "renderdoc_capture_key", "122", buf, sizeof(buf), ini.c_str());
    g_renderDocCaptureKey = atoi(buf);
    GetPrivateProfileStringA("vr", "no_vsync", "1", buf, sizeof(buf), ini.c_str());
    g_vrNoVsync = atoi(buf) != 0;
    GetPrivateProfileStringA("vr", "auto_configure", "1", buf, sizeof(buf), ini.c_str());
    g_vrAutoConfigure = atoi(buf) != 0;
    GetPrivateProfileStringA("vr", "aim_ratio", "0.95", buf, sizeof(buf), ini.c_str());
    g_vrAimRatio = static_cast<float>(atof(buf));
    GetPrivateProfileStringA("vr", "pipboy_ratio", "0.5", buf, sizeof(buf), ini.c_str());
    g_vrPipboyRatio = static_cast<float>(atof(buf));
    GetPrivateProfileStringA("vr", "pipboy_2d", "1", buf, sizeof(buf), ini.c_str());
    g_vrPipboy2D = atoi(buf) != 0;
    GetPrivateProfileStringA("vr", "other_cams_2d", "1", buf, sizeof(buf), ini.c_str());
    g_vrOtherCams2D = atoi(buf) != 0;
    GetPrivateProfileStringA("vr", "submit_with_pose", "1", buf, sizeof(buf), ini.c_str());
    g_vrSubmitPose = atoi(buf) != 0;
    GetPrivateProfileStringA("vr", "ads_overlay", "1", buf, sizeof(buf), ini.c_str());
    g_vrAdsOverlay = atoi(buf) != 0;
    g_adsMode = g_vrAdsOverlay ? 1 : 0;
    GetPrivateProfileStringA("vr", "ads_mode", "-1", buf, sizeof(buf), ini.c_str());
    if (atoi(buf) >= 0 && atoi(buf) <= 2) g_adsMode = atoi(buf);
    GetPrivateProfileStringA("vr", "ads_overlay_distance_m", "4.0", buf, sizeof(buf), ini.c_str());
    g_vrAdsOverlayDist = static_cast<float>(atof(buf));
    if (g_vrAdsOverlayDist < 0.5f) g_vrAdsOverlayDist = 0.5f;
    Log("vr config: submit_with_pose=%d ads_overlay=%d dist=%.1f", g_vrSubmitPose ? 1 : 0, g_vrAdsOverlay ? 1 : 0,
        g_vrAdsOverlayDist);
    GetPrivateProfileStringA("vr", "dialogue_panel", "1", buf, sizeof(buf), ini.c_str());
    g_vrDlgPanel = atoi(buf) != 0;
    GetPrivateProfileStringA("vr", "dialogue_panel_gate", "0", buf, sizeof(buf), ini.c_str());
    g_dlgGate = atoi(buf) != 0;
    GetPrivateProfileStringA("vr", "dialogue_panel_full", "0", buf, sizeof(buf), ini.c_str());
    g_vrDlgFull = atoi(buf) != 0;
    GetPrivateProfileStringA("vr", "dialogue_panel_dist_m", "1.2", buf, sizeof(buf), ini.c_str());
    g_vrDlgDist = static_cast<float>(atof(buf));
    GetPrivateProfileStringA("vr", "dialogue_panel_width_m", "1.4", buf, sizeof(buf), ini.c_str());
    g_vrDlgWidth = static_cast<float>(atof(buf));
    GetPrivateProfileStringA("vr", "dialogue_panel_y_m", "-0.05", buf, sizeof(buf), ini.c_str());
    g_vrDlgY = static_cast<float>(atof(buf));
    GetPrivateProfileStringA("vr", "dialogue_panel_crop", "0.33,0.79,0.67,0.90", buf, sizeof(buf), ini.c_str());
    {
        float c[4];
        if (sscanf_s(buf, "%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3]) == 4 && c[2] > c[0] && c[3] > c[1]) {
            for (int i = 0; i < 4; ++i) g_dlgCrop[i] = c[i] < 0.0f ? 0.0f : (c[i] > 1.0f ? 1.0f : c[i]);
        }
    }
    if (g_vrDlgDist < 0.4f) g_vrDlgDist = 0.4f;
    GetPrivateProfileStringA("vr", "reticle", "1", buf, sizeof(buf), ini.c_str());
    g_vrReticle = atoi(buf) != 0;
    GetPrivateProfileStringA("vr", "reticle_dist_m", "2.5", buf, sizeof(buf), ini.c_str());
    g_reticleDist = static_cast<float>(atof(buf));
    if (g_reticleDist < 0.5f) g_reticleDist = 0.5f;
    GetPrivateProfileStringA("vr", "reticle_deg", "3.0", buf, sizeof(buf), ini.c_str());
    g_reticleDeg = static_cast<float>(atof(buf));
    if (g_reticleDeg < 0.1f) g_reticleDeg = 0.1f;
    GetPrivateProfileStringA("vr", "reticle_yaw_deg", "0", buf, sizeof(buf), ini.c_str());
    g_reticleYawDeg = static_cast<float>(atof(buf));
    GetPrivateProfileStringA("vr", "reticle_pitch_deg", "0", buf, sizeof(buf), ini.c_str());
    g_reticlePitchDeg = static_cast<float>(atof(buf));
    GetPrivateProfileStringA("vr", "hud_panels", "1", buf, sizeof(buf), ini.c_str());
    g_hudPanelsOn = atoi(buf) != 0;
    GetPrivateProfileStringA("vr", "hud_mask", "1", buf, sizeof(buf), ini.c_str());
    g_hudMask = atoi(buf) != 0;
    GetPrivateProfileStringA("vr", "hud_dist_m", "1.2", buf, sizeof(buf), ini.c_str());
    g_hudDist = static_cast<float>(atof(buf));
    GetPrivateProfileStringA("vr", "hud_bg_alpha", "0", buf, sizeof(buf), ini.c_str());
    g_hudBgAlpha = atoi(buf);
    if (g_hudBgAlpha < 0) g_hudBgAlpha = 0;
    if (g_hudBgAlpha > 255) g_hudBgAlpha = 255;
    GetPrivateProfileStringA("vr", "hud_placeholder", "0", buf, sizeof(buf), ini.c_str());
    g_hudPlaceholder = atoi(buf) != 0;
    GetPrivateProfileStringA("vr", "hud_frame", "0", buf, sizeof(buf), ini.c_str());
    g_hudFrame = atoi(buf) != 0;
    GetPrivateProfileStringA("vr", "hud_frame_yaw_deg", "0", buf, sizeof(buf), ini.c_str());
    g_frameYaw = static_cast<float>(atof(buf));
    GetPrivateProfileStringA("vr", "hud_frame_pitch_deg", "-5", buf, sizeof(buf), ini.c_str());
    g_framePitch = static_cast<float>(atof(buf));
    GetPrivateProfileStringA("vr", "hud_frame_width_deg", "100", buf, sizeof(buf), ini.c_str());
    g_frameWDeg = static_cast<float>(atof(buf));
    GetPrivateProfileStringA("vr", "hud_frame_height_deg", "50", buf, sizeof(buf), ini.c_str());
    g_frameHDeg = static_cast<float>(atof(buf));
    // Optional explicit screen edges (degrees from view centre). When present they override centre/size.
    {
        float edge[4]; bool have[4];
        const char* ek[4] = { "hud_frame_left_deg", "hud_frame_right_deg", "hud_frame_top_deg", "hud_frame_bottom_deg" };
        for (int e = 0; e < 4; ++e) {
            GetPrivateProfileStringA("vr", ek[e], "", buf, sizeof(buf), ini.c_str());
            have[e] = buf[0] != 0;
            edge[e] = have[e] ? static_cast<float>(atof(buf)) : 0.0f;
        }
        if (have[0] && have[1] && edge[1] > edge[0]) { g_frameYaw = (edge[0] + edge[1]) * 0.5f; g_frameWDeg = edge[1] - edge[0]; }
        if (have[2] && have[3] && edge[2] > edge[3]) { g_framePitch = (edge[2] + edge[3]) * 0.5f; g_frameHDeg = edge[2] - edge[3]; }
    }
    static const char* const hudNames[13] = { "hp", "ammo", "compass", "effects", "location", "notify", "prompt", "explosive", "crit", "ap", "rads", "flash", "enemy" };
    for (int i = 0; i < 13; ++i) {
        char k[64];
        sprintf_s(k, sizeof(k), "hud_%s_crop", hudNames[i]);
        GetPrivateProfileStringA("vr", k, "", buf, sizeof(buf), ini.c_str());
        float c[4];
        if (sscanf_s(buf, "%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3]) == 4 && c[2] > c[0] && c[3] > c[1]) {
            for (int j = 0; j < 4; ++j) g_hud[i].crop[j] = c[j];
        }
        sprintf_s(k, sizeof(k), "hud_%s_yaw_deg", hudNames[i]);
        GetPrivateProfileStringA("vr", k, "", buf, sizeof(buf), ini.c_str());
        if (buf[0]) g_hud[i].yawDeg = static_cast<float>(atof(buf));
        sprintf_s(k, sizeof(k), "hud_%s_pitch_deg", hudNames[i]);
        GetPrivateProfileStringA("vr", k, "", buf, sizeof(buf), ini.c_str());
        if (buf[0]) g_hud[i].pitchDeg = static_cast<float>(atof(buf));
        sprintf_s(k, sizeof(k), "hud_%s_width_m", hudNames[i]);
        GetPrivateProfileStringA("vr", k, "", buf, sizeof(buf), ini.c_str());
        if (buf[0]) g_hud[i].widthM = static_cast<float>(atof(buf));
        sprintf_s(k, sizeof(k), "hud_%s_enabled", hudNames[i]);
        GetPrivateProfileStringA("vr", k, "", buf, sizeof(buf), ini.c_str());
        if (buf[0]) g_hud[i].enabled = atoi(buf) != 0;
        sprintf_s(k, sizeof(k), "hud_%s_fx", hudNames[i]);
        GetPrivateProfileStringA("vr", k, "", buf, sizeof(buf), ini.c_str());
        g_hudFx[i] = buf[0] ? static_cast<float>(atof(buf)) : 0.5f;
        sprintf_s(k, sizeof(k), "hud_%s_fy", hudNames[i]);
        GetPrivateProfileStringA("vr", k, "", buf, sizeof(buf), ini.c_str());
        g_hudFy[i] = buf[0] ? static_cast<float>(atof(buf)) : 0.5f;
    }
    if (g_hudFrame) {
        for (int i = 0; i < 13; ++i) {
            HudFrameAngles(i, g_hud[i].yawDeg, g_hud[i].pitchDeg);
            // pitch set together with yaw above
        }
    }
    Log("vr config: dialogue_panel=%d dist=%.2f width=%.2f y=%.2f", g_vrDlgPanel ? 1 : 0, g_vrDlgDist, g_vrDlgWidth,
        g_vrDlgY);
    GetPrivateProfileStringA("vr", "vr_cam_states", "0,7", buf, sizeof(buf), ini.c_str());
    g_vrCamMask = 0;
    for (char* p = buf; *p;) {
        const long v = strtol(p, &p, 10);
        if (v >= 0 && v < 32) g_vrCamMask |= 1u << v;
        while (*p == ',' || *p == ' ') ++p;
        if (*p && (*p < '0' || *p > '9')) break;
    }
    GetPrivateProfileStringA("vr", "pipboy_projected", "1", buf, sizeof(buf), ini.c_str());
    g_vrPipProjected = atoi(buf) != 0;
    GetPrivateProfileStringA("vr", "pipboy_screen_width_m", "0.8", buf, sizeof(buf), ini.c_str());
    g_vrPipWidth = static_cast<float>(atof(buf));
    GetPrivateProfileStringA("vr", "pipboy_screen_distance_m", "2.0", buf, sizeof(buf), ini.c_str());
    g_vrPipDist = static_cast<float>(atof(buf));
    GetPrivateProfileStringA("vr", "pipboy_screen_height_m", "-0.35", buf, sizeof(buf), ini.c_str());
    g_vrPipHeight = static_cast<float>(atof(buf));
    GetPrivateProfileStringA("vr", "pipboy_target_index", "0", buf, sizeof(buf), ini.c_str());
    g_vrPipIndex = atoi(buf);
    GetPrivateProfileStringA("vr", "upscale", "0", buf, sizeof(buf), ini.c_str());
    g_vrUpscale = atoi(buf) != 0;
    GetPrivateProfileStringA("vr", "upscale_sharpness", "0.2", buf, sizeof(buf), ini.c_str());
    g_vrUpSharp = static_cast<float>(atof(buf));
    GetPrivateProfileStringA("vr", "head_yaw_offset_deg", "0", buf, sizeof(buf), ini.c_str());
    g_yawOff = static_cast<float>(atof(buf)) * 0.0174532925f;
    GetPrivateProfileStringA("vr", "head_pitch_offset_deg", "0", buf, sizeof(buf), ini.c_str());
    g_pitchOff = static_cast<float>(atof(buf)) * 0.0174532925f;
    GetPrivateProfileStringA("vr", "recenter_key", "35", buf, sizeof(buf), ini.c_str());  // VK_DELETE = 46
    g_recenterKey = atoi(buf);
    Log("vr config: enabled=%d eye_delay=%d swap_eyes=%d wait_poses=%d flat_after=%d screen=%.2fm@%.2fm",
        g_vrEnabled ? 1 : 0, g_vrEyeDelay, g_vrSwap ? 1 : 0, g_vrWaitPoses ? 1 : 0, g_vrFlatAfter,
        g_vrOverlayWidth, g_vrOverlayDist);
    Log("vr config: upscale=%d sharpness=%.2f", g_vrUpscale ? 1 : 0, g_vrUpSharp);
    Log("vr config: capture_aim_frames=%d", g_vrCaptureAimFrames);
    Log("vr config: renderdoc_capture_key=%d", g_renderDocCaptureKey);
    Log("vr config: yaw_off=%.2f deg pitch_off=%.2f deg recenter_key=%d",
        g_yawOff * 57.2957795f, g_pitchOff * 57.2957795f, g_recenterKey);
}

// Yaw and pitch of a pose (OpenVR seated space: x right, y up, -z forward).
// forward = -(column 2 of the rotation matrix); yaw = atan2(-fx, -fz), pitch = asin(fy).
static void PoseYawPitch(const vr::HmdMatrix34_t& m, float& yaw, float& pitch) {
    const float fx = -m.m[0][2], fy = -m.m[1][2], fz = -m.m[2][2];
    yaw = atan2f(-fx, -fz);
    pitch = asinf(fy < -1.0f ? -1.0f : (fy > 1.0f ? 1.0f : fy));
}

static void VrPlaceOverlay(bool pip = false) {
    if (!g_vrOverlay || g_overlay == vr::k_ulOverlayHandleInvalid) return;
    vr::HmdMatrix34_t m = {};
    // Rotation = yaw offset about world up, then pitch offset about the plane's
    // own x axis, so the 2D screen follows the recentered head zero pose.
    const float s = sinf(g_yawOff), c = cosf(g_yawOff);
    const float s2 = sinf(g_pitchOff), c2 = cosf(g_pitchOff);
    m.m[0][0] = c;       m.m[0][1] = s * s2;  m.m[0][2] = s * c2;
    m.m[1][1] = c2;      m.m[1][2] = -s2;
    m.m[2][0] = -s;      m.m[2][1] = c * s2;  m.m[2][2] = c * c2;
    const float h = pip ? g_vrPipHeight : 0.0f;
    const float d = pip ? g_vrPipDist : g_vrOverlayDist;
    m.m[0][3] = -d * s;             // position rotated by the same yaw
    m.m[1][3] = h;
    m.m[2][3] = -d * c;             // seated space: -z is forward from the seated zero pose
    g_vrOverlay->SetOverlayTransformAbsolute(g_overlay, vr::TrackingUniverseSeated, &m);
    g_vrOverlay->SetOverlayWidthInMeters(g_overlay, pip ? g_vrPipWidth : g_vrOverlayWidth);
}

// Dialogue panel: crop of the lower-centre game frame as an HMD-relative quad slightly below the view axis.
static void VrPlaceDialoguePanel() {
    if (!g_vrOverlay || g_overlay == vr::k_ulOverlayHandleInvalid) return;
    vr::HmdMatrix34_t m = {};
    m.m[0][0] = 1.0f; m.m[1][1] = 1.0f; m.m[2][2] = 1.0f;
    m.m[1][3] = g_vrDlgY;
    m.m[2][3] = -g_vrDlgDist;
    g_vrOverlay->SetOverlayTransformTrackedDeviceRelative(g_overlay, vr::k_unTrackedDeviceIndex_Hmd, &m);
    // The crop is baked into g_dlgTex by VrDialoguePanelUpdate, so the overlay shows all of it.
    vr::VRTextureBounds_t b = { 0.0f, 0.0f, 1.0f, 1.0f };
    g_vrOverlay->SetOverlayTextureBounds(g_overlay, &b);
    g_vrOverlay->SetOverlayWidthInMeters(g_overlay, g_vrDlgWidth);
}

// True while the game shows dialogue response options. They are drawn in the HUD's bright green; the
// options region of the frame (the panel crop) is sampled every few presents through a small staging
// copy and counted for strongly green pixels. A short hold avoids flicker. Without options (the NPC is
// still talking) the magnified panel stays hidden instead of showing a piece of scenery.
static bool VrDialogueOptionsVisible(ID3D11Texture2D* frame) {
    static ID3D11Texture2D* s_stage = nullptr;
    static UINT s_w = 0, s_h = 0;
    static DXGI_FORMAT s_fmt = DXGI_FORMAT_UNKNOWN;
    static unsigned s_tick = 0;
    static int s_hold = 0;
    if (!frame || !g_dev || !g_ctx) return false;
    if ((s_tick++ % 8) != 0) return s_hold > 0 ? (--s_hold, true) : false;  // sample every 8th present

    D3D11_TEXTURE2D_DESC d = {};
    frame->GetDesc(&d);
    const bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS
                      || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS
                      || d.Format == DXGI_FORMAT_B8G8R8X8_UNORM || d.Format == DXGI_FORMAT_B8G8R8X8_TYPELESS
                      || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (!rgba || d.SampleDesc.Count != 1 || d.ArraySize != 1) return s_hold > 0;
    const UINT x0 = static_cast<UINT>(g_dlgCrop[0] * d.Width), x1 = static_cast<UINT>(g_dlgCrop[2] * d.Width);
    const UINT y0 = static_cast<UINT>(g_dlgCrop[1] * d.Height), y1 = static_cast<UINT>(g_dlgCrop[3] * d.Height);
    if (x1 <= x0 + 8 || y1 <= y0 + 8) return false;
    const UINT w = x1 - x0, h = y1 - y0;
    if (!s_stage || s_w != w || s_h != h || s_fmt != d.Format) {
        if (s_stage) { s_stage->Release(); s_stage = nullptr; }
        D3D11_TEXTURE2D_DESC sd = {};
        sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = d.Format;
        sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &s_stage)) || !s_stage) { s_stage = nullptr; return false; }
        s_w = w; s_h = h; s_fmt = d.Format;
    }
    D3D11_BOX box = { x0, y0, 0, x1, y1, 1 };
    g_ctx->CopySubresourceRegion(s_stage, 0, 0, 0, 0, frame, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(g_ctx->Map(s_stage, 0, D3D11_MAP_READ, 0, &m)) || !m.pData) return s_hold > 0;
    unsigned green = 0;
    for (UINT y = 0; y < h; y += 2) {
        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
        for (UINT x = 0; x < w; x += 2) {
            const int c0 = row[x * 4 + 0], c1 = row[x * 4 + 1], c2 = row[x * 4 + 2];
            // channel 1 is green in both RGBA and BGRA; red/blue are the other two
            if (c1 > 200 && c1 - c0 > 100 && c1 - c2 > 100) ++green;
        }
    }
    g_ctx->Unmap(s_stage, 0);
    static int s_lastLogged = -1;
    const bool seen = green >= 60;
    if (seen) s_hold = 4; else if (s_hold > 0) --s_hold;
    const int shown = (s_hold > 0) ? 1 : 0;
    if (shown != s_lastLogged) { Log("vr: dialogue options %s (green px=%u)", shown ? "visible" : "hidden", green); s_lastLogged = shown; }
    return s_hold > 0;
}

static void VrResetOverlayBounds() {
    if (!g_vrOverlay || g_overlay == vr::k_ulOverlayHandleInvalid) return;
    vr::VRTextureBounds_t b = { 0.0f, 0.0f, 1.0f, 1.0f };
    g_vrOverlay->SetOverlayTextureBounds(g_overlay, &b);
    g_overlayIsDlg = false;
}

// --- Own reticle (replaces the game's crosshair; no game assets used) ---
// A small head-locked overlay at a fixed distance (reticle_dist_m) sized to reticle_deg. The texture is
// generated here: green ticks plus a centre dot, transparent elsewhere. Turn the game's crosshair off with
// bCrosshairEnabled=0 in Fallout4Prefs.ini to avoid two reticles.
static ID3D11Texture2D* MakeReticleTexture() {
    const UINT n = 64;
    std::vector<uint8_t> px(static_cast<size_t>(n) * n * 4, 0);
    for (UINT y = 0; y < n; ++y) {
        for (UINT x = 0; x < n; ++x) {
            const int dx = static_cast<int>(x) - 32, dy = static_cast<int>(y) - 32;
            const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            const bool dot = ax <= 1 && ay <= 1;
            const bool tickV = ax <= 1 && ay >= 6 && ay <= 18;
            const bool tickH = ay <= 1 && ax >= 6 && ax <= 18;
            if (dot || tickV || tickH) {
                uint8_t* p = &px[(static_cast<size_t>(y) * n + x) * 4];
                p[0] = 40; p[1] = 255; p[2] = 40; p[3] = 255;  // B, G, R, A: bright green
            }
        }
    }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = n; d.Height = n; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = px.data(); sd.SysMemPitch = n * 4;
    ID3D11Texture2D* t = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, &sd, &t)) || !t) return nullptr;
    return t;
}

static void VrReticleInit() {
    if (g_reticleInitTried || !g_vrOverlay || !g_dev) return;
    g_reticleInitTried = true;
    g_reticleTex = MakeReticleTexture();
    if (!g_reticleTex) { Log("vr: reticle texture failed"); return; }
    const vr::VROverlayError e = g_vrOverlay->CreateOverlay("fo4stereo.reticle", "fo4stereo reticle", &g_reticleOverlay);
    if (e != vr::VROverlayError_None) { Log("vr: reticle overlay create error %d", static_cast<int>(e)); return; }
    vr::Texture_t t = { g_reticleTex, vr::TextureType_DirectX, vr::ColorSpace_Auto };
    g_vrOverlay->SetOverlayTexture(g_reticleOverlay, &t);
    const float halfRad = 0.5f * g_reticleDeg * 0.0174532925f;
    g_vrOverlay->SetOverlayWidthInMeters(g_reticleOverlay, 2.0f * g_reticleDist * tanf(halfRad));
    // Position on a sphere of radius reticle_dist_m around the HMD, shifted by the yaw/pitch offsets.
    const float yaw = g_reticleYawDeg * 0.0174532925f, pitch = g_reticlePitchDeg * 0.0174532925f;
    vr::HmdMatrix34_t m = {};
    m.m[0][0] = 1.0f; m.m[1][1] = 1.0f; m.m[2][2] = 1.0f;
    m.m[0][3] = g_reticleDist * sinf(yaw) * cosf(pitch);
    m.m[1][3] = g_reticleDist * sinf(pitch);
    m.m[2][3] = -g_reticleDist * cosf(yaw) * cosf(pitch);
    g_vrOverlay->SetOverlayTransformTrackedDeviceRelative(g_reticleOverlay, vr::k_unTrackedDeviceIndex_Hmd, &m);
    Log("vr: own reticle ready (dist %.2f m, %.2f deg, yaw %.1f pitch %.1f)", g_reticleDist, g_reticleDeg,
        g_reticleYawDeg, g_reticlePitchDeg);
}

// Dialogue panel content: copy the options crop out of the eye texture, keep the bright green option
// text as is and darken everything else to a translucent dark box. This removes the scenery behind the
// text and makes the options read like a subtitle box. Runs each frame while the panel is shown.
static ID3D11Texture2D* g_dlgTex = nullptr;      // shared texture the overlay displays
static ID3D11Texture2D* g_dlgStage = nullptr;    // CPU-readable staging copy of the crop
static UINT g_dlgTexW = 0, g_dlgTexH = 0;
static DXGI_FORMAT g_dlgFmt = DXGI_FORMAT_UNKNOWN;

static bool VrDialoguePanelUpdate(ID3D11Texture2D* src) {
    if (!src || !g_dev || !g_ctx) return false;
    D3D11_TEXTURE2D_DESC d = {};
    src->GetDesc(&d);
    if (d.SampleDesc.Count != 1) return false;
    const UINT x0 = static_cast<UINT>(g_dlgCrop[0] * d.Width), x1 = static_cast<UINT>(g_dlgCrop[2] * d.Width);
    const UINT y0 = static_cast<UINT>(g_dlgCrop[1] * d.Height), y1 = static_cast<UINT>(g_dlgCrop[3] * d.Height);
    if (x1 <= x0 + 8 || y1 <= y0 + 8) return false;
    const UINT w = x1 - x0, h = y1 - y0;
    if (!g_dlgStage || !g_dlgTex || g_dlgTexW != w || g_dlgTexH != h || g_dlgFmt != d.Format) {
        if (g_dlgStage) { g_dlgStage->Release(); g_dlgStage = nullptr; }
        if (g_dlgTex) { g_dlgTex->Release(); g_dlgTex = nullptr; }
        D3D11_TEXTURE2D_DESC sd = {};
        sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = d.Format;
        sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        D3D11_TEXTURE2D_DESC td = sd;
        td.Usage = D3D11_USAGE_DEFAULT; td.CPUAccessFlags = 0; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &g_dlgStage)) || !g_dlgStage) return false;
        if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_dlgTex)) || !g_dlgTex) return false;
        g_dlgTexW = w; g_dlgTexH = h; g_dlgFmt = d.Format;
    }
    const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS
                      || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8X8_UNORM
                      || d.Format == DXGI_FORMAT_B8G8R8X8_TYPELESS;
    (void)bgra;  // green is channel 1 in both orders; alpha is channel 3 in both
    D3D11_BOX box = { x0, y0, 0, x1, y1, 1 };
    g_ctx->CopySubresourceRegion(g_dlgStage, 0, 0, 0, 0, src, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(g_ctx->Map(g_dlgStage, 0, D3D11_MAP_READ, 0, &m)) || !m.pData) return false;
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* s = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
        uint8_t* q = out.data() + static_cast<size_t>(y) * w * 4;
        for (UINT x = 0; x < w; ++x, s += 4, q += 4) {
            const int c0 = s[0], c1 = s[1], c2 = s[2];
            const bool text = c1 > 200 && c1 - c0 > 100 && c1 - c2 > 100;
            if (text) {
                q[0] = s[0]; q[1] = s[1]; q[2] = s[2]; q[3] = 255;
            } else {
                q[0] = static_cast<uint8_t>(c0 * 0.12f); q[1] = static_cast<uint8_t>(c1 * 0.12f);
                q[2] = static_cast<uint8_t>(c2 * 0.12f); q[3] = 225;  // nearly opaque dark box: no scenery behind text
            }
        }
    }
    g_ctx->Unmap(g_dlgStage, 0);
    g_ctx->UpdateSubresource(g_dlgTex, 0, nullptr, out.data(), w * 4, 0);
    return true;
}

// --- HUD panels (phase 1 of docs/HUD_PLAN.md) ---
// The game's HUD elements (HP, ammo, compass) are cropped from the eye textures, shown as head-locked
// panels in front of the view, and masked out of the world image so they are not drawn twice.
// Needs fHUDOpacity=1 in Fallout4Prefs.ini (the crops come from the HUD the game draws).
static void HudCropCopy(HudPanel& p, ID3D11Texture2D* src, UINT& x0, UINT& y0, UINT& w, UINT& h, const D3D11_TEXTURE2D_DESC& d) {
    x0 = static_cast<UINT>(p.crop[0] * d.Width);
    y0 = static_cast<UINT>(p.crop[1] * d.Height);
    w = static_cast<UINT>(p.crop[2] * d.Width) - x0;
    h = static_cast<UINT>(p.crop[3] * d.Height) - y0;
    (void)src;
}

// Copies the crop, builds the panel image (bright HUD pixels kept, background darkened), and masks the
// same region in both eye textures.
// Placeholder panel: the element name as green text on a dark box (used to lay out the frame).
static ID3D11Texture2D* VrLabelTexture(const char* text) {
    const int W = 256, H = 96;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP hb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hb || !bits) { if (hb) DeleteObject(hb); DeleteDC(dc); return nullptr; }
    HGDIOBJ oldBmp = SelectObject(dc, hb);
    uint8_t* px = static_cast<uint8_t*>(bits);
    for (int i = 0; i < W * H * 4; i += 4) { px[i] = 0; px[i + 1] = 0; px[i + 2] = 0; px[i + 3] = 170; }
    HFONT font = CreateFontA(30, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(60, 255, 60));
    TextOutA(dc, 10, 30, text, static_cast<int>(strlen(text)));
    for (int i = 0; i < W * H * 4; i += 4) {
        if (px[i] | px[i + 1] | px[i + 2]) px[i + 3] = 255;
    }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = bits; sd.SysMemPitch = W * 4;
    ID3D11Texture2D* t = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, &sd, &t))) t = nullptr;
    SelectObject(dc, oldFont); DeleteObject(font);
    SelectObject(dc, oldBmp); DeleteObject(hb); DeleteDC(dc);
    return t;
}
static bool VrHudPanelUpdate(HudPanel& p, ID3D11Texture2D* src) {
    if (g_hudPlaceholder) {
        if (!p.tex) p.tex = VrLabelTexture(p.key + 14);
        return p.tex != nullptr;
    }
    if (!src || !g_dev || !g_ctx) return false;
    D3D11_TEXTURE2D_DESC d = {};
    src->GetDesc(&d);
    if (d.SampleDesc.Count != 1) return false;
    UINT x0, y0, w, h;
    HudCropCopy(p, src, x0, y0, w, h, d);
    if (w < 4 || h < 4) return false;
    if (!p.stage || !p.tex || p.w != w || p.h != h || p.fmt != d.Format) {
        if (p.stage) { p.stage->Release(); p.stage = nullptr; }
        if (p.tex) { p.tex->Release(); p.tex = nullptr; }
        D3D11_TEXTURE2D_DESC sd = {};
        sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = d.Format;
        sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        D3D11_TEXTURE2D_DESC td = sd;
        td.Usage = D3D11_USAGE_DEFAULT; td.CPUAccessFlags = 0; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &p.stage)) || !p.stage) { p.stage = nullptr; return false; }
        if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &p.tex)) || !p.tex) { p.tex = nullptr; return false; }
        p.w = w; p.h = h; p.fmt = d.Format;
    }
    D3D11_BOX box = { x0, y0, 0, x0 + w, y0 + h, 1 };
    g_ctx->CopySubresourceRegion(p.stage, 0, 0, 0, 0, src, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(g_ctx->Map(p.stage, 0, D3D11_MAP_READ, 0, &m)) || !m.pData) return false;
    std::vector<uint8_t> orig(static_cast<size_t>(w) * h * 4);  // untouched crop, for the world mask
    for (UINT y = 0; y < h; ++y) {
        memcpy(orig.data() + static_cast<size_t>(y) * w * 4, static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, static_cast<size_t>(w) * 4);
    }
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* s = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
        uint8_t* q = out.data() + static_cast<size_t>(y) * w * 4;
        for (UINT x = 0; x < w; ++x, s += 4, q += 4) {
            const int c0 = s[0], c1 = s[1], c2 = s[2];
            // HUD foreground: any bright pixel (green markers, red enemy/target marks, white items).
            // The translucent dark HUD background is below this level.
            const bool bright = c0 > 90 || c1 > 90 || c2 > 90;
            if (bright) {
                q[0] = s[0]; q[1] = s[1]; q[2] = s[2]; q[3] = 255;
            } else {
                q[0] = static_cast<uint8_t>(c0 * 0.3f); q[1] = static_cast<uint8_t>(c1 * 0.3f);
                q[2] = static_cast<uint8_t>(c2 * 0.3f); q[3] = static_cast<uint8_t>(g_hudBgAlpha);
            }
        }
    }
    g_ctx->Unmap(p.stage, 0);
    g_ctx->UpdateSubresource(p.tex, 0, nullptr, out.data(), w * 4, 0);
    if (g_hudMask) {
        // Replace the whole HUD region in the world image with a plain dark box (no HUD pixels kept,
        // otherwise the HUD text would still be drawn in the world).
        // Only the bright HUD pixels are replaced (near black). Everything else keeps the world's own pixels.
        std::vector<uint8_t> dark(orig);
        for (size_t i = 0; i < dark.size(); i += 4) {
            const int c0 = orig[i], c1 = orig[i + 1], c2 = orig[i + 2];
            if (c0 > 90 || c1 > 90 || c2 > 90) {
                dark[i] = 0; dark[i + 1] = 0; dark[i + 2] = 0; dark[i + 3] = 255;
            }
        }
        D3D11_BOX dst = { x0, y0, 0, x0 + w, y0 + h, 1 };
        if (g_eyeTex[0]) g_ctx->UpdateSubresource(g_eyeTex[0], 0, &dst, dark.data(), w * 4, 0);
        if (g_eyeTex[1]) g_ctx->UpdateSubresource(g_eyeTex[1], 0, &dst, dark.data(), w * 4, 0);
    }
    return true;
}

static void VrPlaceHudPanel(HudPanel& p) {
    const float yaw = p.yawDeg * 0.0174532925f, pitch = p.pitchDeg * 0.0174532925f;
    vr::HmdMatrix34_t m = {};
    m.m[0][0] = 1.0f; m.m[1][1] = 1.0f; m.m[2][2] = 1.0f;
    m.m[0][3] = g_hudDist * sinf(yaw) * cosf(pitch);
    m.m[1][3] = g_hudDist * sinf(pitch);
    m.m[2][3] = -g_hudDist * cosf(yaw) * cosf(pitch);
    g_vrOverlay->SetOverlayTransformTrackedDeviceRelative(p.ov, vr::k_unTrackedDeviceIndex_Hmd, &m);
    g_vrOverlay->SetOverlayWidthInMeters(p.ov, p.widthM);
}

static void VrHudPanelsFrame(bool want) {
    for (HudPanel& p : g_hud) {
        const bool show = want && p.enabled;
        if (show && g_vrOverlay) {
            if (!p.overlayTried) {
                p.overlayTried = true;
                if (g_vrOverlay->CreateOverlay(p.key, p.key, &p.ov) != vr::VROverlayError_None) p.ov = vr::k_ulOverlayHandleInvalid;
            }
            if (p.ov != vr::k_ulOverlayHandleInvalid && VrHudPanelUpdate(p, g_hudSrc) && p.tex) {
                vr::Texture_t t = { p.tex, vr::TextureType_DirectX, vr::ColorSpace_Auto };
                g_vrOverlay->SetOverlayTexture(p.ov, &t);
                if (!p.shown) {
                    VrPlaceHudPanel(p);
                    g_vrOverlay->ShowOverlay(p.ov);
                    p.shown = true;
                }
            }
        } else if (p.shown && p.ov != vr::k_ulOverlayHandleInvalid) {
            g_vrOverlay->HideOverlay(p.ov);
            p.shown = false;
        }
    }
}
// --- HUD panel tuning mode (no mod needed) ---
// Numpad 5 toggles tuning. Numpad 1/2/3 select HP/ammo/compass. Numpad 4/6 yaw, 8/2 pitch (1 degree per
// press), Numpad + / - width (10 %). Numpad 0 saves the values to fo4stereo.ini. Only the proxy reads
// these keys; the game gets them too, so keep tuning to a menu-free moment.
static bool KeyPressedEdge(int vk) {
    static bool s_down[256] = {};
    const bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool edge = d && !s_down[vk & 255];
    s_down[vk & 255] = d;
    return edge;
}

static bool g_tuneOn = false;
static int g_tuneSel = 0;
static int g_tuneMode = 0;   // 0 element, 1 crop, 2 frame edges

// On-screen help and values for tuning: a small text panel in the centre of the view, drawn with GDI into
// a texture (dark box, green text). Re-rendered only when the selection or a value changes.
static const int kTuneCount = 13;
static const char* const kTuneNames[kTuneCount] = { "hp", "ammo", "compass", "effects", "location", "notify", "prompt", "explosive", "crit", "ap", "rads", "flash", "enemy" };
static vr::VROverlayHandle_t g_tuneOv = vr::k_ulOverlayHandleInvalid;
static bool g_tuneOvTried = false, g_tuneOvShown = false;
static ID3D11Texture2D* g_tuneTex = nullptr;
static struct { int sel; float y, p, w, h; int mode; } g_tuneLast = { -1, 0.0f, 0.0f, 0.0f, 0.0f, -1 };

// Tuning view: only the frame outline (its edges) and the controls. Nothing else is shown while tuning.
static ID3D11Texture2D* VrTuneRender() {
    const float tw = tanf(0.5f * g_frameWDeg * 0.0174532925f), th = tanf(0.5f * g_frameHDeg * 0.0174532925f);
    int W = 512;
    int H = static_cast<int>(512.0f * (th / (tw > 0.0001f ? tw : 0.0001f)) + 0.5f);
    if (H < 64) H = 64;
    if (H > 2048) H = 2048;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP hb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hb || !bits) { if (hb) DeleteObject(hb); DeleteDC(dc); return nullptr; }
    HGDIOBJ oldBmp = SelectObject(dc, hb);
    uint8_t* px = static_cast<uint8_t*>(bits);
    for (int i = 0; i < W * H * 4; i += 4) { px[i] = 0; px[i + 1] = 0; px[i + 2] = 0; px[i + 3] = 0; }
    HPEN pen = CreatePen(PS_SOLID, 5, RGB(60, 255, 60));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, 3, 3, W - 3, H - 3);
    HFONT font = CreateFontA(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, OPAQUE);
    SetBkColor(dc, RGB(0, 0, 0));
    SetTextColor(dc, RGB(60, 255, 60));
    const float L = g_frameYaw - g_frameWDeg * 0.5f, R = g_frameYaw + g_frameWDeg * 0.5f;
    const float T = g_framePitch + g_frameHDeg * 0.5f, B = g_framePitch - g_frameHDeg * 0.5f;
    const char* modeName = g_tuneMode == 2 ? "FRAME" : (g_tuneMode == 1 ? "CROP" : "ELEMENT");
    char lines[5][96];
    sprintf_s(lines[0], sizeof(lines[0]), "TUNING [%s]  (5 off, 7 mode, 0 save)", modeName);
    sprintf_s(lines[1], sizeof(lines[1]), "left %.1f   right %.1f deg", L, R);
    sprintf_s(lines[2], sizeof(lines[2]), "top %.1f   bottom %.1f deg", T, B);
    sprintf_s(lines[3], sizeof(lines[3]), "4/6 left   9/3 right");
    sprintf_s(lines[4], sizeof(lines[4]), "8/2 top / bottom");
    for (int i = 0; i < 5; ++i) TextOutA(dc, 14, 12 + i * 30, lines[i], static_cast<int>(strlen(lines[i])));
    for (int i = 0; i < W * H * 4; i += 4) {
        if (px[i] | px[i + 1] | px[i + 2]) px[i + 3] = 255;
    }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = bits; sd.SysMemPitch = W * 4;
    ID3D11Texture2D* t = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, &sd, &t))) t = nullptr;
    SelectObject(dc, oldFont); DeleteObject(font);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
    SelectObject(dc, oldBmp); DeleteObject(hb); DeleteDC(dc);
    return t;
}
static void VrTuneOverlayFrame() {
    if (!g_vrOverlay || !g_dev) return;
    if (!g_tuneOn) {
        if (g_tuneOvShown && g_tuneOv != vr::k_ulOverlayHandleInvalid) { g_vrOverlay->HideOverlay(g_tuneOv); g_tuneOvShown = false; }
        return;
    }
    if (g_tuneOv == vr::k_ulOverlayHandleInvalid && !g_tuneOvTried) {
        g_tuneOvTried = true;
        if (g_vrOverlay->CreateOverlay("fo4stereo.tune", "fo4stereo tune", &g_tuneOv) != vr::VROverlayError_None)
            g_tuneOv = vr::k_ulOverlayHandleInvalid;
    }
    if (g_tuneOv == vr::k_ulOverlayHandleInvalid) return;
    const bool changed = g_tuneLast.sel != g_tuneSel || g_tuneLast.y != g_frameYaw || g_tuneLast.p != g_framePitch
                         || g_tuneLast.w != g_frameWDeg || g_tuneLast.h != g_frameHDeg || !g_tuneTex || g_tuneLast.mode != g_tuneMode;
    if (changed) {
        if (ID3D11Texture2D* t = VrTuneRender()) {
            if (g_tuneTex) g_tuneTex->Release();
            g_tuneTex = t;
        }
        g_tuneLast.sel = g_tuneSel; g_tuneLast.y = g_frameYaw; g_tuneLast.p = g_framePitch;
        g_tuneLast.w = g_frameWDeg; g_tuneLast.h = g_frameHDeg; g_tuneLast.mode = g_tuneMode;
        if (g_tuneTex) {
            vr::Texture_t tt = { g_tuneTex, vr::TextureType_DirectX, vr::ColorSpace_Auto };
            g_vrOverlay->SetOverlayTexture(g_tuneOv, &tt);
        }
        // place the outline at the frame centre, 2.5 m away, sized to the frame's horizontal angle
        const float D = 2.5f;
        const float yaw = g_frameYaw * 0.0174532925f, pitch = g_framePitch * 0.0174532925f;
        vr::HmdMatrix34_t m = {};
        m.m[0][0] = 1.0f; m.m[1][1] = 1.0f; m.m[2][2] = 1.0f;
        m.m[0][3] = D * sinf(yaw) * cosf(pitch);
        m.m[1][3] = D * sinf(pitch);
        m.m[2][3] = -D * cosf(yaw) * cosf(pitch);
        g_vrOverlay->SetOverlayTransformTrackedDeviceRelative(g_tuneOv, vr::k_unTrackedDeviceIndex_Hmd, &m);
        g_vrOverlay->SetOverlayWidthInMeters(g_tuneOv, 2.0f * D * tanf(0.5f * g_frameWDeg * 0.0174532925f));
    }
    if (!g_tuneOvShown) { g_vrOverlay->ShowOverlay(g_tuneOv); g_tuneOvShown = true; }
}

// Recomputes every element's angles from the frame (centre/size) and its normalised position in it.
static void VrApplyFrame() {
    for (int i = 0; i < 13; ++i) {
        HudFrameAngles(i, g_hud[i].yawDeg, g_hud[i].pitchDeg);
        // pitch set together with yaw above
        if (g_hud[i].shown && g_hud[i].ov != vr::k_ulOverlayHandleInvalid) VrPlaceHudPanel(g_hud[i]);
    }
}

static void VrTuningSave() {
    const std::string path = g_iniPath.empty() ? GameDir() + "fo4stereo.ini" : g_iniPath;
    for (int i = 0; i < kTuneCount; ++i) {
        char v[64], k[80];
        sprintf_s(k, sizeof(k), "hud_%s_enabled", kTuneNames[i]);
        WritePrivateProfileStringA("vr", k, g_hud[i].enabled ? "1" : "0", path.c_str());
        sprintf_s(k, sizeof(k), "hud_%s_crop", kTuneNames[i]);
        sprintf_s(v, sizeof(v), "%.3f,%.3f,%.3f,%.3f", g_hud[i].crop[0], g_hud[i].crop[1], g_hud[i].crop[2], g_hud[i].crop[3]);
        WritePrivateProfileStringA("vr", k, v, path.c_str());
        sprintf_s(k, sizeof(k), "hud_%s_width_m", kTuneNames[i]);
        sprintf_s(v, sizeof(v), "%.3f", g_hud[i].widthM);
        WritePrivateProfileStringA("vr", k, v, path.c_str());
        sprintf_s(k, sizeof(k), "hud_%s_fx", kTuneNames[i]);
        sprintf_s(v, sizeof(v), "%.3f", g_hudFx[i]);
        WritePrivateProfileStringA("vr", k, v, path.c_str());
        sprintf_s(k, sizeof(k), "hud_%s_fy", kTuneNames[i]);
        sprintf_s(v, sizeof(v), "%.3f", g_hudFy[i]);
        WritePrivateProfileStringA("vr", k, v, path.c_str());
    }
    char v[64];
    // Save the frame as its screen edges (these are what the proxy uses).
    const float fL = g_frameYaw - g_frameWDeg * 0.5f, fR = g_frameYaw + g_frameWDeg * 0.5f;
    const float fT = g_framePitch + g_frameHDeg * 0.5f, fB = g_framePitch - g_frameHDeg * 0.5f;
    sprintf_s(v, sizeof(v), "%.2f", fL); WritePrivateProfileStringA("vr", "hud_frame_left_deg", v, path.c_str());
    sprintf_s(v, sizeof(v), "%.2f", fR); WritePrivateProfileStringA("vr", "hud_frame_right_deg", v, path.c_str());
    sprintf_s(v, sizeof(v), "%.2f", fT); WritePrivateProfileStringA("vr", "hud_frame_top_deg", v, path.c_str());
    sprintf_s(v, sizeof(v), "%.2f", fB); WritePrivateProfileStringA("vr", "hud_frame_bottom_deg", v, path.c_str());
    Log("vr: tuning saved to %s", path.c_str());
}

static void VrTuningFrame() {
    if (KeyPressedEdge(VK_NUMPAD5)) {
        g_tuneOn = !g_tuneOn;
        Log("vr: tuning %s", g_tuneOn ? "ON" : "OFF");
    }
    if (!g_tuneOn) return;
    if (KeyPressedEdge(VK_NUMPAD7)) { g_tuneMode = (g_tuneMode + 1) % 3; Log("vr: tune mode %d", g_tuneMode); }
    bool changed = false;
    if (g_tuneMode == 2) {
        // frame edges: left/right/top/bottom in degrees
        float L = g_frameYaw - g_frameWDeg * 0.5f, R = g_frameYaw + g_frameWDeg * 0.5f;
        float T = g_framePitch + g_frameHDeg * 0.5f, B = g_framePitch - g_frameHDeg * 0.5f;
        if (KeyPressedEdge(VK_NUMPAD4)) { L -= 1.0f; changed = true; }
        if (KeyPressedEdge(VK_NUMPAD6)) { L += 1.0f; changed = true; }
        if (KeyPressedEdge(VK_NUMPAD9)) { R += 1.0f; changed = true; }
        if (KeyPressedEdge(VK_NUMPAD3)) { R -= 1.0f; changed = true; }
        if (KeyPressedEdge(VK_NUMPAD8)) { T += 1.0f; changed = true; }
        if (KeyPressedEdge(VK_NUMPAD2)) { B -= 1.0f; changed = true; }
        if (changed) {
            g_frameYaw = (L + R) * 0.5f; g_frameWDeg = R - L;
            g_framePitch = (T + B) * 0.5f; g_frameHDeg = T - B;
            VrApplyFrame();
        }
    } else {
        if (KeyPressedEdge(VK_NUMPAD1)) g_tuneSel = (g_tuneSel + kTuneCount - 1) % kTuneCount;
        if (KeyPressedEdge(VK_NUMPAD9)) g_tuneSel = (g_tuneSel + 1) % kTuneCount;
        HudPanel& p = g_hud[g_tuneSel];
        if (KeyPressedEdge(VK_NUMPAD3)) { p.enabled = !p.enabled; changed = true; }
        if (g_tuneMode == 1) {
            const float d = 0.01f;
            float dx = 0.0f, dy = 0.0f;
            if (KeyPressedEdge(VK_NUMPAD4)) dx -= d;
            if (KeyPressedEdge(VK_NUMPAD6)) dx += d;
            if (KeyPressedEdge(VK_NUMPAD8)) dy -= d;
            if (KeyPressedEdge(VK_NUMPAD2)) dy += d;
            if (dx != 0.0f || dy != 0.0f) {
                float u0 = p.crop[0] + dx, u1 = p.crop[2] + dx, v0 = p.crop[1] + dy, v1 = p.crop[3] + dy;
                if (u0 >= 0.0f && u1 <= 1.0f) { p.crop[0] = u0; p.crop[2] = u1; }
                if (v0 >= 0.0f && v1 <= 1.0f) { p.crop[1] = v0; p.crop[3] = v1; }
                changed = true;
            }
        } else {
            const float d = 0.005f;
            if (KeyPressedEdge(VK_NUMPAD4)) { g_hudFx[g_tuneSel] -= d; changed = true; }
            if (KeyPressedEdge(VK_NUMPAD6)) { g_hudFx[g_tuneSel] += d; changed = true; }
            if (KeyPressedEdge(VK_NUMPAD8)) { g_hudFy[g_tuneSel] -= d; changed = true; }
            if (KeyPressedEdge(VK_NUMPAD2)) { g_hudFy[g_tuneSel] += d; changed = true; }
            if (changed) VrApplyFrame();
        }
        if (changed && p.ov != vr::k_ulOverlayHandleInvalid) VrPlaceHudPanel(p);
    }
    // Whole-HUD zoom (any mode): frame size and every element width scale together, so the layout keeps its shape.
    {
        const bool zin = KeyPressedEdge(VK_ADD), zout = KeyPressedEdge(VK_SUBTRACT);
        if (zin || zout) {
            const float f = zin ? 1.1f : (1.0f / 1.1f);
            g_frameWDeg *= f; g_frameHDeg *= f;
            for (int i = 0; i < kTuneCount; ++i) g_hud[i].widthM *= f;
            VrApplyFrame();
            Log("vr: HUD zoom x%.3f (frame %.1f x %.1f deg)", f, g_frameWDeg, g_frameHDeg);
        }
    }
    if (KeyPressedEdge(VK_NUMPAD0)) VrTuningSave();
}

// Gamepad recenter combo: both thumb-stick clicks (L3 + R3) held on controller 0. XInput is
// loaded dynamically (no link dependency); minimal local struct layouts.
static bool PadRecenterCombo() {
    struct Pad { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
    struct State { DWORD packet; Pad pad; };
    using GetStateFn = DWORD(WINAPI*)(DWORD, State*);
    static GetStateFn fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE x = LoadLibraryA("xinput1_4.dll");
        if (!x) x = LoadLibraryA("xinput9_1_0.dll");
        if (x) fn = reinterpret_cast<GetStateFn>(GetProcAddress(x, "XInputGetState"));
    }
    if (!fn) return false;
    State st = {};
    if (fn(0, &st) != ERROR_SUCCESS) return false;
    return (st.pad.buttons & 0x0040) && (st.pad.buttons & 0x0080);  // THUMB_LEFT + THUMB_RIGHT
}

// ADS view as an HMD-relative (head-locked) quad straight ahead. The quad spans exactly the
// game's projected field of view: width = 2 * tanX * distance, so angular size matches the
// scene (aspect comes from the texture).
static void VrPlaceAdsOverlay() {
    if (!g_vrOverlay || g_overlay == vr::k_ulOverlayHandleInvalid) return;
    vr::HmdMatrix34_t m = {};
    m.m[0][0] = 1.0f; m.m[1][1] = 1.0f; m.m[2][2] = 1.0f;
    m.m[2][3] = -g_vrAdsOverlayDist;
    g_vrOverlay->SetOverlayTransformTrackedDeviceRelative(g_overlay, vr::k_unTrackedDeviceIndex_Hmd, &m);
    const float tanX = (g_gameTanX > 0.2f && g_gameTanX < 3.0f) ? g_gameTanX : 1.3f;
    g_vrOverlay->SetOverlayWidthInMeters(g_overlay, 2.0f * tanX * g_vrAdsOverlayDist);
}



// --- Automatic game settings for the connected headset ([vr] auto_configure=1) ---
// From the measured per-eye view: window = headset aspect, height limited to what fits
// on the desktop (a window cannot be taller than the work area), FOV set so the game's
// horizontal FOV covers the headset. Fallout 4 defines FOV horizontally at 16:9 and
// scales Hor+ (measured: 80 -> 81.06 deg at aspect 1.811), so
//   fov = 2 * atan(tan(hfov / 2) * (16/9) / aspect).
// Writes Fallout4Prefs.ini [Display] iSize W/H and Fallout4Custom.ini [Display]
// fDefaultWorldFOV/fDefault1stPersonFOV, only when values change. Takes effect on the
// next game start. Read-only attributes are kept; a one-time backup *.bak_fo4stereo is made.

static std::string DocumentsFallout4Dir() {
    PWSTR docs = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) {
        char buf[MAX_PATH * 2] = {};
        WideCharToMultiByte(CP_ACP, 0, docs, -1, buf, sizeof(buf), nullptr, nullptr);
        out = std::string(buf) + "\\My Games\\Fallout4\\";
    }
    if (docs) CoTaskMemFree(docs);
    return out;
}

// Pip-Boy render target size from Fallout4Prefs.ini [Display] (game default 1752x1400).
static void LoadPipboyTargetSize() {
    const std::string dir = DocumentsFallout4Dir();
    if (dir.empty()) return;
    const std::string prefs = dir + "Fallout4Prefs.ini";
    const UINT w = GetPrivateProfileIntA("Display", "uPipboyTargetWidth", 1752, prefs.c_str());
    const UINT h = GetPrivateProfileIntA("Display", "uPipboyTargetHeight", 1400, prefs.c_str());
    if (w > 0 && h > 0) { g_pipTargetW = w; g_pipTargetH = h; }
    Log("vr: pipboy target size %ux%u (projected=%d, screen %.2fm, index %d)", g_pipTargetW, g_pipTargetH,
        g_vrPipProjected ? 1 : 0, g_vrPipWidth, g_vrPipIndex);
}

static DXGI_FORMAT TypedFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}

static bool IniSetIfDifferent(const std::string& file, const char* section, const char* key, const char* value) {
    char cur[64] = {};
    GetPrivateProfileStringA(section, key, "", cur, sizeof(cur), file.c_str());
    if (strcmp(cur, value) == 0) return false;

    const std::string bak = file + ".bak_fo4stereo";
    if (GetFileAttributesA(bak.c_str()) == INVALID_FILE_ATTRIBUTES) CopyFileA(file.c_str(), bak.c_str(), TRUE);
    const DWORD attrs = GetFileAttributesA(file.c_str());
    const bool readOnly = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY);
    if (readOnly) SetFileAttributesA(file.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
    const BOOL ok = WritePrivateProfileStringA(section, key, value, file.c_str());
    if (readOnly) SetFileAttributesA(file.c_str(), attrs);
    Log("vr: auto-config %s [%s] %s: '%s' -> '%s' (%s)", file.c_str(), section, key, cur, value, ok ? "ok" : "FAILED");
    return ok != 0;
}

static void VrAutoConfigure(float h, float v, uint32_t recH) {
    if (!g_vrAutoConfigure || h <= 0.0f || v <= 0.0f) return;
    const std::string dir = DocumentsFallout4Dir();
    if (dir.empty()) { Log("vr: auto-config: Documents folder not found"); return; }

    // Tallest client area a normal window can have on the primary work area.
    RECT wa = {};
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
    RECT frame = { 0, 0, 100, 100 };
    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
    const int frameH = (frame.bottom - frame.top) - 100;
    int maxH = (wa.bottom - wa.top) - frameH;
    if (maxH < 480) maxH = 480;

    const float aspect = h / v;
    UINT H = recH > 0 ? recH : 1080;
    if (H > static_cast<UINT>(maxH)) H = static_cast<UINT>(maxH);
    const UINT W = static_cast<UINT>(H * aspect + 0.5f);
    const float fov = 2.0f * atanf(h * (16.0f / 9.0f) / aspect) * 57.2957795f;

    char sw[32], sh[32], sf[32];
    sprintf_s(sw, "%u", W);
    sprintf_s(sh, "%u", H);
    sprintf_s(sf, "%.1f", fov);
    const std::string prefs = dir + "Fallout4Prefs.ini";
    const std::string custom = dir + "Fallout4Custom.ini";
    bool changed = false;
    changed |= IniSetIfDifferent(prefs, "Display", "iSize W", sw);
    changed |= IniSetIfDifferent(prefs, "Display", "iSize H", sh);
    changed |= IniSetIfDifferent(custom, "Display", "fDefaultWorldFOV", sf);
    changed |= IniSetIfDifferent(custom, "Display", "fDefault1stPersonFOV", sf);
    Log("vr: auto-config target window %ux%u (max client height %d) fov %s -> %s", W, H, maxH, sf,
        changed ? "UPDATED, restart the game to apply" : "already set");
}

// HMD view per eye: raw projection tangents (left/right/top/bottom, left/top negative)
// and the recommended render size. Writes a recommendation for the game's window size
// and FOV to fo4stereo_hmd.txt next to the game.
static float g_eyeTan[2][4] = {};  // l, r, t, b
static bool g_haveEyeTan = false;
static float g_runtimeIpdM = 0.0f;  // OpenVR Prop_UserIpdMeters_Float
static bool g_haveRuntimeIpd = false;

static void VrQueryHmdView(VRGetIfaceFn getIface) {
    vr::EVRInitError err = vr::VRInitError_None;
    auto* sys = static_cast<vr::IVRSystem*>(getIface(vr::IVRSystem_Version, &err));
    if (!sys || err != vr::VRInitError_None) { Log("vr: IVRSystem unavailable err=%d", (int)err); return; }
    uint32_t rw = 0, rh = 0;
    sys->GetRecommendedRenderTargetSize(&rw, &rh);
    g_hmdRecW = static_cast<UINT>(rw);
    g_hmdRecH = static_cast<UINT>(rh);

    // Runtime IPD: the HMD's configured IPD in metres, adapts to the wearer.
    // The plugin converts it to game units (ipd_units = metres * units_per_meter)
    // when [engine] ipd_units <= 0 (auto mode).
    vr::ETrackedPropertyError ipdErr = vr::TrackedProp_Success;
    const float ipdM = sys->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                                                          vr::Prop_UserIpdMeters_Float, &ipdErr);
    if (ipdErr == vr::TrackedProp_Success && ipdM > 0.001f && ipdM < 0.2f) {
        g_runtimeIpdM = ipdM;
        g_haveRuntimeIpd = true;
        Log("vr: runtime IPD %.4f m (%.1f mm)", ipdM, ipdM * 1000.0f);
    } else {
        Log("vr: runtime IPD unavailable (err=%d, val=%.4f), using configured ipd_units", (int)ipdErr, ipdM);
    }
    if (g_shm) {
        g_shm->runtimeIpdM = g_haveRuntimeIpd ? g_runtimeIpdM : 0.0f;
        InterlockedExchange(&g_shm->runtimeIpdValid, g_haveRuntimeIpd ? 1 : 0);
    }
    for (int e = 0; e < 2; ++e) {
        sys->GetProjectionRaw(e == 0 ? vr::Eye_Left : vr::Eye_Right,
                              &g_eyeTan[e][0], &g_eyeTan[e][1], &g_eyeTan[e][2], &g_eyeTan[e][3]);
    }
    g_haveEyeTan = true;
    float h = 0.0f, v = 0.0f;
    for (int e = 0; e < 2; ++e) {
        h = (std::max)(h, (std::max)(fabsf(g_eyeTan[e][0]), fabsf(g_eyeTan[e][1])));
        v = (std::max)(v, (std::max)(fabsf(g_eyeTan[e][2]), fabsf(g_eyeTan[e][3])));
    }
    g_coverH = h;
    const float rad2deg = 57.2957795f;
    const float hfov = 2.0f * atanf(h) * rad2deg;
    const float vfov = 2.0f * atanf(v) * rad2deg;
    const UINT winH = rh;
    const UINT winW = static_cast<UINT>(rh * (h / v) + 0.5f);
    Log("vr: hmd per-eye recommended %ux%u", rw, rh);
    for (int e = 0; e < 2; ++e) {
        Log("vr: eye %d tan l=%.4f r=%.4f t=%.4f b=%.4f", e, g_eyeTan[e][0], g_eyeTan[e][1], g_eyeTan[e][2], g_eyeTan[e][3]);
    }
    Log("vr: symmetric cover h=%.4f v=%.4f -> hfov=%.1f vfov=%.1f aspect=%.3f window=%ux%u", h, v, hfov, vfov, h / v, winW, winH);

    char dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\');
    if (slash) *(slash + 1) = '\0';
    std::string path = dir + std::string("fo4stereo_hmd.txt");
    if (FILE* f = _fsopen(path.c_str(), "w", _SH_DENYNO)) {
        fprintf(f, "; Written by fo4stereo d3d11.dll from SteamVR (per-eye view of the connected headset)\n");
        fprintf(f, "recommended_per_eye=%ux%u\n", rw, rh);
        for (int e = 0; e < 2; ++e) {
            fprintf(f, "eye%d_tan_l_r_t_b=%.5f %.5f %.5f %.5f\n", e, g_eyeTan[e][0], g_eyeTan[e][1], g_eyeTan[e][2], g_eyeTan[e][3]);
        }
        fprintf(f, "cover_hfov_deg=%.2f\ncover_vfov_deg=%.2f\naspect=%.4f\n", hfov, vfov, h / v);
        fprintf(f, "suggest_iSize_W=%u\nsuggest_iSize_H=%u\nsuggest_fDefaultWorldFOV=%.1f\n", winW, winH, hfov);
        fclose(f);
    }
    VrAutoConfigure(h, v, rh);
}

// Texture bounds that map the game's symmetric projection onto this eye's view.
// Uses the game's real FOV (from its camera buffer), so it stays correct for any game FOV;
// regions outside the game image are clamped (stretched) until the game FOV covers the eye.
static bool VrEyeBounds(int eye, vr::VRTextureBounds_t& b) {
    const float gx = g_gameTanX, gy = g_gameTanY;
    if (!g_haveEyeTan || gx <= 0.0f || gy <= 0.0f) return false;
    auto clamp01 = [](float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); };
    const float* t = g_eyeTan[eye];
    float tl = t[0], tr = t[1], tb = t[2], tt = t[3];
    if (g_monoAim) {
        // Mono aim: the plugin renders BOTH frames from the same (signed) eye
        // offset, so both eyes must show the SAME window. The per-eye crops
        // differ by ~18% horizontally, which split the one image into two
        // shifted copies - the "missing barrel tip" on the left copy.
        // Collapse to the head-centred average of the two windows.
        const float* o = g_eyeTan[1 - eye];
        tl = 0.5f * (t[0] + o[0]);
        tr = 0.5f * (t[1] + o[1]);
        tb = 0.5f * (t[2] + o[2]);
        tt = 0.5f * (t[3] + o[3]);
    }
    b.uMin = clamp01((tl + gx) / (2.0f * gx));
    b.uMax = clamp01((tr + gx) / (2.0f * gx));
    b.vMin = clamp01((tb + gy) / (2.0f * gy));
    b.vMax = clamp01((tt + gy) / (2.0f * gy));
    return b.uMax > b.uMin && b.vMax > b.vMin;
}

static UINT g_vrInitAttempts = 0;
static ULONGLONG g_vrNextTryMs = 0;

// Retries every 10 s until SteamVR has a headset, so the HMD can be connected after the game started.
static void VrInitOnce() {
    if (!g_vrTried) {
        g_vrTried = true;
        LoadVrConfig();
        g_shm = OpenStereoShm();
        Log("stereo shm %s", g_shm ? "open" : "FAILED");
    }
    if (!g_vrEnabled || g_vrComp) return;
    const ULONGLONG now = GetTickCount64();
    if (now < g_vrNextTryMs) return;
    g_vrNextTryMs = now + 10000;
    if (g_vrInitAttempts++ > 0) Log("vr: init retry #%u", g_vrInitAttempts);

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
    if (err != vr::VRInitError_None) {
        Log("vr: init failed err=%d (108 = headset not found; retrying in 10 s)", (int)err);
        typedef void(__cdecl* VRShutdownFn)();
        if (auto shut = reinterpret_cast<VRShutdownFn>(GetProcAddress(vrdll, "VR_ShutdownInternal"))) shut();
        return;
    }
    g_vrComp = static_cast<vr::IVRCompositor*>(getIface(vr::IVRCompositor_Version, &err));
    if (!g_vrComp || err != vr::VRInitError_None) {
        Log("vr: compositor %s failed err=%d", vr::IVRCompositor_Version, (int)err);
        g_vrComp = nullptr;
        return;
    }
    g_vrComp->SetTrackingSpace(vr::TrackingUniverseSeated);
    Log("vr: compositor ready (%s), seated tracking", vr::IVRCompositor_Version);
    VrQueryHmdView(getIface);

    g_vrOverlay = static_cast<vr::IVROverlay*>(getIface(vr::IVROverlay_Version, &err));
    if (g_vrOverlay && err == vr::VRInitError_None) {
        vr::EVROverlayError oe = g_vrOverlay->CreateOverlay("fo4stereo.screen", "Fallout 4", &g_overlay);
        if (oe != vr::VROverlayError_None) {
            Log("vr: CreateOverlay failed %d", (int)oe);
            g_overlay = vr::k_ulOverlayHandleInvalid;
        } else {
            VrPlaceOverlay();
            Log("vr: 2D screen overlay ready (%s)", vr::IVROverlay_Version);
        }
    } else {
        Log("vr: overlay interface %s unavailable err=%d", vr::IVROverlay_Version, (int)err);
        g_vrOverlay = nullptr;
    }
}

static bool CreateFrameTexture(const D3D11_TEXTURE2D_DESC& e, const void* init, UINT pitch, ID3D11Texture2D** out) {
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = init;
    sd.SysMemPitch = pitch;
    return SUCCEEDED(g_dev->CreateTexture2D(&e, init ? &sd : nullptr, out));
}

static void VrUpscaleInit();  // defined below; compiles the FSR1 compute shaders

static void VrEnsureTextures(ID3D11Texture2D* bb) {
    D3D11_TEXTURE2D_DESC d = {};
    bb->GetDesc(&d);
    // [vr] upscale=1: eye textures get the headset's recommended per-eye size at the
    // backbuffer's aspect (uniform scale, capped at 4x). The game keeps rendering at the
    // window size; FSR1 upscales before the per-eye copy (see VrUpscale).
    UINT upW = 0, upH = 0;
    DXGI_FORMAT upFmt = d.Format;
    if (g_vrUpscale && g_hmdRecW && g_hmdRecH && !g_upFail) {
        VrUpscaleInit();  // compile the FSR1 shaders once, before sizing the eye textures
    }
    if (g_vrUpscale && g_hmdRecW && g_hmdRecH && !g_upFail) {
        const float s = (std::min)(4.0f, (std::max)(
            static_cast<float>(g_hmdRecW) / static_cast<float>(d.Width),
            static_cast<float>(g_hmdRecH) / static_cast<float>(d.Height)));
        upW = (std::max)(d.Width, static_cast<UINT>(d.Width * s + 0.5f));
        upH = (std::max)(d.Height, static_cast<UINT>(d.Height * s + 0.5f));
        UINT sup = 0;
        if (FAILED(g_dev->CheckFormatSupport(d.Format, &sup))
                || !(sup & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)) {
            upFmt = DXGI_FORMAT_R8G8B8A8_UNORM;  // UAV-capable everywhere; compositor handles it
            Log("vr: upscale: fmt %d lacks UAV, using R8G8B8A8", (int)d.Format);
        }
    }
    if (g_eyeTex[0] && d.Width == g_bbW && d.Height == g_bbH && d.Format == g_bbFmt
            && upW == g_upW && upH == g_upH && upFmt == g_upFmt
            && !(g_upFail && g_upW)) return;
    for (auto& t : g_eyeTex) { if (t) { t->Release(); t = nullptr; } }
    if (g_flatTex) { g_flatTex->Release(); g_flatTex = nullptr; }
    if (g_blackTex) { g_blackTex->Release(); g_blackTex = nullptr; }
    if (g_upMidA) { g_upMidA->Release(); g_upMidA = nullptr; }
    if (g_upMidSrv) { g_upMidSrv->Release(); g_upMidSrv = nullptr; }
    if (g_upMidUav) { g_upMidUav->Release(); g_upMidUav = nullptr; }
    if (g_upMidB) { g_upMidB->Release(); g_upMidB = nullptr; }
    if (g_upMidBUav) { g_upMidBUav->Release(); g_upMidBUav = nullptr; }
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
    if (upW) { e.Width = upW; e.Height = upH; e.Format = upFmt; }
    for (auto& t : g_eyeTex) {
        if (!CreateFrameTexture(e, nullptr, 0, &t)) { Log("vr: eye texture create failed"); return; }
    }
    // The 2D-screen overlay texture keeps the backbuffer's size (not upscaled).
    if (upW) {
        e.Width = d.Width; e.Height = d.Height; e.Format = d.Format;
    }
    if (!CreateFrameTexture(e, nullptr, 0, &g_flatTex)) Log("vr: flat texture create failed");
    // Black frame for both eyes while the 2D screen is shown (4 bytes per pixel formats).
    if (upW) {
        e.Width = upW; e.Height = upH; e.Format = upFmt;
    }
    std::vector<uint8_t> zeros(static_cast<size_t>(e.Width) * e.Height * 4, 0);
    if (!CreateFrameTexture(e, zeros.data(), e.Width * 4, &g_blackTex)) Log("vr: black texture create failed");
    // Intermediate textures for the FSR1 passes (not shared; UAV+SRV allowed here).
    if (upW) {
        e.MiscFlags = 0;
        e.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (!CreateFrameTexture(e, nullptr, 0, &g_upMidA)) { Log("vr: upscale: midA create failed"); }
        if (g_upMidA) {
            if (FAILED(g_dev->CreateShaderResourceView(g_upMidA, nullptr, &g_upMidSrv))
                    || FAILED(g_dev->CreateUnorderedAccessView(g_upMidA, nullptr, &g_upMidUav))) {
                Log("vr: upscale: midA view create failed");
            }
        }
        e.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        if (!CreateFrameTexture(e, nullptr, 0, &g_upMidB)) { Log("vr: upscale: midB create failed"); }
        if (g_upMidB && FAILED(g_dev->CreateUnorderedAccessView(g_upMidB, nullptr, &g_upMidBUav))) {
            Log("vr: upscale: midB view create failed");
        }
        e.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        e.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    }
    g_upW = upW; g_upH = upH; g_upFmt = upFmt;
    g_bbW = d.Width; g_bbH = d.Height; g_bbFmt = d.Format; g_eyeFmt = d.Format;
    Log("vr: eye textures %ux%u fmt=%d samples=%u (bb %ux%u fmt %d, upscale %u)", e.Width, e.Height,
        (int)e.Format, 1u, d.Width, d.Height, (int)d.Format, upW ? 1u : 0u);
}

// HMD pose (seated space) -> shared memory, for the plugin's head tracking.
// Applies the head view offsets directly to a pose matrix:
// R' = Ry(yawOff) * R * Rx(pitchOff) (pitch about the head's own right axis, yaw about
// world up), then the position is rotated by the yaw offset. Doing it on the matrix
// keeps the existing quaternion extraction untouched.
static void ApplyHeadOffsets(vr::HmdMatrix34_t& m) {
    if (g_yawOff == 0.0f && g_pitchOff == 0.0f) return;
    const float c = cosf(g_yawOff), s = sinf(g_yawOff);
    const float c2 = cosf(g_pitchOff), s2 = sinf(g_pitchOff);
    // R * Rx(pitchOff): rotate columns 1..2 (local pitch about the head's right axis)
    for (int r = 0; r < 3; ++r) {
        const float y = m.m[r][1], z = m.m[r][2];
        m.m[r][1] = y * c2 + z * s2;
        m.m[r][2] = -y * s2 + z * c2;
    }
    // Ry(yawOff) * R: mix rows 0 and 2 (world up yaw)
    for (int cix = 0; cix < 4; ++cix) {
        const float x = m.m[0][cix], z = m.m[2][cix];
        m.m[0][cix] = x * c + z * s;
        m.m[2][cix] = -x * s + z * c;
    }
}

static void VrWritePose(const vr::TrackedDevicePose_t& p) {
    if (!g_shm) return;
    vr::TrackedDevicePose_t pose = p;  // local copy: ApplyHeadOffsets edits the matrix
    if (pose.bPoseIsValid) ApplyHeadOffsets(pose.mDeviceToAbsoluteTracking);
    const auto& m = pose.mDeviceToAbsoluteTracking.m;
    float q[4];
    const float tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0.0f) {
        const float s = sqrtf(tr + 1.0f) * 2.0f;
        q[0] = 0.25f * s;
        q[1] = (m[2][1] - m[1][2]) / s;
        q[2] = (m[0][2] - m[2][0]) / s;
        q[3] = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const float s = sqrtf(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        q[0] = (m[2][1] - m[1][2]) / s;
        q[1] = 0.25f * s;
        q[2] = (m[0][1] + m[1][0]) / s;
        q[3] = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        const float s = sqrtf(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        q[0] = (m[0][2] - m[2][0]) / s;
        q[1] = (m[0][1] + m[1][0]) / s;
        q[2] = 0.25f * s;
        q[3] = (m[1][2] + m[2][1]) / s;
    } else {
        const float s = sqrtf(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        q[0] = (m[1][0] - m[0][1]) / s;
        q[1] = (m[0][2] + m[2][0]) / s;
        q[2] = (m[1][2] + m[2][1]) / s;
        q[3] = 0.25f * s;
    }
    InterlockedIncrement(&g_shm->poseSeq);  // odd: writing
    g_shm->hmdPos[0] = m[0][3];
    g_shm->hmdPos[1] = m[1][3];
    g_shm->hmdPos[2] = m[2][3];
    for (int i = 0; i < 4; ++i) g_shm->hmdQuat[i] = q[i];
    g_shm->poseValid = p.bPoseIsValid ? 1 : 0;
    InterlockedIncrement(&g_shm->poseSeq);  // even: done
    if (g_poseWrites++ < 3 || g_poseWrites % 600 == 0) {
        Log("vr: hmd pose valid=%d pos=(%.3f %.3f %.3f) q=(%.3f %.3f %.3f %.3f)", p.bPoseIsValid ? 1 : 0,
            m[0][3], m[1][3], m[2][3], q[0], q[1], q[2], q[3]);
    }
}

// Copies the captured Pip-Boy render target into a shareable texture. Returns it, or nullptr.
static ID3D11Texture2D* VrPipboyTexture() {
    ID3D11Texture2D* src = nullptr;
    int count = 0;
    AcquireSRWLockShared(&g_pipLock);
    count = g_pipCandCount;
    if (count > 0) {
        // Prefer the target the game actually drew into; else [vr] pipboy_target_index.
        const LONG drawn = g_pipDrawnIdx;
        const int want = drawn >= 0 ? static_cast<int>(drawn) : g_vrPipIndex;
        const int idx = want < 0 ? 0 : (want >= count ? count - 1 : want);
        src = g_pipCand[idx];
        src->AddRef();
    }
    ReleaseSRWLockShared(&g_pipLock);
    if (!src) return nullptr;
    D3D11_TEXTURE2D_DESC d = {};
    src->GetDesc(&d);
    const DXGI_FORMAT fmt = TypedFormat(d.Format);
    if (src != g_pipTexSrc || !g_pipTex) {
        if (g_pipTex) { g_pipTex->Release(); g_pipTex = nullptr; }
        D3D11_TEXTURE2D_DESC e = {};
        e.Width = d.Width; e.Height = d.Height; e.MipLevels = 1; e.ArraySize = 1;
        e.Format = fmt; e.SampleDesc.Count = 1; e.Usage = D3D11_USAGE_DEFAULT;
        e.BindFlags = D3D11_BIND_SHADER_RESOURCE; e.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        if (!CreateFrameTexture(e, nullptr, 0, &g_pipTex)) {
            Log("vr: pipboy copy texture create failed (fmt %d)", (int)fmt);
            g_pipTex = nullptr;
        }
        g_pipTexSrc = src;
        Log("vr: pipboy overlay from target %p (%d candidates) %ux%u fmt=%d mips=%u samples=%u",
            (void*)src, count, d.Width, d.Height, (int)d.Format, d.MipLevels, d.SampleDesc.Count);
    }
    if (g_pipTex) {
        if (d.SampleDesc.Count > 1) g_ctx->ResolveSubresource(g_pipTex, 0, src, 0, fmt);
        else g_ctx->CopySubresourceRegion(g_pipTex, 0, 0, 0, 0, src, 0, nullptr);
    }
    src->Release();
    return g_pipTex;
}

// --- FSR1 upscale (AMD FidelityFX Super Resolution 1, MIT) ---
// EASU: backbuffer -> g_upMid (12-tap edge-aware scaling). RCAS: g_upMid -> eye texture
// (sharpen). The eye texture is the SteamVR-shared texture, so the final RCAS result is
// copied into it with CopyResource (shared textures must not carry a UAV bind flag).
struct FsrCb {
    UINT c0[4], c1[4], c2[4], c3[4], rcas[4], misc[4];  // matches cbUpscale in fsr1_main.hlsl (96 bytes)
};
static_assert(sizeof(FsrCb) == 96, "cbUpscale layout");

static UINT FsrBits(float f) { UINT u; memcpy(&u, &f, 4); return u; }

// Debug: reads one pixel back from a GPU texture and logs it. Staging copy forces a
// GPU sync, so this is only used for the first few frames while bring-up debugging.
static void LogUpPx(const char* tag, ID3D11Texture2D* src, UINT x, UINT y) {
    if (!g_dev || !g_ctx || !src) return;
    D3D11_TEXTURE2D_DESC d = {};
    src->GetDesc(&d);
    if (x >= d.Width) x = d.Width / 2;
    if (y >= d.Height) y = d.Height / 2;
    D3D11_TEXTURE2D_DESC s = d;
    s.Width = 1; s.Height = 1; s.MipLevels = 1; s.ArraySize = 1;
    s.SampleDesc.Count = 1; s.SampleDesc.Quality = 0;
    s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ; s.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&s, nullptr, &st)) || !st) return;
    D3D11_BOX box = { x, y, 0, x + 1, y + 1, 1 };
    g_ctx->CopySubresourceRegion(st, 0, 0, 0, 0, src, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        const unsigned char* p = static_cast<const unsigned char*>(m.pData);
        Log("vr: upscale px %s (%u,%u) = %02x %02x %02x %02x", tag, x, y, p[0], p[1], p[2], p[3]);
        g_ctx->Unmap(st, 0);
    } else {
        Log("vr: upscale px %s map failed", tag);
    }
    st->Release();
}

// FsrEasuCon() from ffx_fsr1.h (CPU section), transcribed to plain C++ so the proxy does
// not need to include the whole CPU half of ffx_a.h. Bit-exact to the original.
static void FsrEasuConFill(UINT* c, float inX, float inY, float resX, float resY) {
    const float rX = 1.0f / resX, rY = 1.0f / resY;
    c[0]  = FsrBits(inX * rX);
    c[1]  = FsrBits(inY * rY);
    c[2]  = FsrBits(0.5f * inX * rX - 0.5f);
    c[3]  = FsrBits(0.5f * inY * rY - 0.5f);
    c[4]  = FsrBits(1.0f / inX);
    c[5]  = FsrBits(1.0f / inY);
    c[6]  = FsrBits(1.0f / inX);
    c[7]  = FsrBits(-1.0f / inY);
    c[8]  = FsrBits(-1.0f / inX);
    c[9]  = FsrBits(2.0f / inY);
    c[10] = FsrBits(1.0f / inX);
    c[11] = FsrBits(2.0f / inY);
    c[12] = FsrBits(0.0f);
    c[13] = FsrBits(4.0f / inY);
    c[14] = c[15] = 0;
}

static void VrUpscaleInit() {
    if (!g_dev || g_upEasu || g_upFail) return;
    UINT sup = 0;
    if (FAILED(g_dev->CheckFormatSupport(DXGI_FORMAT_B8G8R8A8_UNORM, &sup))) sup = 0;
    if (!(sup & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)) {
        Log("vr: upscale: device lacks BGRA UAV support (fmt support %x)", sup);
        g_upFail = true;
        return;
    }
    // Join the embedded source lines once (each line is its own literal because this
    // MSVC rejects string literals over ~16.4 KB, C2026).
    static std::string src;
    static bool srcReady = false;
    if (!srcReady) {
        srcReady = true;
        for (UINT i = 0; i < g_fsr1PartCount; ++i) { src += g_fsr1Parts[i]; src += '\n'; }
    }
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    for (int pass = 0; pass < 2; ++pass) {
        const char* fn = pass == 0 ? "CsEasu" : "CsRcas";
        if (FAILED(D3DCompile(src.data(), src.size(), "fsr1", nullptr, nullptr, fn, "cs_5_0",
                              0, 0, &blob, &err))) {
            Log("vr: upscale: %s compile failed: %s", fn,
                err && err->GetBufferPointer() ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            if (err) err->Release();
            err = nullptr;
            g_upFail = true;
            return;
        }
        HRESULT hr = pass == 0 ? g_dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                           nullptr, &g_upEasu)
                               : g_dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                                            nullptr, &g_upRcas);
        blob->Release();
        blob = nullptr;
        if (FAILED(hr)) { Log("vr: upscale: %s create failed hr=%x", fn, (unsigned)hr); g_upFail = true; return; }
    }
    if (err) err->Release();
    D3D11_BUFFER_DESC cb = {};
    cb.ByteWidth = sizeof(FsrCb);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_dev->CreateBuffer(&cb, nullptr, &g_upCb))) {
        Log("vr: upscale: cb create failed");
        g_upFail = true;
        return;
    }
    // EASU gathers through s0; an unbound sampler slot gives undefined (black) results.
    D3D11_SAMPLER_DESC smp = {};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(g_dev->CreateSamplerState(&smp, &g_upSamp))) {
        Log("vr: upscale: sampler create failed");
        g_upFail = true;
        return;
    }
    Log("vr: upscale: FSR1 shaders compiled (sharpness %.2f)", g_vrUpSharp);
}

// Runs EASU+RCAS and copies the result into dst (an eye texture). Returns false when the
// upscale path is not available (caller then falls back to a plain copy/resolve).
static bool VrUpscale(ID3D11Texture2D* bb, ID3D11Texture2D* dst) {
    // Without the mid textures the caller's plain-copy fallback cannot work either
    // (the eye texture is larger than the backbuffer, CopyResource silently no-ops),
    // so disable the oversized eye textures for this session.
    if (!g_upW || !g_upH || !g_upMidUav || !g_upMidBUav) { g_upFail = true; return false; }
    VrUpscaleInit();
    if (!g_upEasu || !g_upRcas || !g_upCb || !g_upMidSrv) { g_upFail = true; return false; }

    D3D11_TEXTURE2D_DESC d = {};
    bb->GetDesc(&d);
    // The swapchain backbuffer carries no D3D11_BIND_SHADER_RESOURCE flag, so a view on
    // it cannot be created; copy it into our own SRV-bindable texture first.
    if (g_upSrcW != d.Width || g_upSrcH != d.Height || g_upSrcF != d.Format) {
        if (g_upSrc) { g_upSrc->Release(); g_upSrc = nullptr; }
        if (g_upSrcSrv) { g_upSrcSrv->Release(); g_upSrcSrv = nullptr; }
        D3D11_TEXTURE2D_DESC s = d;
        s.MipLevels = 1;
        s.ArraySize = 1;
        s.SampleDesc.Count = 1;
        s.SampleDesc.Quality = 0;
        s.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        s.MiscFlags = 0;
        if (!CreateFrameTexture(s, nullptr, 0, &g_upSrc)
                || FAILED(g_dev->CreateShaderResourceView(g_upSrc, nullptr, &g_upSrcSrv))) {
            Log("vr: upscale: src texture/SRV create failed");
            g_upFail = true;
            return false;
        }
        g_upSrcW = d.Width; g_upSrcH = d.Height; g_upSrcF = d.Format;
    }
    if (d.SampleDesc.Count > 1) g_ctx->ResolveSubresource(g_upSrc, 0, bb, 0, d.Format);
    else g_ctx->CopyResource(g_upSrc, bb);
    ID3D11ShaderResourceView* srcSrv = g_upSrcSrv;
    srcSrv->AddRef();

    // Constants: EASU in pass 1, RCAS in pass 2; output size gates the dispatch bounds.
    FsrCb cb = {};
    FsrEasuConFill(cb.c0, static_cast<float>(d.Width), static_cast<float>(d.Height),
                   static_cast<float>(g_upW), static_cast<float>(g_upH));
    cb.rcas[0] = FsrBits(exp2f(-g_vrUpSharp));
    cb.misc[0] = g_upW;
    cb.misc[1] = g_upH;
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (SUCCEEDED(g_ctx->Map(g_upCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &cb, sizeof(cb));
        g_ctx->Unmap(g_upCb, 0);
    } else {
        srcSrv->Release();
        Log("vr: upscale: cb map failed");
        g_upFail = true;
        return false;
    }

    // 8x8 tiles: a 1D layout needs (W*H+63)/64 groups in X, which at 2432x2524 is
    // 95912 > 65535 (D3D11 max thread groups per dimension) and the dispatch is
    // silently dropped, leaving the output black.
    const UINT gx = (g_upW + 7) / 8, gy = (g_upH + 7) / 8;
    ID3D11UnorderedAccessView* midUav = g_upMidUav;
    ID3D11Buffer* cbp = g_upCb;
    ID3D11SamplerState* smp = g_upSamp;
    g_ctx->CSSetSamplers(0, 1, smp ? &smp : nullptr);
    g_ctx->CSSetShader(g_upEasu, nullptr, 0);
    g_ctx->CSSetConstantBuffers(0, 1, &cbp);
    g_ctx->CSSetShaderResources(0, 1, &srcSrv);
    g_ctx->CSSetUnorderedAccessViews(0, 1, &midUav, nullptr);
    g_ctx->Dispatch(gx, gy, 1);

    ID3D11ShaderResourceView* midSrv = g_upMidSrv;
    ID3D11UnorderedAccessView* midBUav = g_upMidBUav;
    // Unbind midA from u0 BEFORE binding it as t0: D3D11 drops an SRV bind that
    // conflicts with an already-bound UAV, which left t0 null and zeroed every
    // RCAS load (EASU output lost -> black eye texture).
    ID3D11UnorderedAccessView* nullUav = nullptr;
    g_ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    g_ctx->CSSetShader(g_upRcas, nullptr, 0);
    g_ctx->CSSetShaderResources(0, 1, &midSrv);
    g_ctx->CSSetUnorderedAccessViews(0, 1, &midBUav, nullptr);
    g_ctx->Dispatch(gx, gy, 1);

    ID3D11ShaderResourceView* nsrv = nullptr;
    ID3D11UnorderedAccessView* nuav = nullptr;
    ID3D11SamplerState* nsmp = nullptr;
    g_ctx->CSSetShaderResources(0, 1, &nsrv);
    g_ctx->CSSetUnorderedAccessViews(0, 1, &nuav, nullptr);
    g_ctx->CSSetSamplers(0, 1, &nsmp);
    g_ctx->CSSetShader(nullptr, nullptr, 0);
    g_ctx->CopyResource(dst, g_upMidB);
    srcSrv->Release();
    // Bring-up debug: track where the image turns black (src=EASU input, easu/rcas=pass
    // outputs, eye=what SteamVR gets). First 3 calls only; staging copies sync the GPU.
    static int s_pxDbg = 0;
    if (s_pxDbg < 3) {
        ++s_pxDbg;
        Log("vr: upscale px debug frame %d (bb %ux%u -> eye %ux%u)", s_pxDbg, g_upSrcW, g_upSrcH, g_upW, g_upH);
        LogUpPx("src ", g_upSrc, g_upSrcW / 4, g_upSrcH / 4);
        LogUpPx("src ", g_upSrc, g_upSrcW / 2, g_upSrcH / 2);
        LogUpPx("easu", g_upMidA, g_upW / 4, g_upH / 4);
        LogUpPx("easu", g_upMidA, g_upW / 2, g_upH / 2);
        LogUpPx("rcas", g_upMidB, g_upW / 4, g_upH / 4);
        LogUpPx("rcas", g_upMidB, g_upW / 2, g_upH / 2);
        LogUpPx("eye ", dst, g_upW / 4, g_upH / 4);
        LogUpPx("eye ", dst, g_upW / 2, g_upH / 2);
    }
    return true;
}

// Called from Present before the original Present.
// --- software cursor for the 2D screen ---------------------------------------
// Fallout 4 draws menu pointers with the OS hardware cursor, which never lands in
// the UI render target, so the headset screen shows no pointer even though the
// game receives the mouse. Composite an arrow at the mouse position into the
// overlay texture (read-modify-write of a small rect; menus only).
static const UINT kCurW = 14, kCurH = 21;
static ID3D11Texture2D* g_curTex = nullptr;   // arrow sprite (GPU)
static ID3D11Texture2D* g_curStg = nullptr;   // staging scratch for the composite
static unsigned char g_curPx[kCurW * kCurH * 4];
static POINT g_curLastPos{};
static ULONGLONG g_curLastMoveMs = 0;

static void MakeCursorSprite() {
    if (g_curTex || !g_dev) return;
    // Classic arrow polygon: black outline, white fill.
    static const float px[7] = { 0.0f, 0.0f, 4.0f, 7.0f, 9.0f, 6.0f, 11.0f };
    static const float py[7] = { 0.0f, 16.0f, 12.0f, 19.0f, 18.0f, 11.0f, 11.0f };
    auto inside = [](float x, float y) {
        bool in = false;
        for (int i = 0, j = 6; i < 7; j = i++) {
            if ((py[i] > y) != (py[j] > y) &&
                x < (px[j] - px[i]) * (y - py[i]) / (py[j] - py[i]) + px[i]) {
                in = !in;
            }
        }
        return in;
    };
    auto edgeDist = [](float x, float y) {
        float best = 1e9f;
        for (int i = 0, j = 6; i < 7; j = i++) {
            const float dx = px[j] - px[i], dy = py[j] - py[i];
            float t = ((x - px[i]) * dx + (y - py[i]) * dy) / (dx * dx + dy * dy);
            if (t < 0.0f) t = 0.0f;
            else if (t > 1.0f) t = 1.0f;
            const float ex = px[i] + t * dx - x, ey = py[i] + t * dy - y;
            const float d = std::sqrt(ex * ex + ey * ey);
            if (d < best) best = d;
        }
        return best;
    };
    memset(g_curPx, 0, sizeof(g_curPx));
    for (UINT y = 0; y < kCurH; ++y) {
        for (UINT x = 0; x < kCurW; ++x) {
            const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
            unsigned char* p = &g_curPx[(y * kCurW + x) * 4];
            if (inside(fx, fy)) {
                p[0] = p[1] = p[2] = 255;
                p[3] = 255;
            } else if (edgeDist(fx, fy) < 1.1f) {
                p[3] = 255;  // outline (black)
            }
        }
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = kCurW;
    d.Height = kCurH;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = g_curPx;
    sd.SysMemPitch = kCurW * 4;
    if (FAILED(g_dev->CreateTexture2D(&d, &sd, &g_curTex))) g_curTex = nullptr;
}

static void DrawOverlayCursor(ID3D11Texture2D* dst) {
    if (!dst || !g_dev || !g_ctx || !g_gameHwnd) return;
    MakeCursorSprite();
    if (!g_curTex) return;
    POINT p{};
    RECT rc{};
    if (!GetCursorPos(&p) || !ScreenToClient(g_gameHwnd, &p) ||
            !GetClientRect(g_gameHwnd, &rc) || rc.right <= 0 || rc.bottom <= 0) {
        return;
    }
    if (p.x != g_curLastPos.x || p.y != g_curLastPos.y) {
        g_curLastPos = p;
        g_curLastMoveMs = GetTickCount64();
    } else if (GetTickCount64() - g_curLastMoveMs > 5000) {
        return;  // idle: hide the pointer
    }
    if (p.x < 0 || p.y < 0 || p.x >= rc.right || p.y >= rc.bottom) return;
    D3D11_TEXTURE2D_DESC td{};
    dst->GetDesc(&td);
    // The target is shown letterboxed/pillarboxed in the window (aspect kept,
    // centred) and the game hit-tests with that same mapping; mapping proportionally
    // over the whole window made the drawn cursor sit ~10% above the game's pointer.
    const double ta = static_cast<double>(td.Width) / static_cast<double>(td.Height);
    const double wa = static_cast<double>(rc.right) / static_cast<double>(rc.bottom);
    double sc, ox, oy;
    if (wa > ta) {  // window wider: fit height, pillarbox left/right
        sc = static_cast<double>(rc.bottom) / static_cast<double>(td.Height);
        ox = (static_cast<double>(rc.right) - static_cast<double>(td.Width) * sc) * 0.5;
        oy = 0.0;
    } else {  // window taller: fit width, letterbox top/bottom
        sc = static_cast<double>(rc.right) / static_cast<double>(td.Width);
        ox = 0.0;
        oy = (static_cast<double>(rc.bottom) - static_cast<double>(td.Height) * sc) * 0.5;
    }
    const int x0 = static_cast<int>(((static_cast<double>(p.x) - ox) / sc));
    const int y0 = static_cast<int>(((static_cast<double>(p.y) - oy) / sc));
    if (x0 < 0 || y0 < 0 || x0 >= static_cast<int>(td.Width) || y0 >= static_cast<int>(td.Height)) return;
    UINT w = kCurW, h = kCurH;
    if (x0 + static_cast<int>(w) > static_cast<int>(td.Width)) w = td.Width - static_cast<UINT>(x0);
    if (y0 + static_cast<int>(h) > static_cast<int>(td.Height)) h = td.Height - static_cast<UINT>(y0);
    if (w == 0 || h == 0) return;
    if (!g_curStg) {
        D3D11_TEXTURE2D_DESC s{};
        s.Width = kCurW;
        s.Height = kCurH;
        s.MipLevels = 1;
        s.ArraySize = 1;
        s.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        s.SampleDesc.Count = 1;
        s.Usage = D3D11_USAGE_STAGING;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g_dev->CreateTexture2D(&s, nullptr, &g_curStg))) g_curStg = nullptr;
    }
    if (!g_curStg) return;
    const D3D11_BOX box{ static_cast<UINT>(x0), static_cast<UINT>(y0), 0,
                         static_cast<UINT>(x0) + w, static_cast<UINT>(y0) + h, 1 };
    g_ctx->CopySubresourceRegion(g_curStg, 0, 0, 0, 0, dst, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g_ctx->Map(g_curStg, 0, D3D11_MAP_READ, 0, &m))) return;
    std::vector<unsigned char> px(w * h * 4);
    const unsigned char* src = static_cast<const unsigned char*>(m.pData);
    for (UINT y = 0; y < h; ++y) {
        memcpy(&px[y * w * 4], src + y * m.RowPitch, w * 4);
    }
    g_ctx->Unmap(g_curStg, 0);
    for (UINT y = 0; y < h; ++y) {
        for (UINT x = 0; x < w; ++x) {
            const unsigned char* a = &g_curPx[(y * kCurW + x) * 4];
            if (a[3]) {
                unsigned char* d = &px[(y * w + x) * 4];
                d[0] = a[0];
                d[1] = a[1];
                d[2] = a[2];
                d[3] = 255;
            }
        }
    }
    g_ctx->UpdateSubresource(dst, 0, &box, px.data(), w * 4, 0);
}

// Capture-key fallback (RenderDoc not loaded): save what is being shown as BMP files into
// <game>\fo4stereo_shots\: both eye textures in 3D mode, the 2D screen texture in 2D mode, tagged
// with camera state / zoom so HUD and dialogue frames can be inspected offline.
static void SaveShotFallback(bool flat, LONG camState) {
    const std::string dir = GameDir() + "fo4stereo_shots";
    CreateDirectoryA(dir.c_str(), nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamp[64];
    sprintf_s(stamp, sizeof(stamp), "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
              st.wMinute, st.wSecond);
    const LONG zoom = g_shm ? g_shm->zoom : -1;
    char tag[96];
    sprintf_s(tag, sizeof(tag), "%s_cam%ld_zoom%ld_%s", stamp, camState, zoom, flat ? "2D" : (g_monoAim ? "ads" : "3D"));
    const std::string stem = dir + "\\" + tag;
    bool ok = false;
    if (flat) {
        ok = SaveTextureBmp(g_flatTex, stem + "_screen.bmp");
    } else {
        const bool l = SaveTextureBmp(g_eyeTex[0], stem + "_left.bmp");
        const bool r = SaveTextureBmp(g_eyeTex[1], stem + "_right.bmp");
        ok = l || r;
    }
    Log("vr: screenshot %s -> %s (flat=%d camState=%ld zoom=%ld)", ok ? "saved" : "FAILED", stem.c_str(),
        flat ? 1 : 0, camState, zoom);
}

#if FO4STEREO_HAS_RENDERDOC_API
static RENDERDOC_API_1_6_0* g_renderDocApi = nullptr;
static bool g_renderDocCaptureActive = false;
static bool g_renderDocWildcardCapture = false;
static LONG g_renderDocStartFrame = -1;
static std::string g_renderDocCaptureLabel;

static bool InitRenderDocApi() {
    if (g_renderDocApi) return true;
    HMODULE module = GetModuleHandleA("renderdoc.dll");
    if (!module) {
        Log("renderdoc: API is not loaded; launch with capture_renderdoc.ps1");
        return false;
    }
    auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
    void* api = nullptr;
    if (!getApi || !getApi(eRENDERDOC_API_Version_1_6_0, &api) || !api) {
        Log("renderdoc: API 1.6.0 is unavailable");
        return false;
    }

    char modulePath[MAX_PATH] = {};
    GetModuleFileNameA(module, modulePath, MAX_PATH);
    int major = 0, minor = 0, patch = 0;
    auto* renderDocApi = static_cast<RENDERDOC_API_1_6_0*>(api);
    renderDocApi->GetAPIVersion(&major, &minor, &patch);
    const std::string outputDir = GameDir() + "fo4stereo_renderdoc";
    DWORD attrs = GetFileAttributesA(outputDir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryA(outputDir.c_str(), nullptr)) {
            Log("renderdoc: cannot create output directory %s (error=%lu)",
                outputDir.c_str(), GetLastError());
            return false;
        }
    } else if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        Log("renderdoc: output path is not a directory: %s", outputDir.c_str());
        return false;
    }

    g_renderDocApi = renderDocApi;
    Log("renderdoc: API ready version=%d.%d.%d module=%s output=%s",
        major, minor, patch, modulePath, outputDir.c_str());
    return true;
}

static void HandleRenderDocCapture(bool flat, LONG camState) {
    static bool keyWasDown = false;
    bool keyDown = false;
    if (g_renderDocCaptureKey > 0) {
        const SHORT ks = GetAsyncKeyState(g_renderDocCaptureKey);
        keyDown = (ks & 0x8000) != 0 || (ks & 0x0001) != 0;  // held, or tapped since the last call
    }
    const bool pressed = keyDown && !keyWasDown;
    keyWasDown = keyDown;
    if (pressed) Log("vr: capture key %d pressed (flat=%d camState=%ld)", g_renderDocCaptureKey, flat ? 1 : 0, camState);

    if (g_renderDocCaptureActive && g_frames > g_renderDocStartFrame) {
        const uint32_t saved = g_renderDocWildcardCapture
            ? g_renderDocApi->EndFrameCapture(nullptr, nullptr)
            : g_renderDocApi->EndFrameCapture(g_dev, g_gameHwnd);
        Log("renderdoc: capture complete label=%s saved=%u",
            g_renderDocCaptureLabel.c_str(), saved);
        g_renderDocCaptureActive = false;
        g_renderDocWildcardCapture = false;
        g_renderDocCaptureLabel.clear();
    }
    if (!pressed) return;
    if (!g_dev || !g_gameHwnd) return;
    // RenderDoc not loaded (the normal case: it cannot be injected while this proxy is d3d11.dll):
    // fall back to saving BMP screenshots of the eye / 2D-screen textures. Works in any camera state.
    if (!g_renderDocApi && !GetModuleHandleA("renderdoc.dll")) {
        SaveShotFallback(flat, camState);
        return;
    }
    if (!InitRenderDocApi()) { SaveShotFallback(flat, camState); return; }
    if (g_renderDocApi->IsFrameCapturing()) {
        Log("renderdoc: capture key ignored because another capture is active");
        return;
    }

    const char* label = g_monoAim ? "ADS" : "hipfire";
    const std::string outputTemplate = GameDir() + "fo4stereo_renderdoc\\fo4stereo_" + label;
    g_renderDocApi->SetCaptureFilePathTemplate(outputTemplate.c_str());
    g_renderDocApi->SetActiveWindow(g_dev, g_gameHwnd);
    g_renderDocApi->StartFrameCapture(g_dev, g_gameHwnd);
    if (!g_renderDocApi->IsFrameCapturing()) {
        Log("renderdoc: exact device/window capture unavailable; trying wildcard");
        g_renderDocApi->StartFrameCapture(nullptr, nullptr);
        if (!g_renderDocApi->IsFrameCapturing()) {
            Log("renderdoc: StartFrameCapture did not start (label=%s)", label);
            return;
        }
        g_renderDocWildcardCapture = true;
    }
    g_renderDocCaptureLabel = label;
    g_renderDocStartFrame = g_frames;
    g_renderDocCaptureActive = true;
    g_renderDocApi->SetCaptureTitle(label);
    Log("renderdoc: capture started label=%s template=%s", label, outputTemplate.c_str());
}
#else
static void HandleRenderDocCapture(bool flat, LONG camState) {
    static bool keyWasDown = false;
    bool keyDown = false;
    if (g_renderDocCaptureKey > 0) {
        const SHORT ks = GetAsyncKeyState(g_renderDocCaptureKey);
        keyDown = (ks & 0x8000) != 0 || (ks & 0x0001) != 0;
    }
    if (keyDown && !keyWasDown) {
        Log("vr: capture key %d pressed (flat=%d camState=%ld)", g_renderDocCaptureKey, flat ? 1 : 0, camState);
        if (g_dev) SaveShotFallback(flat, camState);
    }
    keyWasDown = keyDown;
}
#endif

static void VrOnPresent(IDXGISwapChain* sc) {
    VrInitOnce();
    // Frames presented before this one; the plugin picked this frame's eye from the same count.
    const LONG count = g_shm ? InterlockedIncrement(&g_shm->presents) - 1 : static_cast<LONG>(g_vrSubmits);
    if (!g_vrComp || !g_dev || !g_ctx) return;

    // SteamVR takes window focus when it starts; Fallout 4 pauses without focus
    // (black headset until the window is clicked). Give the game window focus once.
    static bool s_focused = false;
    if (!s_focused) {
        s_focused = true;
        DXGI_SWAP_CHAIN_DESC sd = {};
        if (SUCCEEDED(sc->GetDesc(&sd)) && sd.OutputWindow) {
            g_gameHwnd = sd.OutputWindow;
            const BOOL ok = SetForegroundWindow(sd.OutputWindow);
            Log("vr: focus game window %p -> %d", (void*)sd.OutputWindow, ok ? 1 : 0);
        }
    }

    // 2D screen while the engine camera is not updating (intro, main menu, loading).
    const LONG seq = g_shm ? g_shm->seq : 0;
    if (seq != g_lastSeq) { g_lastSeq = seq; g_seqStill = 0; }
    else if (g_seqStill < 1000000) g_seqStill++;
    // Zoom from the game projection: aiming narrows the FOV a little, the Pip-Boy a lot.
    LONG zoom = 0;
    // Engine-side aim flag from the plugin (PlayerCamera fovAdjust). It is exact and
    // one frame earlier than the projection heuristic: a heavy scope zoom narrows the
    // FOV below pipboy_ratio, which would flip the view to the 2D screen - the flag
    // keeps scope aiming in the VR view (mono). The Pip-Boy check below still wins.
    const bool pluginAim = g_shm && g_shm->aim == 1;
    if (g_coverH > 0.0f && g_gameTanX > 0.0f) {
        const float r = g_gameTanX / g_coverH;
        // Order matters: aim_ratio is checked first (narrower FOV), then pipboy_ratio.
        // If aim_ratio were checked last, the wider Pip-Boy threshold would
        // shadow it and no eye would ever be marked as aiming.
        zoom = (pluginAim || r < g_vrAimRatio) ? 1 : (r < g_vrPipboyRatio ? 2 : 0);
        // Hysteresis: the ADS FOV animates through the threshold and back, which
        // flickered the mono/full-IPD switch (double-vision flashes while aiming).
        // Enter below aim_ratio, hold until aim_ratio + 0.015.
        if (zoom == 0 && g_zoomPrev == 1 && r < g_vrAimRatio + 0.015f) zoom = 1;
    } else if (pluginAim) {
        zoom = 1;
    }
    // A narrow FOV is the Pip-Boy only if the game drew into its Pip-Boy target this frame;
    // otherwise (VATS zoom, third-person aim, scopes) treat it as aiming.
    const bool pipDrawn = g_pipEverDrawn && (count - g_pipDrawnAt) <= 10;
    static int s_pipDrawnPrev = -1;
    if (static_cast<int>(pipDrawn) != s_pipDrawnPrev) {
        Log("vr: pipboy target drawn=%d (target #%ld)", pipDrawn ? 1 : 0, g_pipDrawnIdx);
        s_pipDrawnPrev = pipDrawn ? 1 : 0;
    }
    if (zoom == 2 && g_vrPipProjected && g_pipEverDrawn && !pipDrawn) zoom = 1;
    const LONG camState = g_shm ? g_shm->camState : 0;
    if (g_shm) InterlockedExchange(&g_shm->zoom, zoom);
    // Calibration: track the min/max game-projection ratio seen in a ~5 s window so a
    // single play session (hipfire + a few ADS holds) reveals the real ADS ratio and
    // the correct aim_ratio threshold, without RenderDoc. Logs on change only.
    {
        static float s_rMin = 1e9f, s_rMax = -1e9f;
        static ULONGLONG s_winStart = 0, s_lastAds = 0;
        const float rNow = g_coverH > 0.0f && g_gameTanX > 0.0f ? g_gameTanX / g_coverH : -1.0f;
        const ULONGLONG nowMs = GetTickCount64();
        if (s_winStart == 0) s_winStart = nowMs;
        if (rNow > 0.0f) {
            if (rNow < s_rMin) s_rMin = rNow;
            if (rNow > s_rMax) s_rMax = rNow;
        }
        // Log the moment the ratio dips into "possible ADS" territory (< 1.0 = narrower than headset).
        if (rNow > 0.0f && rNow < 0.985f && (nowMs - s_lastAds) > 400) {
            s_lastAds = nowMs;
            Log("vr: CAL ratio=%.4f (tan %.3f) -> ADS candidate (aim_ratio now %.2f)", rNow, g_gameTanX, g_vrAimRatio);
        }
        if (nowMs - s_winStart >= 5000) {
            if (s_rMin < 1e8f) {
                Log("vr: CAL 5s window ratio min=%.4f max=%.4f", s_rMin, s_rMax);
            }
            s_rMin = 1e9f; s_rMax = -1e9f; s_winStart = nowMs;
        }
    }
    if (zoom != g_zoomPrev || camState != g_camStatePrev) {
        Log("vr: zoom=%ld (game tan x=%.3f, ratio %.3f) camState=%ld", zoom, g_gameTanX,
            g_coverH > 0.0f ? g_gameTanX / g_coverH : 0.0f, camState);
        g_zoomPrev = zoom; g_camStatePrev = camState;
    }
    // Pip-Boy open = the game draws into its Pip-Boy target (the FOV does not always narrow).
    const bool pipOpen = g_vrPipProjected && pipDrawn;
    const bool flat = !g_shm || g_seqStill >= g_vrFlatAfter || (zoom == 2 && g_vrPipboy2D) || (pipOpen && g_vrPipboy2D)
                      || (g_vrOtherCams2D && camState >= 0 && camState < 32 && !(g_vrCamMask & (1u << camState)));
    if (g_shm) InterlockedExchange(&g_shm->flatMode, flat ? 1 : 0);
    if (static_cast<int>(flat) != g_flatPrev) {
        Log("vr: mode -> %s (frame %ld seq %ld)", flat ? "2D screen" : "3D stereo", count, seq);
        g_flatPrev = flat ? 1 : 0;
    }
    // Mono aim = first-person aim: the plugin renders both frames from the same
    // offset (g_shm->zoom is exactly the value the plugin uses for aimFp), so the
    // eye bounds must collapse to one shared window (VrEyeBounds).
    // Live ADS presentation switch (Insert): 1 head-locked overlay -> 2 stereo like hipfire -> 0 mono eye
    // textures -> 1 ... Lets one play session compare the variants when diagnosing the aim double image.
    {
        static bool s_insDown = false;
        const SHORT ks = GetAsyncKeyState(VK_INSERT);
        const bool down = (ks & 0x8000) != 0 || (ks & 0x0001) != 0;
        if (down && !s_insDown) {
            g_adsMode = (g_adsMode + 1) % 3;
            Log("vr: ADS mode -> %ld (0 mono eye textures, 1 head-locked overlay, 2 stereo like hipfire)", g_adsMode);
        }
        s_insDown = down;
        if (g_shm) InterlockedExchange(&g_shm->adsMode, g_adsMode);
    }
    g_monoAim = !flat && zoom == 1 && camState == 0 && g_adsMode != 2;

    // [vr] mouse: SteamVR's dashboard takes foreground and the game stops receiving
    // mouse input (Pip-Boy UI dead, no cursor). Re-assert focus every ~2 s, but only
    // while the 3D view is active — when the player Alt+Tabs to the desktop we must
    // not steal focus back, or the cursor stays stuck in the game window.
    if (g_gameHwnd && !flat && (count & 127) == 0 && GetForegroundWindow() != g_gameHwnd) {
        const BOOL ok = SetForegroundWindow(g_gameHwnd);
        Log("vr: focus lost -> re-assert %p -> %d", (void*)g_gameHwnd, ok ? 1 : 0);
    }

    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb) return;
    VrEnsureTextures(bb);
    if (!g_eyeTex[0]) { bb->Release(); return; }

    // The plugin alternates the eye per camera update and records it in
    // shm->eye (published before the seq bump). Follow that record instead of
    // deriving the eye from the present count: (count - eyeDelay) & 1 raced
    // the camera updates whenever a present landed before the next update,
    // repeating an eye every few frames (constant left-right wobble).
    // eye_delay selects the update lag: 0 = the newest update's eye, 1 = the
    // one before it (the render thread lags the game thread by one update).
    static LONG s_seqSeen = -1, s_eyeCur = 0, s_eyePrev = 0;
    if (g_shm) {
        const LONG seq = g_shm->seq;
        if (seq != s_seqSeen) {
            s_seqSeen = seq;
            s_eyePrev = s_eyeCur;
            s_eyeCur = g_shm->eye;
        }
    }
    LONG eye = (g_vrEyeDelay ? s_eyePrev : s_eyeCur) & 1;
    if (g_vrSwap) eye ^= 1;

    ID3D11Texture2D* dst = flat ? g_flatTex : g_eyeTex[eye];
    D3D11_TEXTURE2D_DESC d = {};
    bb->GetDesc(&d);
    if (dst) {
        bool upscaled = false;
        if (!flat && g_upW && g_upMidUav && g_upMidBUav) {
            // [vr] upscale=1: FSR1 EASU+RCAS backbuffer -> eye texture at the headset size.
            upscaled = VrUpscale(bb, dst);
        }
        if (!upscaled) {
            if (!flat && g_upW) {
                // Size mismatch: CopyResource silently no-ops -> uninitialized (black) eye.
                static bool s_fbLogged = false;
                if (!s_fbLogged) {
                    s_fbLogged = true;
                    Log("vr: upscale unavailable, plain copy no-ops across sizes (bb %ux%u eye %ux%u)",
                        d.Width, d.Height, g_upW, g_upH);
                }
            }
            if (d.SampleDesc.Count > 1) g_ctx->ResolveSubresource(dst, 0, bb, 0, d.Format);
            else g_ctx->CopyResource(dst, bb);
        }
        // Mono aim: both eyes must show the SAME newest frame. Without this the eye
        // textures hold frames one game tick apart (the alternation history), and any
        // head/mouse motion while aiming turns that time gap into stereo disparity -
        // the double vision on ADS with a moving view. With identical textures both
        // eyes see the same image regardless of eye_delay or eye parity.
        if (!flat && g_monoAim && dst == g_eyeTex[eye]) {
            if (ID3D11Texture2D* other = g_eyeTex[eye ^ 1]; other && other != dst) {
                g_ctx->CopyResource(other, dst);
            }
        }
        // Remember which HMD pose this content was rendered with (the plugin read the pose
        // written by the previous present's WaitGetPoses when it built this frame).
        if (!flat) {
            g_eyePose[eye] = g_lastHmdPose;
            g_eyePoseOk[eye] = g_lastHmdPoseOk;
            if (g_monoAim) { g_eyePose[eye ^ 1] = g_lastHmdPose; g_eyePoseOk[eye ^ 1] = g_lastHmdPoseOk; }
        }
    }
    bb->Release();
    g_hudSrc = dst;  // this present's eye texture: freshly written, not yet masked

    if (g_vrCaptureAimFrames) {
        static int s_captureMode = -2;
        static int s_captureStableFrames = 0;
        static bool s_capturedHipfire = false;
        static bool s_capturedAim = false;
        const int captureMode = flat ? -1 : (g_monoAim ? 1 : 0);
        if (captureMode != s_captureMode) {
            s_captureMode = captureMode;
            s_captureStableFrames = 0;
        } else if (s_captureStableFrames < 150) {
            ++s_captureStableFrames;
        }

        // Burst diagnosis (capture_aim_frames=2): after the first stable hipfire
        // stereo frames, save 40 consecutive presents of BOTH eye textures. A
        // single-frame BMP cannot show the monocular double image (every frame is
        // individually clean); consecutive frames of the SAME texture can: if
        // g_eyeTex[0] alternates left-eye/right-eye renders (eye-pairing slip),
        // the near weapon jumps ~IPD between neighbouring frames. Offline pixel
        // diffing of the burst then proves (or kills) the flip hypothesis.
        static int s_burstRemaining = 0;
        static std::string s_burstStem;
        static unsigned s_burstFrame = 0;
        if (g_vrCaptureAimFrames >= 2) {
            static bool s_burstStarted = false;
            if (!s_burstStarted && captureMode >= 0 && seq > 0 && s_captureStableFrames >= 150) {
                s_burstStarted = true;
                s_burstRemaining = 40;
                s_burstFrame = 0;
                SYSTEMTIME now = {};
                GetLocalTime(&now);
                char stamp[64] = {};
                sprintf_s(stamp, sizeof(stamp), "fo4stereo_burst_%04u%02u%02u_%02u%02u%02u",
                    now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
                s_burstStem = GameDir() + stamp;
                Log("vr: burst capture start mode=%s seq=%ld eye=%ld frames=40 (game will hitch: 2 full-res BMP writes/frame)",
                    captureMode == 1 ? "aim" : "hipfire", seq, eye);
            }
            if (s_burstRemaining > 0 && captureMode >= 0) {
                --s_burstRemaining;
                char frameTag[16] = {};
                sprintf_s(frameTag, sizeof(frameTag), "_f%03u", s_burstFrame);
                const std::string leftPath = s_burstStem + frameTag + "_left.bmp";
                const std::string rightPath = s_burstStem + frameTag + "_right.bmp";
                const bool leftSaved = SaveTextureBmp(g_eyeTex[0], leftPath);
                const bool rightSaved = SaveTextureBmp(g_eyeTex[1], rightPath);
                Log("vr: burst frame %03u eye=%ld seq=%ld left=%d right=%d",
                    s_burstFrame++, eye, seq, leftSaved ? 1 : 0, rightSaved ? 1 : 0);
                if (s_burstRemaining == 0) {
                    Log("vr: burst capture complete (%u frames)", s_burstFrame);
                }
            }
        } else {
        bool& captured = captureMode == 1 ? s_capturedAim : s_capturedHipfire;
        // seq>0 = the game camera updates are flowing; without it the hipfire capture
        // lands on the loading screen (the first stable stereo frames are the load
        // fade-in). 150 stable frames (~2.5 s) lets the ADS animation and TAA settle,
        // otherwise the aim BMP catches the weapon mid-transition (motion ghost).
        if (captureMode >= 0 && seq > 0 && s_captureStableFrames >= 150 && !captured) {
            captured = true;
            SYSTEMTIME now = {};
            GetLocalTime(&now);
            char stamp[64] = {};
            sprintf_s(stamp, sizeof(stamp), "fo4stereo_capture_%04u%02u%02u_%02u%02u%02u_%s",
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                captureMode == 1 ? "aim" : "hipfire");
            const std::string stem = GameDir() + stamp;
            const std::string leftPath = stem + "_left.bmp";
            const std::string rightPath = stem + "_right.bmp";

            vr::VRTextureBounds_t leftBounds = { 0.0f, 0.0f, 1.0f, 1.0f };
            vr::VRTextureBounds_t rightBounds = { 0.0f, 0.0f, 1.0f, 1.0f };
            const bool leftCropped = g_vrUseBounds && VrEyeBounds(0, leftBounds);
            const bool rightCropped = g_vrUseBounds && VrEyeBounds(1, rightBounds);
            const char* modeName = captureMode == 1 ? "aim" : "hipfire";
            Log("vr: capture pair mode=%s seq=%ld eye=%ld zoom=%ld camState=%ld "
                "left=%p bounds=%d[%.3f..%.3f,%.3f..%.3f] "
                "right=%p bounds=%d[%.3f..%.3f,%.3f..%.3f]",
                modeName, seq, eye, zoom, camState, g_eyeTex[0], leftCropped ? 1 : 0,
                leftBounds.uMin, leftBounds.uMax, leftBounds.vMin, leftBounds.vMax,
                g_eyeTex[1], rightCropped ? 1 : 0, rightBounds.uMin, rightBounds.uMax,
                rightBounds.vMin, rightBounds.vMax);
            const bool leftSaved = SaveTextureBmp(g_eyeTex[0], leftPath);
            const bool rightSaved = SaveTextureBmp(g_eyeTex[1], rightPath);
            Log("vr: capture pair result left=%d path=%s right=%d path=%s",
                leftSaved ? 1 : 0, leftPath.c_str(), rightSaved ? 1 : 0, rightPath.c_str());
        }
        }
    }

    if (g_vrWaitPoses) {
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount] = {};
        g_vrComp->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
        // [vr] recenter_key (VK code): press -> the current head direction becomes the
        // new zero pose. Cancels the seated zero pose's residual yaw AND pitch (SteamVR's
        // ResetZeroPose resets position and yaw but never pitch), then re-places the
        // 2D screen onto the new forward direction.
        static bool s_rcDown = false;
        // Keyboard (default Delete; "pressed since last call" bit also catches short taps) OR the
        // gamepad combo L3+R3 (both stick clicks), since the keyboard may be out of reach in VR.
        bool keyDown = false;
        if (g_recenterKey) {
            const SHORT ks = GetAsyncKeyState(g_recenterKey);
            keyDown = (ks & 0x8000) != 0 || (ks & 0x0001) != 0;
        }
        const bool padDown = PadRecenterCombo();
        const bool rcDown = keyDown || padDown;
        if (rcDown && !s_rcDown) {
            Log("vr: recenter requested (%s)", padDown ? "gamepad L3+R3" : "keyboard");
        }
        if (rcDown && !s_rcDown) {
            const vr::TrackedDevicePose_t& hp = poses[vr::k_unTrackedDeviceIndex_Hmd];
            if (hp.bPoseIsValid) {
                float yaw = 0.0f, pitch = 0.0f;
                PoseYawPitch(hp.mDeviceToAbsoluteTracking, yaw, pitch);
                g_yawOff = -yaw;
                g_pitchOff = -pitch;
                Log("vr: recenter: head yaw=%.2f deg pitch=%.2f deg -> offsets set, 2D screen re-placed",
                    yaw * 57.2957795f, pitch * 57.2957795f);
                VrPlaceOverlay(g_overlayIsPip);
            }
        }
        s_rcDown = rcDown;
        VrWritePose(poses[vr::k_unTrackedDeviceIndex_Hmd]);
        g_lastHmdPose = poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
        g_lastHmdPoseOk = poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid;
    }

    // ADS as a head-locked overlay ([vr] ads_overlay=1): while aiming the view ignores head rotation
    // (plugin aim_headrot=0), i.e. the content is head-locked. Submitted as an eye texture the
    // compositor reprojects it as if it were world-locked, which shows up as a double image when the
    // head moves. An HMD-relative overlay is composited head-locked without any reprojection.
    const bool adsOv = g_adsMode == 1 && !flat && g_monoAim && g_vrOverlay
                       && g_overlay != vr::k_ulOverlayHandleInvalid && g_eyeTex[eye];
    // Conversation panel ([vr] dialogue_panel=1): the dialogue options and subtitles are drawn by the
    // game in the lower centre of the frame, which falls on/below the lower edge of the headset's
    // vertical FOV (asymmetric: ~55 deg up, ~44 deg down) and is tiny. Show a magnified crop of that
    // region as a head-relative panel in front of the lower view while the 3D scene keeps rendering.
    const bool dlgPanel = g_vrDlgPanel && !flat && !adsOv && camState == 12 && g_vrOverlay
                          && g_overlay != vr::k_ulOverlayHandleInvalid && g_eyeTex[eye]
                          && (!g_dlgGate || g_vrDlgFull || VrDialogueOptionsVisible(g_eyeTex[eye]));
    if (!dlgPanel && g_overlayIsDlg && g_vrOverlay && g_overlay != vr::k_ulOverlayHandleInvalid) {
        VrResetOverlayBounds();
    }
    if (dlgPanel) {
        const bool haveDlg = VrDialoguePanelUpdate(g_eyeTex[eye]) && g_dlgTex;
        vr::Texture_t ptex = { haveDlg ? g_dlgTex : g_eyeTex[eye], vr::TextureType_DirectX, vr::ColorSpace_Auto };
        g_vrOverlay->SetOverlayTexture(g_overlay, &ptex);
        if (!g_overlayIsDlg || !g_overlayShown) {
            g_overlayIsDlg = true;
            g_overlayIsAds = false;
            VrPlaceDialoguePanel();
            if (!g_overlayShown) { g_vrOverlay->ShowOverlay(g_overlay); g_overlayShown = true; }
            Log("vr: dialogue panel shown (crop u %.2f..%.2f v %.2f..%.2f, %.2f m wide at %.2f m)", g_dlgCrop[0],
                g_dlgCrop[2], g_dlgCrop[1], g_dlgCrop[3], g_vrDlgWidth, g_vrDlgDist);
        }
    } else if (adsOv) {
        vr::Texture_t atex = { g_eyeTex[eye], vr::TextureType_DirectX, vr::ColorSpace_Auto };
        g_vrOverlay->SetOverlayTexture(g_overlay, &atex);
        if (!g_overlayIsAds || !g_overlayShown) {
            g_overlayIsAds = true;
            VrPlaceAdsOverlay();
            if (!g_overlayShown) { g_vrOverlay->ShowOverlay(g_overlay); g_overlayShown = true; }
            Log("vr: ADS shown as head-locked overlay (dist %.1f m, game tan x=%.3f)", g_vrAdsOverlayDist, g_gameTanX);
        } else {
            VrPlaceAdsOverlay();  // width follows the (animating) ADS FOV
        }
    } else if (flat && g_vrOverlay && g_overlay != vr::k_ulOverlayHandleInvalid && g_flatTex) {
        if (g_overlayIsAds) {  // coming back from the ADS overlay: restore the seated 2D screen placement
            g_overlayIsAds = false;
            VrPlaceOverlay(g_overlayIsPip);
        }
        // Pip-Boy: show the game's Pip-Boy UI target (projected Pip-Boy) instead of the zoomed frame.
        ID3D11Texture2D* pip = pipOpen ? VrPipboyTexture() : nullptr;
        ID3D11Texture2D* shown = pip ? pip : g_flatTex;
        DrawOverlayCursor(shown);  // the OS cursor never lands in the render target
        vr::Texture_t ftex = { shown, vr::TextureType_DirectX, vr::ColorSpace_Auto };
        g_vrOverlay->SetOverlayTexture(g_overlay, &ftex);
        if ((pip != nullptr) != g_overlayIsPip) {
            g_overlayIsPip = pip != nullptr;
            VrPlaceOverlay(g_overlayIsPip);
            Log("vr: 2D screen shows %s", g_overlayIsPip ? "Pip-Boy target" : "game frame");
        }
        if (!g_overlayShown) {
            VrPlaceOverlay(g_overlayIsPip);
            g_vrOverlay->ShowOverlay(g_overlay);
            g_overlayShown = true;
        }
    } else if (!flat && g_overlayShown) {
        g_vrOverlay->HideOverlay(g_overlay);
        g_overlayShown = false;
        g_overlayIsAds = false;
    }

    // Own reticle: first-person 3D and ADS overlay (not the conversation panel).
    const bool wantReticle = g_vrReticle && !flat && camState == 0 && !dlgPanel && g_vrOverlay;
    if (wantReticle) {
        VrReticleInit();
        if (g_reticleOverlay != vr::k_ulOverlayHandleInvalid && !g_reticleShown) {
            g_vrOverlay->ShowOverlay(g_reticleOverlay);
            g_reticleShown = true;
        }
    } else if (g_reticleShown && g_reticleOverlay != vr::k_ulOverlayHandleInvalid) {
        g_vrOverlay->HideOverlay(g_reticleOverlay);
        g_reticleShown = false;
    }

    // HUD panels: first person 3D only (not conversation, not flat screens).
    VrTuningFrame();
    VrTuneOverlayFrame();
    VrHudPanelsFrame(g_hudPanelsOn && !flat && (camState == 0 || camState == 7) && !dlgPanel && !adsOv);

    ID3D11Texture2D* monoAimCaptureTexture = g_monoAim ? g_eyeTex[eye] : nullptr;
    static bool s_monoAimPrev = false;
    const bool monoAimChanged = g_monoAim != s_monoAimPrev;
    if (monoAimChanged) {
        Log("vr: mono aim capture texture=%p (capture eye=%ld)", monoAimCaptureTexture, eye);
        s_monoAimPrev = g_monoAim;
    }

    for (int i = 0; i < 2; ++i) {
        const bool dlgFullScreen = dlgPanel && g_vrDlgFull;  // full-frame head-locked screen (2D); else only a panel
        ID3D11Texture2D* src = (flat || adsOv || dlgFullScreen) ? g_blackTex : g_eyeTex[i];
        if (!src) continue;
        vr::Texture_t tex = { src, vr::TextureType_DirectX, vr::ColorSpace_Auto };
        vr::VRTextureBounds_t bounds = { 0.0f, 0.0f, 1.0f, 1.0f };
        const bool useBounds = !flat && !adsOv && !dlgFullScreen && g_vrUseBounds && VrEyeBounds(i, bounds);
        // Log on change (not just the first two): bounds moving per eye tells
        // whether the crop keeps a horizontal offset between the eyes (mono aim
        // must collapse them to the same window for a real single image).
        static float s_bU0[2] = { -1.0f, -1.0f }, s_bV0[2] = { -1.0f, -1.0f };
        if (useBounds && (std::fabs(bounds.uMin - s_bU0[i]) > 0.003f || std::fabs(bounds.vMin - s_bV0[i]) > 0.003f)) {
            s_bU0[i] = bounds.uMin;
            s_bV0[i] = bounds.vMin;
            Log("vr: eye %d bounds u=%.3f..%.3f v=%.3f..%.3f (game tan x=%.3f y=%.3f)", i, bounds.uMin, bounds.uMax,
                bounds.vMin, bounds.vMax, g_gameTanX, g_gameTanY);
        }
        // Hipfire stereo: tell the compositor the pose each eye was rendered with, so its
        // reprojection corrects the eye that lags a tick. Not used for mono aim (the view is
        // head-locked there) or the 2D/black frames.
        const bool withPose = g_vrSubmitPose && !flat && !g_monoAim && g_eyePoseOk[i];
        vr::VRTextureWithPose_t texPose = {};
        if (withPose) {
            texPose.handle = tex.handle;
            texPose.eType = tex.eType;
            texPose.eColorSpace = tex.eColorSpace;
            texPose.mDeviceToAbsoluteTracking = g_eyePose[i];
        }
        vr::EVRCompositorError e = g_vrComp->Submit(i == 0 ? vr::Eye_Left : vr::Eye_Right,
                                                    withPose ? static_cast<const vr::Texture_t*>(&texPose) : &tex,
                                                    useBounds ? &bounds : nullptr,
                                                    withPose ? vr::Submit_TextureWithPose : vr::Submit_Default);
        if (monoAimChanged) {
            Log("vr: submit eye=%d texture=%p mono=%d bounds=%d u=%.3f..%.3f v=%.3f..%.3f zoom=%ld camState=%ld seq=%ld err=%d",
                i, src, g_monoAim ? 1 : 0, useBounds ? 1 : 0, bounds.uMin, bounds.uMax, bounds.vMin, bounds.vMax,
                g_shm ? g_shm->zoom : -1, g_shm ? g_shm->camState : -1, seq, static_cast<int>(e));
        }
        if (e != vr::VRCompositorError_None) {
            g_vrLastErr = (int)e;
            if (g_vrSubmitErr++ < 10) Log("vr: submit eye %d error %d", i, (int)e);
        } else {
            g_vrSubmitOk++;
        }
    }
    g_vrComp->PostPresentHandoff();
    if (g_vrSubmits++ < 3 || g_vrSubmits % 600 == 0) {
        Log("vr: frame %u eye=%ld seq=%ld flat=%d submitOk=%u submitErr=%u lastErr=%d", g_vrSubmits, eye,
            seq, flat ? 1 : 0, g_vrSubmitOk, g_vrSubmitErr, g_vrLastErr);
    }
    HandleRenderDocCapture(flat, camState);
}

static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* self, UINT sync, UINT flags) {
    // One-time timing + stereo-config init, on the first present of ANY path.
    // This used to live at the tail of SaveTextureBmp and became unreachable
    // dead code after an edit, which left g_qpf==0 (t=inf), g_frameShift=NaN
    // and g_stereoEnabled=true-by-default -> the camera buffer patch wrote NaN
    // view-projections and the world vanished (only character+weapon visible).
    static bool s_stereoInited = false;
    if (!s_stereoInited) {
        s_stereoInited = true;
        QueryPerformanceFrequency(&g_qpf);
        QueryPerformanceCounter(&g_qpcStart);
        LoadStereoConfig();
        Log("stereo experiment: enabled=%d amp_units=%.3f period_sec=%.2f",
            g_stereoEnabled ? 1 : 0, g_ampUnits, g_periodSec);
    }
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
    // The game presents with sync interval 2 (30 fps at 60 Hz). With VR running, the
    // compositor's WaitGetPoses paces frames to the headset, so drop the game's vsync.
    const UINT useSync = (g_vrComp && g_vrNoVsync) ? 0 : sync;
    return g_origPresent(self, useSync, flags);
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
// Pip-Boy detection: the game draws into its Pip-Boy target only while the Pip-Boy is open.
// RTVs of the captured targets are recorded at creation; binding one marks the frame.

static LONG PresentCount() { return g_shm ? g_shm->presents : 0; }

static void NotePipRtvs(UINT n, ID3D11RenderTargetView* const* rtvs) {
    const LONG cnt = g_pipRtvCount;
    if (!rtvs || cnt == 0) return;
    for (UINT i = 0; i < n; ++i) {
        if (!rtvs[i]) continue;
        for (LONG k = 0; k < cnt; ++k) {
            if (g_pipRtv[k] == rtvs[i]) {
                InterlockedExchange(&g_pipDrawnAt, PresentCount());
                InterlockedExchange(&g_pipDrawnIdx, g_pipRtvCand[k]);
                InterlockedExchange(&g_pipEverDrawn, 1);
                return;
            }
        }
    }
}

static void STDMETHODCALLTYPE HookedOMSetRT(ID3D11DeviceContext* self, UINT n, ID3D11RenderTargetView* const* rtvs,
                                            ID3D11DepthStencilView* dsv) {
    NotePipRtvs(n, rtvs);
    FindVt(self)->omSetRT(self, n, rtvs, dsv);
}

static void STDMETHODCALLTYPE HookedOMSetRTUav(ID3D11DeviceContext* self, UINT n, ID3D11RenderTargetView* const* rtvs,
                                               ID3D11DepthStencilView* dsv, UINT uavStart, UINT nUav,
                                               ID3D11UnorderedAccessView* const* uavs, const UINT* counts) {
    if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) NotePipRtvs(n, rtvs);
    FindVt(self)->omSetRTUav(self, n, rtvs, dsv, uavStart, nUav, uavs, counts);
}

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
    vt.omSetRT = reinterpret_cast<OMSetRTFn>(vtbl[kOMSetRenderTargets]);
    vt.omSetRTUav = reinterpret_cast<OMSetRTUavFn>(vtbl[kOMSetRenderTargetsAndUAVs]);
    InterlockedExchange(&g_nvt, n + 1);  // publish before patching

    SetVtEntry(vtbl, kVSSetConstantBuffers, reinterpret_cast<void*>(&HookedVSSetCB));
    SetVtEntry(vtbl, kDrawIndexed, reinterpret_cast<void*>(&HookedDrawIndexed));
    SetVtEntry(vtbl, kDraw, reinterpret_cast<void*>(&HookedDraw));
    SetVtEntry(vtbl, kMap, reinterpret_cast<void*>(&HookedMap));
    SetVtEntry(vtbl, kUnmap, reinterpret_cast<void*>(&HookedUnmap));
    SetVtEntry(vtbl, kCopySubresourceRegion, reinterpret_cast<void*>(&HookedCopySub));
    SetVtEntry(vtbl, kCopyResource, reinterpret_cast<void*>(&HookedCopyRes));
    SetVtEntry(vtbl, kUpdateSubresource, reinterpret_cast<void*>(&HookedUpdateSub));
    SetVtEntry(vtbl, kOMSetRenderTargets, reinterpret_cast<void*>(&HookedOMSetRT));
    SetVtEntry(vtbl, kOMSetRenderTargetsAndUAVs, reinterpret_cast<void*>(&HookedOMSetRTUav));
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

typedef HRESULT(STDMETHODCALLTYPE* CreateTex2DFn)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                                  const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
static CreateTex2DFn g_origCreateTex2D = nullptr;

static bool IsDepthFormat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R24G8_TYPELESS || f == DXGI_FORMAT_D24_UNORM_S8_UINT || f == DXGI_FORMAT_R32_TYPELESS
        || f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_R16_TYPELESS || f == DXGI_FORMAT_D16_UNORM
        || f == DXGI_FORMAT_R32G8X24_TYPELESS || f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

// Records the game's Pip-Boy UI render target(s): textures of the uPipboyTarget size that are
// colour render targets. Our own textures have no render-target bind and are not recorded.
static HRESULT STDMETHODCALLTYPE HookedCreateTex2D(ID3D11Device* self, const D3D11_TEXTURE2D_DESC* desc,
                                                   const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    HRESULT hr = g_origCreateTex2D(self, desc, init, out);
    if (SUCCEEDED(hr) && desc && out && *out && desc->Width == g_pipTargetW && desc->Height == g_pipTargetH
        && (desc->BindFlags & D3D11_BIND_RENDER_TARGET) && !IsDepthFormat(desc->Format)) {
        AcquireSRWLockExclusive(&g_pipLock);
        if (g_pipCandCount < 8) {
            (*out)->AddRef();  // kept for the process lifetime (created once at startup)
            g_pipCand[g_pipCandCount++] = *out;
        }
        const int n = g_pipCandCount;
        ReleaseSRWLockExclusive(&g_pipLock);
        Log("vr: pipboy-size render target #%d %p fmt=%d bind=0x%x misc=0x%x mips=%u array=%u samples=%u", n - 1,
            (void*)*out, (int)desc->Format, desc->BindFlags, desc->MiscFlags, desc->MipLevels, desc->ArraySize,
            desc->SampleDesc.Count);
    }
    return hr;
}

typedef HRESULT(STDMETHODCALLTYPE* CreateRtvFn)(ID3D11Device*, ID3D11Resource*, const D3D11_RENDER_TARGET_VIEW_DESC*,
                                                ID3D11RenderTargetView**);
static CreateRtvFn g_origCreateRtv = nullptr;

static HRESULT STDMETHODCALLTYPE HookedCreateRtv(ID3D11Device* self, ID3D11Resource* res,
                                                 const D3D11_RENDER_TARGET_VIEW_DESC* desc,
                                                 ID3D11RenderTargetView** out) {
    HRESULT hr = g_origCreateRtv(self, res, desc, out);
    if (SUCCEEDED(hr) && res && out && *out) {
        int cand = -1;
        AcquireSRWLockShared(&g_pipLock);
        for (int i = 0; i < g_pipCandCount; ++i) {
            if (static_cast<ID3D11Resource*>(g_pipCand[i]) == res) { cand = i; break; }
        }
        ReleaseSRWLockShared(&g_pipLock);
        if (cand >= 0) {
            AcquireSRWLockExclusive(&g_pipLock);
            const LONG n = g_pipRtvCount;
            if (n < 32) {
                (*out)->AddRef();  // keep the pointer valid for comparisons
                g_pipRtv[n] = *out;
                g_pipRtvCand[n] = cand;
                InterlockedExchange(&g_pipRtvCount, n + 1);
            }
            ReleaseSRWLockExclusive(&g_pipLock);
            Log("vr: pipboy target #%d render target view %p", cand, (void*)*out);
        }
    }
    return hr;
}

static void HookDevice(ID3D11Device* dev) {
    void** vtbl = *reinterpret_cast<void***>(dev);
    // ID3D11Device: IUnknown(3) => CreateDeferredContext = 27
    HookVtableEntry(vtbl, 27, reinterpret_cast<void*>(&HookedCreateDeferred),
                    reinterpret_cast<void**>(&g_origCreateDeferred));
    if (g_vrEnabled && g_vrPipProjected) {
        LoadPipboyTargetSize();
        // ID3D11Device: IUnknown(3), CreateBuffer 3, CreateTexture1D 4 => CreateTexture2D = 5
        HookVtableEntry(vtbl, 5, reinterpret_cast<void*>(&HookedCreateTex2D),
                        reinterpret_cast<void**>(&g_origCreateTex2D));
        // CreateShaderResourceView 7, CreateUnorderedAccessView 8 => CreateRenderTargetView = 9
        HookVtableEntry(vtbl, 9, reinterpret_cast<void*>(&HookedCreateRtv),
                        reinterpret_cast<void**>(&g_origCreateRtv));
    }
    Log("Hooked device %p (CreateDeferredContext%s)", (void*)dev,
        g_origCreateTex2D ? ", CreateTexture2D" : "");
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

// Re-entrancy guard. When RenderDoc and the Steam overlay both hook D3D11 creation while our
// proxy is itself named d3d11.dll, their "original function" can point back at this proxy and
// recurse until the stack overflows (crash dumps 15.11.2026). A nested call on the same thread
// can never be satisfied, so fail it cleanly instead of recursing.
static thread_local int t_createDepth = 0;
struct CreateGuard {
    bool nested;
    CreateGuard() : nested(t_createDepth > 0) { ++t_createDepth; }
    ~CreateGuard() { --t_createDepth; }
};
static void LogNestedCreate(const char* what) {
    static LONG s_count = 0;
    if (InterlockedIncrement(&s_count) <= 3)
        Log("%s: nested call from another hook (RenderDoc/overlay loop?) refused to avoid stack overflow", what);
}

extern "C" HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* levels, UINT numLevels, UINT sdkVersion,
    const DXGI_SWAP_CHAIN_DESC* scDesc, IDXGISwapChain** outSwapChain,
    ID3D11Device** outDevice, D3D_FEATURE_LEVEL* outLevel, ID3D11DeviceContext** outContext)
{
    CreateGuard guard;
    if (guard.nested) { LogNestedCreate("D3D11CreateDeviceAndSwapChain"); return E_FAIL; }
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
    CreateGuard guard;
    if (guard.nested) { LogNestedCreate("D3D11CreateDevice"); return E_FAIL; }
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
