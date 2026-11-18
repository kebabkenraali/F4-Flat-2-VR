// Shared memory between the F4SE plugin (fo4flat_probe.dll, engine camera) and
// the d3d11.dll proxy (frame capture + OpenVR). v4 contains per-frame camera,
// eye, zoom, and HMD pose state; settings come from fo4stereo.ini.
#pragma once
#include <windows.h>

#define FO4STEREO_SHM_NAME "Local\\fo4stereo_shm_v4"
#define FO4STEREO_SHM_MAGIC 0x46345634  // 'F4V4'

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
	volatile LONG aim;       // plugin: 1 when the engine itself is zoomed for aiming (PlayerCamera fovAdjust);
	volatile LONG runtimeIpdValid; // proxy: 1 once runtimeIpdM has been read from OpenVR
	float runtimeIpdM;       // proxy: runtime IPD in metres (Prop_UserIpdMeters_Float; adapts to the wearer)
	float hmdPos[3];         // HMD position, OpenVR seated space, metres (x right, y up, -z forward)
	float hmdQuat[4];        // HMD rotation, OpenVR seated space, quaternion (w, x, y, z)
	volatile LONG adsMode;   // proxy: ADS presentation (0 mono eye textures, 1 head-locked overlay, 2 stereo like hipfire)
	volatile LONG hudSeq;    // plugin: seqlock for the HUD values below (odd while writing)
	volatile LONG hudValid;  // plugin: 1 once the Papyrus script has sent values
	float hudHp;             // plugin: current health (Papyrus Game.GetHealthAV)
	float hudMaxHp;          // plugin: base health
	float hudReserve;        // plugin: reserve ammo for the equipped weapon (-1 = no weapon / unknown)
};

// Reads the HUD values consistently (seqlock). Returns false if the script has not sent values yet.
inline bool ReadHud(const StereoShm* s, float& hp, float& maxHp, float& reserve)
{
	for (int tries = 0; tries < 4; ++tries) {
		const LONG a = s->hudSeq;
		if (a & 1) {
			continue;
		}
		MemoryBarrier();
		const bool valid = s->hudValid != 0;
		hp = s->hudHp;
		maxHp = s->hudMaxHp;
		reserve = s->hudReserve;
		MemoryBarrier();
		if (s->hudSeq == a) {
			return valid;
		}
	}
	return false;
}

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
