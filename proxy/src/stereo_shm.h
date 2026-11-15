// Shared memory between the F4SE plugin (fo4flat_probe.dll, engine camera) and the
// d3d11.dll proxy (frame capture + OpenVR). Both sides open or create the same
// named mapping. v2 adds the HMD pose and the 2D (flat screen) mode flag.
// v3 adds the settings block: the plugin reads the MCM user settings
// (Data\MCM\Settings\F4Flat2VR.ini) and pushes them here, so both sides agree
// without restarts.
#pragma once
#include <windows.h>

#define FO4STEREO_SHM_NAME "Local\\fo4stereo_shm_v3"
#define FO4STEREO_SHM_MAGIC 0x46345633  // 'F4V3'

// Live settings written by the plugin (seqlock on seq). The proxy applies the
// proxy-owned fields per frame; the plugin applies the engine-owned ones itself.
struct StereoSettings
{
	volatile LONG seq;             // odd while writing; bump on every change
	volatile LONG valid;           // 1 once the plugin has written this block
	float ipdUnits;                // [engine] ipd_units
	float aimIpdScale;             // [engine] aim_ipd_scale
	float convergeM;               // [engine] convergence_m
	float unitsPerMeter;           // [engine] units_per_meter
	float aimRatio;                // [vr] aim_ratio (zoom detection)
	float pipboyRatio;             // [vr] pipboy_ratio (zoom detection)
	float screenWidthM;            // [vr] screen_width_m (2D overlay)
	float screenDistanceM;         // [vr] screen_distance_m (2D overlay)
	float pipWidthM;               // [vr] pipboy_screen_width_m
	float pipDistanceM;            // [vr] pipboy_screen_distance_m
	float upscaleSharpness;        // [vr] upscale_sharpness (FSR1 RCAS)
	LONG eyeDelay;                 // [vr] eye_delay
	LONG swapEyes;                 // [vr] swap_eyes
	LONG waitPoses;                // [vr] wait_poses
	LONG upscale;                  // [vr] upscale (texture sizing: restart needed)
	LONG pad[3];
};

struct StereoShm
{
	volatile LONG magic;     // FO4STEREO_SHM_MAGIC once initialised
	volatile LONG eye;       // eye the plugin used for the latest camera update (0 left, 1 right)
	volatile LONG seq;       // incremented by the plugin on every camera-state update
	volatile LONG presents;  // incremented by the proxy on every Present; plugin derives the eye from it
	volatile LONG flatMode;  // proxy: 1 while no camera updates (menus, intro, loading) -> 2D screen
	volatile LONG poseSeq;   // proxy: seqlock for the pose below (odd while writing)
	volatile LONG poseValid; // proxy: 1 when the HMD pose is tracked
	volatile LONG zoom;      // proxy: 0 normal, 1 aiming (narrowed FOV), 2 very narrow (Pip-Boy) -> 2D screen
	volatile LONG camState;  // plugin: active camera state index (0 = first person; VATS = 2, 3rd person = 8, ...)
	volatile LONG pad2;
	float hmdPos[3];         // HMD position, OpenVR seated space, metres (x right, y up, -z forward)
	float hmdQuat[4];        // HMD rotation, OpenVR seated space, quaternion (w, x, y, z)
	StereoSettings cfg;      // live settings, written by the plugin
};

inline StereoShm* OpenStereoShm()
{
	HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(StereoShm),
		FO4STEREO_SHM_NAME);
	if (!h) {
		return nullptr;
	}
	auto* p = static_cast<StereoShm*>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(StereoShm)));
	if (p && p->magic != FO4STEREO_SHM_MAGIC) {
		ZeroMemory(const_cast<StereoShm*>(p), sizeof(StereoShm));
		InterlockedExchange(&p->magic, FO4STEREO_SHM_MAGIC);
	}
	return p;  // the mapping handle is intentionally kept open for the process lifetime
}

// Reads the settings block consistently (seqlock). Returns false if not valid.
// Field-by-field: the struct has volatile members, and a whole-struct copy is
// not guaranteed to touch them the way the seqlock needs.
inline bool ReadStereoSettings(const StereoSettings* s, StereoSettings* out)
{
	for (int tries = 0; tries < 8; ++tries) {
		const LONG a = s->seq;
		if (a & 1) {
			continue;
		}
		MemoryBarrier();
		out->seq = a;
		out->valid = s->valid;
		out->ipdUnits = s->ipdUnits;
		out->aimIpdScale = s->aimIpdScale;
		out->convergeM = s->convergeM;
		out->unitsPerMeter = s->unitsPerMeter;
		out->aimRatio = s->aimRatio;
		out->pipboyRatio = s->pipboyRatio;
		out->screenWidthM = s->screenWidthM;
		out->screenDistanceM = s->screenDistanceM;
		out->pipWidthM = s->pipWidthM;
		out->pipDistanceM = s->pipDistanceM;
		out->upscaleSharpness = s->upscaleSharpness;
		out->eyeDelay = s->eyeDelay;
		out->swapEyes = s->swapEyes;
		out->waitPoses = s->waitPoses;
		out->upscale = s->upscale;
		MemoryBarrier();
		if (s->seq == a) {
			return out->valid != 0;
		}
	}
	return false;
}

// Writes the settings block (seqlock). The payload is copied field by field so
// the seq counter is only ever touched by the interlocked increments: a whole-
// struct copy would overwrite seq with the caller's zero and leave it odd,
// which the readers read as "write in progress" forever (this exact bug shipped
// once and made the proxy ignore the block).
inline void WriteStereoSettings(StereoSettings* s, const StereoSettings& v)
{
	InterlockedIncrement(&s->seq);  // odd: writing
	s->valid = 1;
	s->ipdUnits = v.ipdUnits;
	s->aimIpdScale = v.aimIpdScale;
	s->convergeM = v.convergeM;
	s->unitsPerMeter = v.unitsPerMeter;
	s->aimRatio = v.aimRatio;
	s->pipboyRatio = v.pipboyRatio;
	s->screenWidthM = v.screenWidthM;
	s->screenDistanceM = v.screenDistanceM;
	s->pipWidthM = v.pipWidthM;
	s->pipDistanceM = v.pipDistanceM;
	s->upscaleSharpness = v.upscaleSharpness;
	s->eyeDelay = v.eyeDelay;
	s->swapEyes = v.swapEyes;
	s->waitPoses = v.waitPoses;
	s->upscale = v.upscale;
	MemoryBarrier();
	InterlockedIncrement(&s->seq);  // even: done
}

// Reads the HMD pose consistently (seqlock). Returns false if not valid.
inline bool ReadHmdPose(const StereoShm* s, float pos[3], float quat[4])
{
	for (int tries = 0; tries < 4; ++tries) {
		const LONG a = s->poseSeq;
		if (a & 1) {
			continue;
		}
		MemoryBarrier();
		const bool valid = s->poseValid != 0;
		for (int i = 0; i < 3; ++i) pos[i] = s->hmdPos[i];
		for (int i = 0; i < 4; ++i) quat[i] = s->hmdQuat[i];
		MemoryBarrier();
		if (s->poseSeq == a) {
			return valid;
		}
	}
	return false;
}
