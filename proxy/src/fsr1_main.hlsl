// Wrapper for the AMD FidelityFX Super Resolution 1 compute shaders (MIT license).
// This file is concatenated after ffx_a.h and ffx_fsr1.h by make_fsr1_header.py into
// the embedded shader source in fsr1_shaders.h. See AGENT_GUIDE.md.
//
// Pass 1 (CsEasu):  tSrc = game backbuffer  -> rwOut = intermediate (EASU, 12-tap edge-aware)
// Pass 2 (CsRcas):  tSrc = intermediate     -> rwOut = eye texture      (RCAS sharpen)
// Input must be gamma/sRGB-encoded (the game framebuffer is); output is written with the
// same encoding, so the submitted eye texture matches the un-upscaled path. Non-temporal:
// no frame history, safe with the alternate-frame stereo. UAVs are only written, never read.

Texture2D tSrc : register(t0);
SamplerState sLin : register(s0);
RWTexture2D<float4> rwOut : register(u0);

cbuffer cbUpscale : register(b0) {
    uint4 cCon0;   // FsrEasuCon constants 0..3 (bit-packed floats)
    uint4 cCon1;
    uint4 cCon2;
    uint4 cCon3;
    uint4 cRcas;   // x = bit pattern of exp2(-sharpness) (RCAS pass only)
    uint4 cMisc;   // xy = output size in pixels (dispatch bounds)
};

AF4 FsrEasuRF(AF2 p){return AF4(tSrc.GatherRed(sLin,p));}
AF4 FsrEasuGF(AF2 p){return AF4(tSrc.GatherGreen(sLin,p));}
AF4 FsrEasuBF(AF2 p){return AF4(tSrc.GatherBlue(sLin,p));}

// 8x8 tiles instead of a 1D layout: (W*H+63)/64 thread groups in X alone is 95912 at
// 2432x2524, busting the D3D11 limit of 65535 groups per dimension (the dispatch is
// then dropped silently and the output stays black). Tiles stay small in both axes.
[numthreads(8,8,1)]
void CsEasu(uint3 tid : SV_DispatchThreadID){
    if (tid.x >= cMisc.x || tid.y >= cMisc.y) return;
    uint2 ip = tid.xy;
    AF3 c;
    FsrEasuF(c, ip, cCon0, cCon1, cCon2, cCon3);
    rwOut[ip] = AF4(c, 1.0);
}

[numthreads(8,8,1)]
void CsRcas(uint3 tid : SV_DispatchThreadID){
    if (tid.x >= cMisc.x || tid.y >= cMisc.y) return;
    uint2 ip = tid.xy;
    AF1 r,g,b;
    FsrRcasF(r, g, b, ip, cRcas);
    rwOut[ip] = AF4(r, g, b, 1.0);
}

AF4 FsrRcasLoadF(ASU2 p){return AF4(tSrc.Load(int3(int2(p),0)));}
void FsrRcasInputF(inout AF1 r,inout AF1 g,inout AF1 b){}
