// Shared memory between the F4SE plugin (fo4flat_probe.dll, engine camera) and the
// d3d11.dll proxy (frame capture + OpenVR). Both sides open or create the same
// named mapping. v2 adds the HMD pose and the 2D (flat screen) mode flag.
#pragma once
#include <windows.h>

#define FO4STEREO_SHM_NAME "Local\\fo4stereo_shm_v2"
#define FO4STEREO_SHM_MAGIC 0x46345632  // 'F4V2'

struct StereoShm
{
	volatile LONG magic;     // FO4STEREO_SHM_MAGIC once initialised
	volatile LONG eye;       // eye the plugin used for the latest camera update (0 left, 1 right)
	volatile LONG seq;       // incremented by the plugin on every camera-state update
	volatile LONG presents;  // incremented by the proxy on every Present; plugin derives the eye from it
	volatile LONG flatMode;  // proxy: 1 while no camera updates (menus, intro, loading) -> 2D screen
	volatile LONG poseSeq;   // proxy: seqlock for the pose below (odd while writing)
	volatile LONG poseValid; // proxy: 1 when the HMD pose is tracked
	volatile LONG pad;
	float hmdPos[3];         // HMD position, OpenVR seated space, metres (x right, y up, -z forward)
	float hmdQuat[4];        // HMD rotation, OpenVR seated space, quaternion (w, x, y, z)
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
