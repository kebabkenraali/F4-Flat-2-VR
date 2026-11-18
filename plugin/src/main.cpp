// Minimal probe plugin for flat Fallout 4 (runtime 1.11.240).
// Logs the F4SE runtime version and Xbox controller (XInput) state for ~30 seconds.
// Diagnostic only.

// CommonLibF4 headers MUST come before <windows.h>: WinAPI.h declares
// constexpr constants (MEM_RELEASE, PAGE_EXECUTE_READWRITE, ...) whose names
// collide with macros defined by windows.h.
#include "F4SE/F4SE.h"
#include "RE/Fallout.h"
#include "../../proxy/src/stereo_shm.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/spdlog.h"

#include <windows.h>
#include <xinput.h>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <variant>
#include <thread>
#include <cmath>

#ifndef DLLEXPORT
#define DLLEXPORT __declspec(dllexport)
#endif

// F4SE only loads plugins that export version data (F4SEPlugin_Version).
// Struct layout from F4SE's f4se/PluginAPI.h (not provided by vendored CommonLibF4).
struct F4SEPluginVersionData
{
	enum : std::uint32_t
	{
		kVersion = 1
	};

	enum : std::uint32_t
	{
		kAddressIndependence_Signatures = 1 << 0,
		kAddressIndependence_AddressLibrary_1_10_980 = 1 << 1,
		kAddressIndependence_AddressLibrary_1_11_137 = 1 << 2
	};

	enum : std::uint32_t
	{
		kStructureIndependence_NoStructs = 1 << 0,
		kStructureIndependence_1_10_980Layout = 1 << 1,
		kStructureIndependence_1_11_137Layout = 1 << 2
	};

	std::uint32_t dataVersion;              // kVersion
	std::uint32_t pluginVersion;            // version of this plugin
	char name[256];                         // null-terminated plugin name
	char author[256];                       // null-terminated author (may be empty)
	std::uint32_t addressIndependence;      // kAddressIndependence_* bitfield
	std::uint32_t structureIndependence;    // kStructureIndependence_* bitfield
	std::uint32_t compatibleVersions[16];   // zero-terminated RUNTIME_VERSION list
	std::uint32_t seVersionRequired;        // minimum F4SE version required (0 = any)
	std::uint32_t reservedNonBreaking;      // 0
	std::uint32_t reservedBreaking;         // 0
	std::uint8_t reserved[512];             // 0
};

// Runtime version 1.11.240.0 packed the way F4SE packs it:
// major = (v >> 24), minor = (v >> 16), patch = (v >> 4) & 0xFFF, build = v & 0xF
constexpr std::uint32_t RUNTIME_VERSION_1_11_240 =
	(1u << 24) | (11u << 16) | (240u << 4) | 0u;

extern "C" DLLEXPORT F4SEPluginVersionData F4SEPlugin_Version = {
	F4SEPluginVersionData::kVersion,    // dataVersion
	1,                                  // pluginVersion
	"fo4flat_probe",
	"kebabkenraali",
	0,                                  // addressIndependence: no hardcoded addresses, no address db use
	F4SEPluginVersionData::kStructureIndependence_NoStructs,
	{ RUNTIME_VERSION_1_11_240, 0 },    // compatible with 1.11.240
	0                                   // seVersionRequired
};

static std::atomic<bool> g_pollRunning{ false };

// Logging setup must be callable from both F4SEPlugin_Query and F4SEPlugin_Load:
// F4SE 0.7.x does not call Query for plugins that provide F4SEPlugin_Version.
static void SetupLog()
{
	static std::once_flag once;
	std::call_once(once, [] {
		const char* home = std::getenv("USERPROFILE");
		const std::string path =
			std::string(home ? home : ".") + "\\Documents\\My Games\\Fallout4\\F4SE\\fo4flat_probe.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path, true);
		auto log = std::make_shared<spdlog::logger>("probe", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S] %v");
	});
}

static void PollGamepad()
{
	const auto start = std::chrono::steady_clock::now();
	XINPUT_STATE prev{};
	while (std::chrono::steady_clock::now() - start < std::chrono::seconds(30)) {
		XINPUT_STATE st{};
		if (XInputGetState(0, &st) == ERROR_SUCCESS && st.dwPacketNumber != prev.dwPacketNumber) {
			spdlog::info("pad: buttons=0x{:04X} LX={} LY={} RX={} RY={} LT={} RT={}",
				st.Gamepad.wButtons, st.Gamepad.sThumbLX, st.Gamepad.sThumbLY,
				st.Gamepad.sThumbRX, st.Gamepad.sThumbRY, st.Gamepad.bLeftTrigger,
				st.Gamepad.bRightTrigger);
			prev = st;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(33));
	}
	spdlog::info("pad probe finished");
	g_pollRunning = false;
}

// ---------------------------------------------------------------------------
// Engine-camera eye shift (test): after the engine updates the player camera,
// move the camera root sideways along the camera's right axis. The engine then
// builds every matrix (VP, shadows, glass, ...) from the shifted camera.
// Config: <game dir>\fo4stereo.ini, section [engine]: enabled, amp_units, period_sec.
// ---------------------------------------------------------------------------
using CamUpdateFn = void (*)(RE::TESCamera*);
static CamUpdateFn g_origCamUpdate = nullptr;
static bool g_engEnabled = true;
static float g_engAmp = 30.0f;
static float g_engPeriod = 4.0f;
static bool g_engStereo = false;
static float g_engIpd = 4.48f;  // 64 mm, 1 unit ~= 1.43 cm
static StereoShm* g_shm = nullptr;
static std::atomic<long> g_eye{ 0 };
static int g_compensate = 3;         // [engine] compensate: 0 none, 1 delta, 3 restore camera root
static int g_headRotMode = 1;        // [engine] head_rotation: 0 off, 1 cam*head, 2 head*cam
static bool g_headRotComp = false;   // [engine] head_rot_compensate
static bool g_headPos = true;        // [engine] head_position
static float g_unitsPerMeter = 70.0f;  // [engine] units_per_meter
static float g_aimIpdScale = 0.0f;    // final signed eye offset while aiming
static bool g_aimHeadPos = false;     // [engine] aim_headpos: apply head POSITION while aiming
static bool g_dialogueFreeze = true;// [engine] dialogue_freeze: conversation view frozen (head-locked screen)
static bool g_fpFovSync = true;      // [engine] fp_fov_sync: weapon/hands FOV = world FOV in hipfire
static float g_dialogueFov = 0.0f;    // [engine] dialogue_fov: world FOV in conversations (0 = hipfire world FOV)
static float g_dialogueBackM = 0.4f; // [engine] dialogue_back_m: pull the conversation camera back (metres)
static bool g_aimHeadRot = false;    // [engine] aim_headrot: apply head ROTATION while aiming (0 = sights follow the weapon)
static float g_convergeM = 0.0f;      // [engine] convergence_m (0 = parallel eyes) - DISABLED by default, causes issues with mono view
static float g_worldScale = 1.0f;     // [engine] world_scale: multiplies IPD + head translation (1 = neutral)
static bool g_useRuntimeIpd = true;   // [engine] use_runtime_ipd: take IPD from the HMD when available
static float g_effIpd = 4.48f;        // effective IPD in game units for the current frame (auto or configured x world scale)
static float g_curConvYaw = 0.0f;     // per-frame toe-in yaw for the current eye (rad)
static long g_convStateIdx = -1;      // camera state index that owns g_curConvYaw
static unsigned g_vrCamMask = (1u << 0);  // [vr] vr_cam_states - ONLY state 0 (1st person)
static RE::NiPoint3 g_lastWritten{};
static RE::NiPoint3 g_lastOffset{};
static bool g_haveWritten = false;
static bool g_firstStateUpdate = true;  // track first HookedStateUpdate for init
static std::uint64_t g_camUpdates = 0;
static LARGE_INTEGER g_qpf{}, g_qpc0{};

static std::string GameDirA()
{
	char path[MAX_PATH];
	GetModuleFileNameA(nullptr, path, MAX_PATH);
	char* slash = strrchr(path, '\\');
	if (slash) *(slash + 1) = '\0';
	return path;
}

static void LoadEngineConfig()
{
	const std::string ini = GameDirA() + "fo4stereo.ini";
	char buf[64]{};
	GetPrivateProfileStringA("engine", "enabled", "1", buf, sizeof(buf), ini.c_str());
	g_engEnabled = std::atoi(buf) != 0;
	GetPrivateProfileStringA("engine", "amp_units", "30", buf, sizeof(buf), ini.c_str());
	g_engAmp = static_cast<float>(std::atof(buf));
	GetPrivateProfileStringA("engine", "period_sec", "4", buf, sizeof(buf), ini.c_str());
	g_engPeriod = static_cast<float>(std::atof(buf));
	GetPrivateProfileStringA("engine", "mode", "stereo", buf, sizeof(buf), ini.c_str());
	g_engStereo = std::strcmp(buf, "stereo") == 0;
	GetPrivateProfileStringA("engine", "ipd_units", "4.48", buf, sizeof(buf), ini.c_str());
	g_engIpd = static_cast<float>(std::atof(buf));
	GetPrivateProfileStringA("engine", "compensate", "4", buf, sizeof(buf), ini.c_str());
	g_compensate = std::atoi(buf);
	GetPrivateProfileStringA("engine", "head_rotation", "1", buf, sizeof(buf), ini.c_str());
	g_headRotMode = std::atoi(buf);
	GetPrivateProfileStringA("engine", "head_rot_compensate", "0", buf, sizeof(buf), ini.c_str());
	g_headRotComp = std::atoi(buf) != 0;
	GetPrivateProfileStringA("engine", "head_position", "1", buf, sizeof(buf), ini.c_str());
	g_headPos = std::atoi(buf) != 0;
	GetPrivateProfileStringA("engine", "units_per_meter", "70", buf, sizeof(buf), ini.c_str());
	g_unitsPerMeter = static_cast<float>(std::atof(buf));
	GetPrivateProfileStringA("engine", "aim_ipd_scale", "0", buf, sizeof(buf), ini.c_str());
	g_aimIpdScale = static_cast<float>(std::atof(buf));
	GetPrivateProfileStringA("engine", "aim_headpos", "0", buf, sizeof(buf), ini.c_str());
	g_aimHeadPos = std::atoi(buf) != 0;
	GetPrivateProfileStringA("engine", "aim_headrot", "0", buf, sizeof(buf), ini.c_str());
	g_aimHeadRot = std::atoi(buf) != 0;
	GetPrivateProfileStringA("engine", "dialogue_back_m", "0.4", buf, sizeof(buf), ini.c_str());
	g_dialogueBackM = static_cast<float>(std::atof(buf));
	GetPrivateProfileStringA("engine", "fp_fov_sync", "1", buf, sizeof(buf), ini.c_str());
	g_fpFovSync = std::atoi(buf) != 0;
	GetPrivateProfileStringA("engine", "dialogue_freeze", "1", buf, sizeof(buf), ini.c_str());
	g_dialogueFreeze = std::atoi(buf) != 0;
	GetPrivateProfileStringA("engine", "dialogue_fov", "0", buf, sizeof(buf), ini.c_str());
	g_dialogueFov = static_cast<float>(std::atof(buf));
	spdlog::info("dialogue_back_m={:.2f} aim_headrot={} fp_fov_sync={} dialogue_fov={:.1f}", g_dialogueBackM,
		g_aimHeadRot, g_fpFovSync, g_dialogueFov);
	GetPrivateProfileStringA("engine", "convergence_m", "0", buf, sizeof(buf), ini.c_str());
	g_convergeM = static_cast<float>(std::atof(buf));
	GetPrivateProfileStringA("engine", "world_scale", "1.0", buf, sizeof(buf), ini.c_str());
	g_worldScale = std::clamp(static_cast<float>(std::atof(buf)), 0.1f, 5.0f);
	GetPrivateProfileStringA("engine", "use_runtime_ipd", "1", buf, sizeof(buf), ini.c_str());
	g_useRuntimeIpd = std::atoi(buf) != 0;
	GetPrivateProfileStringA("vr", "vr_cam_states", "0", buf, sizeof(buf), ini.c_str());
	g_vrCamMask = 0;
	for (char* p = buf; *p;) {
		const long v = std::strtol(p, &p, 10);
		if (v >= 0 && v < 32) g_vrCamMask |= 1u << v;
		while (*p == ',' || *p == ' ') ++p;
		if (*p && (*p < '0' || *p > '9')) break;
	}
	spdlog::info("vr camera states mask=0x{:X}", g_vrCamMask);
	spdlog::info("engine mode={} ipd_units={} compensate={} head_rotation={} head_rot_compensate={} head_position={} aim_headpos={} units_per_meter={} convergence_m={}",
		g_engStereo ? "stereo" : "wobble", g_engIpd, g_compensate, g_headRotMode, g_headRotComp, g_headPos, g_aimHeadPos, g_unitsPerMeter, g_convergeM);
	spdlog::info("engine stereo test: enabled={} amp_units={} period_sec={}", g_engEnabled, g_engAmp, g_engPeriod);
}

static void HookedCamUpdate(RE::TESCamera* a_self)
{
	g_origCamUpdate(a_self);
	++g_camUpdates;
	return;  // shifting now happens in the camera-state GetTranslation hook
	if (!g_engEnabled || g_engPeriod <= 0.0f) {
		return;
	}
	RE::NiNode* root = a_self->cameraRoot.get();
	if (!root) {
		return;
	}

	auto& lt = root->local.translate;
	// If the engine did not rebuild the camera translation this frame, remove our
	// previous offset first so it never accumulates.
	if (g_haveWritten && lt.x == g_lastWritten.x && lt.y == g_lastWritten.y && lt.z == g_lastWritten.z) {
		lt.x -= g_lastOffset.x;
		lt.y -= g_lastOffset.y;
		lt.z -= g_lastOffset.z;
	}

	// Right axis: row 0 of the world rotation (Bethesda NiMatrix3 rows = local axes;
	// FRIK reads forward from row 1).
	const auto& m = root->world.rotate;
	RE::NiPoint3 right{ m.entry[0].pt[0], m.entry[0].pt[1], m.entry[0].pt[2] };

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	const double t = static_cast<double>(now.QuadPart - g_qpc0.QuadPart) / static_cast<double>(g_qpf.QuadPart);
	const float shift = static_cast<float>(g_engAmp * std::sin(2.0 * 3.14159265358979323846 * t / g_engPeriod));

	RE::NiPoint3 off{ right.x * shift, right.y * shift, right.z * shift };
	lt.x += off.x;
	lt.y += off.y;
	lt.z += off.z;
	g_lastWritten = lt;
	g_lastOffset = off;
	g_haveWritten = true;

	// Propagate to the camera root and its direct children (the render NiCamera).
	root->UpdateWorldData(nullptr);
	for (std::uint32_t i = 0; i < root->children.capacity(); ++i) {
		if (auto* child = root->children[i].get()) {
			child->UpdateWorldData(nullptr);
		}
	}

	++g_camUpdates;
	if (g_camUpdates <= 5 || g_camUpdates % 600 == 0) {
		const auto& w = root->world.translate;
		spdlog::info("cam #{} t={:.2f} shift={:.2f} right=({:.3f} {:.3f} {:.3f}) world=({:.1f} {:.1f} {:.1f})",
			g_camUpdates, t, shift, right.x, right.y, right.z, w.x, w.y, w.z);
	}
}

// ---------------------------------------------------------------------------
// Version-independent PlayerCamera lookup via MSVC RTTI (the CommonLibF4 address-
// library ID for the singleton points to the wrong place on runtime 1.11.240).
// type name -> TypeDescriptor -> CompleteObjectLocator -> vtable -> object in .data
// ---------------------------------------------------------------------------
struct Section
{
	std::uintptr_t begin{ 0 };
	std::uintptr_t end{ 0 };
};

static bool GetSection(std::uintptr_t a_base, const char* a_name, Section& a_out)
{
	auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(a_base);
	auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(a_base + dos->e_lfanew);
	auto* sec = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
		if (std::strncmp(reinterpret_cast<const char*>(sec->Name), a_name, 8) == 0) {
			a_out.begin = a_base + sec->VirtualAddress;
			a_out.end = a_out.begin + sec->Misc.VirtualSize;
			return true;
		}
	}
	return false;
}

// SEH-guarded 8-byte read for pointers of unknown validity.
static bool SafeRead64(std::uintptr_t a_addr, std::uint64_t& a_out)
{
	__try {
		a_out = *reinterpret_cast<const volatile std::uint64_t*>(a_addr);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

static void* FindObjectByRtti(const char* a_typeName, std::uintptr_t& a_vtableOut)
{
	const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
	Section data{}, rdata{};
	if (!GetSection(base, ".data", data) || !GetSection(base, ".rdata", rdata)) {
		spdlog::error("rtti: sections not found");
		return nullptr;
	}
	const std::size_t nameLen = std::strlen(a_typeName);

	// 1. TypeDescriptor: { vfptr, spare, name[] } in .data
	std::uintptr_t td = 0;
	for (std::uintptr_t p = data.begin; p + nameLen < data.end; ++p) {
		if (std::memcmp(reinterpret_cast<const void*>(p), a_typeName, nameLen + 1) == 0) {
			td = p - 16;
			break;
		}
	}
	if (!td) {
		spdlog::error("rtti: type name {} not found", a_typeName);
		return nullptr;
	}
	const auto tdRva = static_cast<std::uint32_t>(td - base);

	// 2. CompleteObjectLocator (x64): sig=1, offset, cdOffset, typeRva, classRva, selfRva
	std::uintptr_t col = 0;
	for (std::uintptr_t p = rdata.begin; p + 24 <= rdata.end; p += 4) {
		const auto* u = reinterpret_cast<const std::uint32_t*>(p);
		if (u[0] == 1 && u[1] == 0 && u[3] == tdRva && u[5] == static_cast<std::uint32_t>(p - base)) {
			col = p;
			break;
		}
	}
	if (!col) {
		spdlog::error("rtti: complete object locator for {} not found", a_typeName);
		return nullptr;
	}

	// 3. vtable: the qword right after a pointer to the COL
	std::uintptr_t vtbl = 0;
	for (std::uintptr_t p = rdata.begin; p + 16 <= rdata.end; p += 8) {
		if (*reinterpret_cast<const std::uintptr_t*>(p) == col) {
			vtbl = p + 8;
			break;
		}
	}
	if (!vtbl) {
		spdlog::error("rtti: vtable for {} not found", a_typeName);
		return nullptr;
	}
	a_vtableOut = vtbl;

	// 4. object: a pointer in .data whose target starts with that vtable
	for (std::uintptr_t p = data.begin; p + 8 <= data.end; p += 8) {
		const std::uint64_t v = *reinterpret_cast<const std::uint64_t*>(p);
		if (v < 0x10000 || v > 0x7FFFFFFFFFFF || (v & 7) != 0) {
			continue;
		}
		std::uint64_t first = 0;
		if (SafeRead64(static_cast<std::uintptr_t>(v), first) && first == vtbl) {
			spdlog::info("rtti: {} td=+{:X} col=+{:X} vtbl=+{:X} object={:X} (global at +{:X})",
				a_typeName, td - base, col - base, vtbl - base, v, p - base);
			return reinterpret_cast<void*>(v);
		}
	}
	// The object may also live inside the image (static storage).
	for (std::uintptr_t p = data.begin; p + 8 <= data.end; p += 8) {
		if (*reinterpret_cast<const std::uint64_t*>(p) == vtbl) {
			spdlog::info("rtti: {} static object at +{:X}", a_typeName, p - base);
			return reinterpret_cast<void*>(p);
		}
	}
	spdlog::error("rtti: no live {} object found (vtbl=+{:X})", a_typeName, vtbl - base);
	return nullptr;
}

// ---------------------------------------------------------------------------
// Camera-state hooks. TESCamera::Update (slot 3) was never called in the test,
// so also hook the states' Update (0x0B) and GetTranslation (0x0D). Shifting the
// translation a state reports lets the engine place the camera itself.
// ---------------------------------------------------------------------------
using StateUpdateFn = void (*)(RE::TESCameraState*, void*);
using StateGetTransFn = void (*)(const RE::TESCameraState*, RE::NiPoint3&);
using StateGetRotFn = void (*)(const RE::TESCameraState*, RE::NiQuaternion&);
struct StateVt
{
	void** vtbl{ nullptr };
	StateUpdateFn update{ nullptr };
	StateGetTransFn getTrans{ nullptr };
	StateGetRotFn getRot{ nullptr };
	int index{ -1 };
};
static StateVt g_stateVts[16];
static int g_nStateVt = 0;
static RE::PlayerCamera* g_cam = nullptr;
static std::atomic<std::uint64_t> g_stateUpdates{ 0 }, g_getTrans{ 0 };

static StateVt* FindStateVt(const void* a_self)
{
	void** v = *reinterpret_cast<void** const*>(a_self);
	for (int i = 0; i < g_nStateVt; ++i) {
		if (g_stateVts[i].vtbl == v) return &g_stateVts[i];
	}
	return nullptr;
}

static float CurrentShift()
{
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	const double t = static_cast<double>(now.QuadPart - g_qpc0.QuadPart) / static_cast<double>(g_qpf.QuadPart);
	return static_cast<float>(g_engAmp * std::sin(2.0 * 3.14159265358979323846 * t / g_engPeriod));
}

// --- per-frame state (set once per camera-state Update) -----------------------
// The first-person state's GetTranslation returns a position that already contains
// the offset applied in the previous frame (measured: out ~= shift_n + shift_{n-1}).
// So every query gets  orig - prevApplied + curApplied  (config [engine] compensate=1).
static RE::NiPoint3 g_prevApplied{};
static RE::NiPoint3 g_curApplied{};
static float g_prevHeadQ[4] = { 1.0f, 0.0f, 0.0f, 0.0f };  // w x y z, game space
static float g_curHeadQ[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
static float g_lastHmdPos[3] = { 0.0f, 0.0f, 0.0f };
static bool g_lastPoseValid = false;
static std::atomic<std::uint64_t> g_getRot{ 0 };
static RE::NiPoint3 g_lastRootLocal{};
static RE::NiAVObject* g_camChild = nullptr;
static long g_curStateIdx = 0;
static RE::NiPoint3 g_childLocal{};

static void QuatMul(const float a[4], const float b[4], float out[4])
{
	out[0] = a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3];
	out[1] = a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2];
	out[2] = a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
	out[3] = a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
}

static void QuatConj(const float a[4], float out[4])
{
	out[0] = a[0];
	out[1] = -a[1];
	out[2] = -a[2];
	out[3] = -a[3];
}

static void QuatNormalize(float q[4])
{
	const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (l > 1e-6f) {
		for (int i = 0; i < 4; ++i) q[i] /= l;
	} else {
		q[0] = 1.0f; q[1] = q[2] = q[3] = 0.0f;
	}
}

// Convert OpenVR seated HMD quaternion to a Fallout 4 camera-space quaternion.
//
// This is a proper change of basis R_game = T * R_vr * T^-1 with
//   T = [ [1, 0, 0], [0, 0, -1], [0, 1, 0] ]   (det +1, a true rotation)
// For an orthonormal, det +1 T the quaternion's vector part transforms exactly
// like the axis vectors:
//   VR:   x = right, y = up,    z = back
//   Game: x = right, y = forward, z = up
//   game.x = vr.x, game.y = -vr.z, game.z = vr.y
// so gameQ = (w, vr.x, -vr.z, vr.y). Using this instead of a
// matrix round-trip avoids putting the head YAW into the game's forward axis
// (which rolled the whole view and caused instant nausea).
static void VrQuatToGameQuat(const float vrQ[4], float gameQ[4])
{
	gameQ[0] = vrQ[0];        // w
	gameQ[1] = vrQ[1];        // x  = vr pitch (right axis)
	gameQ[2] = -vrQ[3];       // y  = -vr roll (forward axis)
	gameQ[3] = vrQ[2];        // z  = vr yaw  (up axis)
	QuatNormalize(gameQ);
}

static void HookedStateUpdate(RE::TESCameraState* a_self, void* a_next)
{
	// New frame of camera updates. The proxy owns the eye: it is the parity of the
	// number of frames presented so far (the frame being prepared is the next one).
	long eye = g_eye.load();
	if (g_engStereo) {
		// Strict per-update alternation, in lockstep with the camera shift
		// below (both use this `eye`). The old scheme derived the eye from the
		// present counter: a present can land before OR after the next camera
		// update (render thread vs game thread race), and whenever it did, an
		// eye repeated - the same eye's view rendered twice while the other
		// eye's texture kept a stale frame - a constant left-right wobble.
		// Self-alternation cannot drift; the proxy follows the recorded eye.
		eye ^= 1;
		g_eye.store(eye);
		if (g_shm) {
			InterlockedExchange(&g_shm->eye, eye);
		}
	}

	// First HookedStateUpdate: initialize eye to 0 (left) and set initial applied offset
	// so GetTranslation hooks have valid data before the first alternation.
	// Note: firstPerson is not yet computed here, so we check g_curStateIdx directly.
	if (g_firstStateUpdate) {
		g_firstStateUpdate = false;
		eye = 0;  // start with left eye
		g_eye.store(eye);
		if (g_shm) InterlockedExchange(&g_shm->eye, eye);
		// Pre-compute initial applied offset for eye 0 (left = -IPD/2)
		// We need to know if this is first-person state. Check g_curStateIdx from previous frame
		// or compute it here. For the very first call, g_curStateIdx might be stale,
		// so we also check the current state's vtable.
		const StateVt* svt = FindStateVt(a_self);
		const int curStateIdx = svt ? svt->index : -1;
		const bool isFirstPerson = curStateIdx >= 0 && curStateIdx < 32 && (g_vrCamMask & (1u << curStateIdx));
		if (g_engEnabled && isFirstPerson) {
			const float shift = g_engStereo ? -0.5f * g_engIpd : 0.0f;
			RE::NiPoint3 right{ 1.0f, 0.0f, 0.0f };
			if (g_cam) {
				if (RE::NiNode* root = g_cam->cameraRoot.get()) {
					const auto& m = root->world.rotate;
					right = { m.entry[0].pt[0], m.entry[0].pt[1], m.entry[0].pt[2] };
				}
			}
			g_curApplied.x = right.x * shift;
			g_curApplied.y = right.y * shift;
			g_curApplied.z = right.z * shift;
			g_prevApplied = g_curApplied;
		}
	}
	if (g_shm) {
		InterlockedIncrement(&g_shm->seq);
	}
	// Only the first-person state (index 0) gets stereo/head tracking; VATS, dialogue,
	// 3rd person etc. stay untouched (the proxy shows them on the 2D screen).
	{
		const StateVt* svt = FindStateVt(a_self);
		g_curStateIdx = svt ? svt->index : -1;
		if (g_shm) InterlockedExchange(&g_shm->camState, g_curStateIdx);
		{
			// Log every camera-state change (0 first person, 2 VATS, 5 PCTransition, 6 Tween,
			// 7/8 third person, 9 furniture, 12 dialogue) so new states can be added to vr_cam_states.
			static int s_loggedState = -2;
			if (g_curStateIdx != s_loggedState) {
				spdlog::info("camera state {} -> {} (inVR={})", s_loggedState, g_curStateIdx,
					(g_curStateIdx >= 0 && g_curStateIdx < 32 && (g_vrCamMask & (1u << g_curStateIdx))) ? 1 : 0);
				s_loggedState = g_curStateIdx;
			}
		}
	}
	// stereo/head tracking only for the camera states shown in VR ([vr] vr_cam_states)
	const bool firstPerson = g_curStateIdx >= 0 && g_curStateIdx < 32 && (g_vrCamMask & (1u << g_curStateIdx));
	// Aim detection, engine side. While aiming, the weapon's AimModel narrows the
	// camera FOV: PlayerCamera animates fovAdjustCurrent toward fovAdjustTarget.
	// Reading it here on the game thread removes the one-frame round-trip through
	// the proxy (shm->zoom is written at present time, this camera update already
	// prepares the next frame) and the FOV-ratio heuristics. It also detects
	// scope zoom correctly: a heavy scope zoom crosses the proxy's pipboy_ratio
	// threshold, which would otherwise flip the view to the 2D screen.
	float fovAdj = 1.0f;
	bool pipMode = false;
	if (g_cam) {
		fovAdj = g_cam->fovAdjustCurrent;
		pipMode = g_cam->pipboyMode != 0;
		// Calibration: log the engine FOV fields once per second so an ADS vs
		// hipfire play session reveals which field actually changes on ADS.
		// worldFOV / firstPersonFOV are candidates; fovAdjust* may read 0.
		static std::uint64_t s_fovLog = 0;
		if (++s_fovLog % 60 == 0) {
			spdlog::info("fov world={:.3f} firstPerson={:.3f} adjustCur={:.3f} adjustTgt={:.3f} perSec={:.3f} pipboy={}",
				g_cam->worldFOV, g_cam->firstPersonFOV, fovAdj, g_cam->fovAdjustTarget,
				g_cam->fovAdjustPerSec, pipMode ? 1 : 0);
			// Field scan: dump the float window 0x150..0x1B0 of PlayerCamera. The
			// CommonLibF4 fovAdjustCurrent offset (0x170) reads 0 in runtime
			// 1.11.240; comparing hipfire vs ADS dumps locates the field that
			// animates ~1.0 -> ~0.75 on ADS, i.e. the real offset in this runtime.
			const auto* base = reinterpret_cast<const std::uint8_t*>(g_cam);
			std::string row;
			char cell[32];
			for (int i = 0; i < 24; ++i) {
				float f;
				std::memcpy(&f, base + 0x150 + i * 4, sizeof(f));
				std::snprintf(cell, sizeof(cell), "%s0x%03X=%.3f", i ? " " : "", 0x150 + i * 4, f);
				row += cell;
			}
			spdlog::info("fovscan {}", row);
		}
	}
	// Fallback: if fovAdjustCurrent is 0 (uninitialized), assume normal FOV
	if (fovAdj <= 0.0f) fovAdj = 1.0f;
	const bool aimFov = fovAdj < 0.999f && !pipMode;
	const bool aiming = aimFov || (g_shm && g_shm->zoom == 1);
	// Tell the proxy (overrides its FOV-ratio zoom for scope zoom etc.). The flag
	// only applies to the first-person state; other states keep the proxy's rules.
	if (g_shm) {
		InterlockedExchange(&g_shm->aim, (firstPerson && aimFov) ? 1 : 0);
	}

	// Reset head rotation state when transitioning from a non-VR camera state
	// (Pip-Boy, VATS, dialogue) back to a VR state. Without this, stale
	// g_curHeadQ/g_prevHeadQ from before the menu froze the view on return.
	{
		static int s_prevStateIdx = -1;
		const bool wasVR = s_prevStateIdx >= 0 && s_prevStateIdx < 32 && (g_vrCamMask & (1u << s_prevStateIdx));
		if (firstPerson && !wasVR) {
			std::memcpy(g_curHeadQ, "\x00\x00\x80\x3F\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", sizeof(g_curHeadQ));
			std::memcpy(g_prevHeadQ, g_curHeadQ, sizeof(g_curHeadQ));
			spdlog::info("head rotation reset (state {} -> {})", s_prevStateIdx, g_curStateIdx);
		}
		s_prevStateIdx = g_curStateIdx;
	}

	// Camera axes of the last rendered frame (rows of the world rotation).
	RE::NiPoint3 right{ 1.0f, 0.0f, 0.0f }, fwd{ 0.0f, 1.0f, 0.0f }, up{ 0.0f, 0.0f, 1.0f };
	if (g_cam) {
		if (RE::NiNode* root = g_cam->cameraRoot.get()) {
			const auto& m = root->world.rotate;
			right = { m.entry[0].pt[0], m.entry[0].pt[1], m.entry[0].pt[2] };
			fwd = { m.entry[1].pt[0], m.entry[1].pt[1], m.entry[1].pt[2] };
			up = { m.entry[2].pt[0], m.entry[2].pt[1], m.entry[2].pt[2] };
		}
	}

	// compensate=4: put the eye/head offset on the camera root's child (the render
	// camera) instead of the state's translation. The state never reads the child,
	// so there is no feedback (modes 1 and 3 were cancelled by the engine).
	if (g_compensate == 4 && g_cam && !g_camChild) {
		if (RE::NiNode* root = g_cam->cameraRoot.get()) {
			for (std::uint32_t i = 0; i < root->children.capacity(); ++i) {
				RE::NiAVObject* c = root->children[i].get();
				if (!c) continue;
				const std::string_view nm = c->GetName();
				spdlog::info("cameraRoot child {} = {} '{}' local=({:.2f} {:.2f} {:.2f})", i,
					static_cast<const void*>(c), nm, c->local.translate.x, c->local.translate.y, c->local.translate.z);
				if (!g_camChild || nm.find("Camera") != std::string_view::npos) {
					g_camChild = c;
				}
			}
			spdlog::info("eye offset target = {}", static_cast<const void*>(g_camChild));
		}
	}

	// compensate=3: the engine builds the next camera position from the previous one
	// (feedback, measured). Put the camera root back to where the engine had it before
	// our offset, so the state's GetTranslation starts from a clean value every frame.
	g_lastRootLocal = RE::NiPoint3{};
	if (g_compensate == 3 && g_cam) {
		if (RE::NiNode* root = g_cam->cameraRoot.get()) {
			root->local.translate.x -= g_curApplied.x;
			root->local.translate.y -= g_curApplied.y;
			root->local.translate.z -= g_curApplied.z;
			root->world.translate.x -= g_curApplied.x;
			root->world.translate.y -= g_curApplied.y;
			root->world.translate.z -= g_curApplied.z;
			g_lastRootLocal = root->local.translate;
		}
	}

	g_prevApplied = g_curApplied;
	std::memcpy(g_prevHeadQ, g_curHeadQ, sizeof(g_curHeadQ));

	// Effective IPD for this frame: the HMD's runtime IPD (auto, adapts to the
	// wearer) or the configured ipd_units, both scaled by world_scale. Game
	// units: metres * units_per_meter. world_scale widens both the eye
	// separation and the head translation, so one slider changes perceived
	// world size without breaking stereo calibration.
	{
		float baseIpd = g_engIpd;
		if (g_useRuntimeIpd && g_shm && g_shm->runtimeIpdValid == 1 && g_shm->runtimeIpdM > 0.001f) {
			baseIpd = g_shm->runtimeIpdM * g_unitsPerMeter;
		}
		g_effIpd = baseIpd * g_worldScale;
	}

	RE::NiPoint3 applied{ 0.0f, 0.0f, 0.0f };
	float shift = 0.0f;
	// Aim handling is first-person only (state 0). The 3rd-person state (7; it
	// shares its vtable with 8) shows the character holding the weapon at
	// roughly 1-2 m, where plain stereo is correct; the sideways mono offset
	// made the aiming view look displaced there.
	const bool aimFp = aiming && g_curStateIdx == 0;
	// Conversation camera frozen like ADS: the proxy shows it as a head-locked screen, so every
	// frame must be identical (no eye alternation, no head rotation/position applied).
	const bool dlgFreeze = g_dialogueFreeze && g_curStateIdx == 12 && firstPerson;
	// ADS mode 2 (proxy hotkey Insert): aim renders as normal alternating stereo, like hipfire.
	const bool adsStereo = aimFp && g_shm && g_shm->adsMode == 2;
	if (g_engEnabled && firstPerson) {
		if (dlgFreeze) {
			shift = 0.0f;
		} else if (aimFp && !adsStereo) {
			// Signed mono-view offset while aiming: the value is the eye position as
			// a fraction of half the IPD (-1 = left eye, 0 = centre, +1 = right eye).
			// Both frames use the SAME offset; the offset only changes framing and
			// does not enable mono rendering.
			shift = g_aimIpdScale * 0.5f * g_effIpd;
		} else {
			shift = g_engStereo ? (eye == 0 ? -0.5f : 0.5f) * g_effIpd : CurrentShift();
		}
		applied.x += right.x * shift;
		applied.y += right.y * shift;
		applied.z += right.z * shift;
	}

	// Convergence (toe-in): yaw each eye toward the centreline so near objects
	// (weapon at arm's length, people closer than ~2 m) fuse without cross-eyed
	// strain. Only for the alternating stereo frames: with the mono aim view
	// both frames would rotate the SAME way, which just turns the whole view
	// (and the crosshair with it) away from the engine's bullet path.
	g_curConvYaw = 0.0f;
	g_convStateIdx = -1;
	if (g_engEnabled && firstPerson && (!aimFp || adsStereo) && !dlgFreeze && g_convergeM > 0.0f && g_effIpd > 0.0f) {
		const float half = 0.5f * g_effIpd;
		const float dist = g_convergeM * g_unitsPerMeter;
		if (half > 0.0f && dist > 0.0f) {
			const float theta = std::atan2(half, dist);
			// Sign from the shift direction: the eye at -IPD/2 (left) turns
			// right (+yaw), the eye at +IPD/2 (right) turns left (-yaw).
			// Local +Z yaw = left turn, so left eye needs +theta, right needs -theta.
			g_curConvYaw = (shift <= 0.0f) ? theta : -theta;
			g_convStateIdx = g_curStateIdx;
		}
	}

	// Instrument: prove whether mono (aim) and the toe-in yaw are actually active
	// when double vision is reported while aiming.
	{
		static bool s_aimPrev = false;
		if (aiming != s_aimPrev) {
			spdlog::info("aim {} (state={} eye={} aimFp={} ipdScale={:.2f} shift={:.2f} shmZoom={} fovAdj={:.3f} pipboy={})",
				aiming ? 1 : 0, g_curStateIdx, static_cast<int>(eye), aimFp ? 1 : 0, g_aimIpdScale, shift,
				g_shm ? static_cast<int>(g_shm->zoom) : -1, fovAdj, pipMode ? 1 : 0);
			s_aimPrev = aiming;
		}
		static float s_yawPrev = 1e9f;
		// Compare magnitudes: the yaw sign alternates with the eye every frame,
		// so comparing signed values would log on every single camera update.
		if (std::fabs(std::fabs(g_curConvYaw) - s_yawPrev) > 1e-5f) {
			spdlog::info("conv yaw={:.4f} rad ({:.2f} deg) eye={}", g_curConvYaw,
				g_curConvYaw * 57.2957795f, static_cast<int>(eye));
			s_yawPrev = g_curConvYaw;
		}
	}

	// Head pose from the proxy (OpenVR seated space, metres; x right, y up, -z forward).
	float pos[3]{}, q[4]{ 1.0f, 0.0f, 0.0f, 0.0f };
	g_lastPoseValid = firstPerson && g_shm && ReadHmdPose(g_shm, pos, q);
	float headQ[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
	if (g_lastPoseValid) {
		std::memcpy(g_lastHmdPos, pos, sizeof(pos));

		// Always convert VR quaternion to game space (needed for head position rotation)
		VrQuatToGameQuat(q, headQ);

		// Head position in head-local space: rotate by head orientation first,
		// then apply in world space. This ensures leaning/tilting moves the view correctly.
		// In ADS (aimFp) the positional offset is skipped by default (aim_headpos=0):
		// the weapon model stays anchored to the player body, so a positional camera
		// offset detaches the sights from the crosshair (reported ADS aim offset).
		// Head ROTATION still follows the HMD, so aiming works; hipfire keeps 6-DOF.
		if (g_headPos && !(aimFp && !g_aimHeadPos) && !dlgFreeze) {
			// world_scale multiplies the head translation too, so it moves with the
			// eye separation (one slider changes the perceived world size uniformly).
			const float s = g_unitsPerMeter * g_worldScale;

			// Rotate head position by head quaternion (headQ is now in game space)
			// headQ = (w, x, y, z) in game coords: x right, y forward, z up
			const float hx = headQ[1], hy = headQ[2], hz = headQ[3], hw = headQ[0];
			// Rotation matrix from headQ (row-major, game axes)
			float hr[3][3];
			const float hxx = hx * hx, hyy = hy * hy, hzz = hz * hz;
			const float hxy = hx * hy, hxz = hx * hz, hyz = hy * hz;
			const float hwx = hw * hx, hwy = hw * hy, hwz = hw * hz;

			hr[0][0] = 1.0f - 2.0f * (hyy + hzz);
			hr[0][1] = 2.0f * (hxy + hwz);
			hr[0][2] = 2.0f * (hxz - hwy);

			hr[1][0] = 2.0f * (hxy - hwz);
			hr[1][1] = 1.0f - 2.0f * (hxx + hzz);
			hr[1][2] = 2.0f * (hyz + hwx);

			hr[2][0] = 2.0f * (hxz + hwy);
			hr[2][1] = 2.0f * (hyz - hwx);
			hr[2][2] = 1.0f - 2.0f * (hxx + hyy);

			// VR pos: x right, y up, z back (-z forward) -> game: x right, y forward, z up
			// So VR (x, y, -z) in head-local -> rotate by headQ -> world
			const float vrX = pos[0];
			const float vrY = pos[1];
			const float vrZ = -pos[2];  // VR z back -> game z forward

			// Rotate by head orientation
			const float wx = hr[0][0] * vrX + hr[0][1] * vrY + hr[0][2] * vrZ;
			const float wy = hr[1][0] * vrX + hr[1][1] * vrY + hr[1][2] * vrZ;
			const float wz = hr[2][0] * vrX + hr[2][1] * vrY + hr[2][2] * vrZ;

			applied.x += wx * s;
			applied.y += wy * s;
			applied.z += wz * s;
		}

		// ADS: the weapon, sights and bullet path follow the game's own aim direction
		// (mouse/stick), not the HMD. With head rotation applied to the render camera the
		// crosshair/sights drift away from the barrel ("aim follows the gaze"). While aiming
		// down sights, blend the head rotation to identity so the view equals the game's
		// aim view; blend back on release. [engine] aim_headrot=1 restores head rotation.
		{
			static float s_aimBlend = 0.0f;
			const float target = ((aimFp && !g_aimHeadRot) || dlgFreeze) ? 1.0f : 0.0f;
			const float step = 0.15f;  // ~7 camera updates (~0.1 s) either way
			s_aimBlend += (target > s_aimBlend) ? std::min(step, target - s_aimBlend)
			                                    : -std::min(step, s_aimBlend - target);
			if (s_aimBlend > 0.0f) {
				float h[4] = { headQ[0], headQ[1], headQ[2], headQ[3] };
				if (h[0] < 0.0f) { h[0] = -h[0]; h[1] = -h[1]; h[2] = -h[2]; h[3] = -h[3]; }
				const float b = s_aimBlend;
				headQ[0] = h[0] * (1.0f - b) + b;
				headQ[1] = h[1] * (1.0f - b);
				headQ[2] = h[2] * (1.0f - b);
				headQ[3] = h[3] * (1.0f - b);
				QuatNormalize(headQ);
			}
		}

		// If head rotation is disabled, use identity for camera rotation
		if (g_headRotMode == 0) {
			headQ[0] = 1.0f; headQ[1] = headQ[2] = headQ[3] = 0.0f;
		}
	}
	// Conversation camera (state 12): pull it back along its view axis so the NPC and the
	// scene are not too close for VR. No collision test is possible here, so keep it modest.
	if (g_engEnabled && firstPerson && g_curStateIdx == 12 && g_dialogueBackM != 0.0f) {
		const float d = g_dialogueBackM * g_unitsPerMeter * g_worldScale;
		applied.x -= fwd.x * d;
		applied.y -= fwd.y * d;
		applied.z -= fwd.z * d;
	}
	g_curApplied = applied;
	if (g_compensate == 4 && g_camChild) {
		// world offset -> camera-root local axes (rows of the world rotation are the local axes)
		g_childLocal = RE::NiPoint3{ right.x * applied.x + right.y * applied.y + right.z * applied.z,
			fwd.x * applied.x + fwd.y * applied.y + fwd.z * applied.z,
			up.x * applied.x + up.y * applied.y + up.z * applied.z };
		g_camChild->local.translate = g_childLocal;
		g_camChild->UpdateWorldData(nullptr);
	}
	std::memcpy(g_curHeadQ, headQ, sizeof(headQ));

	if (auto* vt = FindStateVt(a_self)) {
		vt->update(a_self, a_next);
	}
	// FOV overrides, applied AFTER the state's own update so the renderer sees them.
	// 1) Weapon/hands: the engine keeps the first-person model FOV at 80 although
	//    fDefault1stPersonFOV=137 (log: firstPerson=80), so the weapon is drawn ~3x too big next
	//    to the 137-degree world. Match it to the world FOV in hipfire (fp_fov_sync=1).
	// 2) Conversation camera: the dialogue state narrows the world FOV to ~30 degrees (a telephoto
	//    shot). The proxy cannot show that narrower image at its true angular size, so the NPC is
	//    magnified ~9x. Restore the normal FOV (dialogue_fov, 0 = the hipfire world FOV).
	if (g_cam) {
		static float s_baseWorld = 0.0f;
		if (g_curStateIdx == 0 && !aiming && g_cam->worldFOV > 60.0f) {
			s_baseWorld = g_cam->worldFOV;
			if (g_fpFovSync) g_cam->firstPersonFOV = g_cam->worldFOV;
		}
		if (g_curStateIdx == 12 && firstPerson) {
			const float target = g_dialogueFov > 0.0f ? g_dialogueFov : s_baseWorld;
			if (target > 0.0f) g_cam->worldFOV = target;
		}
	}
	++g_stateUpdates;
}

static void HookedGetTranslation(const RE::TESCameraState* a_self, RE::NiPoint3& a_out)
{
	auto* vt = FindStateVt(a_self);
	if (vt) {
		vt->getTrans(a_self, a_out);
	}
	const auto n = ++g_getTrans;
	const RE::NiPoint3 orig = a_out;
	if (g_compensate == 4) {
		// offset lives on the camera child; leave the state's translation untouched
	} else if (g_compensate == 1) {
		a_out.x += g_curApplied.x - g_prevApplied.x;
		a_out.y += g_curApplied.y - g_prevApplied.y;
		a_out.z += g_curApplied.z - g_prevApplied.z;
	} else {
		a_out.x += g_curApplied.x;
		a_out.y += g_curApplied.y;
		a_out.z += g_curApplied.z;
	}
	// n in [4000, 4048): log every call to verify the per-eye offsets frame by frame
	if (n <= 8 || n % 600 == 0 || (n >= 400 && n < 448)) {
		spdlog::info("getTrans #{} state#{} eye={} orig=({:.2f} {:.2f} {:.2f}) out=({:.2f} {:.2f} {:.2f}) cur=({:.2f} {:.2f} {:.2f}) pose={} hmd=({:.2f} {:.2f} {:.2f}) rootLocal=({:.2f} {:.2f} {:.2f}) child=({:.2f} {:.2f} {:.2f}) updates={} presents={}",
			n, vt ? vt->index : -1, g_eye.load(), orig.x, orig.y, orig.z, a_out.x, a_out.y, a_out.z,
			g_curApplied.x, g_curApplied.y, g_curApplied.z, g_lastPoseValid, g_lastHmdPos[0], g_lastHmdPos[1],
			g_lastHmdPos[2], g_lastRootLocal.x, g_lastRootLocal.y, g_lastRootLocal.z, g_childLocal.x, g_childLocal.y, g_childLocal.z, g_stateUpdates.load(), g_shm ? g_shm->presents : -1);
	}
}

static void HookedGetRotation(const RE::TESCameraState* a_self, RE::NiQuaternion& a_rot)
{
	auto* vt = FindStateVt(a_self);
	if (vt && vt->getRot) {
		vt->getRot(a_self, a_rot);
	}
	const auto n = ++g_getRot;
	if (g_headRotMode == 0) {
		return;
	}
	float cam[4] = { a_rot.w, a_rot.x, a_rot.y, a_rot.z };
	const float orig[4] = { cam[0], cam[1], cam[2], cam[3] };
	float head[4];
	std::memcpy(head, g_curHeadQ, sizeof(head));
	if (g_headRotComp) {
		// remove last frame's head rotation in case the state feeds its output back
		float inv[4], tmp[4];
		QuatConj(g_prevHeadQ, inv);
		if (g_headRotMode == 1) {
			QuatMul(inv, head, tmp);
		} else {
			QuatMul(head, inv, tmp);
		}
		std::memcpy(head, tmp, sizeof(head));
	}
	float out[4];
	if (g_headRotMode == 1) {
		QuatMul(cam, head, out);
	} else {
		QuatMul(head, cam, out);
	}
	// Per-eye convergence: yaw about the camera's own up axis (local post-multiply).
	// Set in HookedStateUpdate for the frame's eye; 0 = parallel eyes.
	// Only apply for the state that computed it (first-person); other states get stale data.
	if (g_curConvYaw != 0.0f && vt && vt->index == g_convStateIdx) {
		const float h = g_curConvYaw * 0.5f;
		const float yaw[4] = { std::cos(h), 0.0f, 0.0f, std::sin(h) };
		float tmp[4];
		QuatMul(out, yaw, tmp);
		std::memcpy(out, tmp, sizeof(out));
	}
	QuatNormalize(out);
	a_rot.w = out[0];
	a_rot.x = out[1];
	a_rot.y = out[2];
	a_rot.z = out[3];
	if (n <= 5 || n % 600 == 0) {
		spdlog::info("getRot #{} orig=({:.3f} {:.3f} {:.3f} {:.3f}) head=({:.3f} {:.3f} {:.3f} {:.3f}) out=({:.3f} {:.3f} {:.3f} {:.3f})",
			n, orig[0], orig[1], orig[2], orig[3], g_curHeadQ[0], g_curHeadQ[1], g_curHeadQ[2], g_curHeadQ[3],
			out[0], out[1], out[2], out[3]);
	}
}

static bool PatchSlot(void** a_vtbl, int a_slot, void* a_fn)
{
	DWORD oldProt = 0;
	if (!VirtualProtect(&a_vtbl[a_slot], sizeof(void*), PAGE_READWRITE, &oldProt)) {
		return false;
	}
	a_vtbl[a_slot] = a_fn;
	VirtualProtect(&a_vtbl[a_slot], sizeof(void*), oldProt, &oldProt);
	return true;
}

static void HookCameraStates(RE::PlayerCamera* a_cam)
{
	const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
	Section rdata{};
	if (!GetSection(base, ".rdata", rdata)) {
		return;
	}
	const auto camAddr = reinterpret_cast<std::uintptr_t>(a_cam);
	for (int i = 0; i < static_cast<int>(RE::CameraStates::kTotal); ++i) {
		std::uint64_t statePtr = 0, vtblVal = 0;
		if (!SafeRead64(camAddr + 0x0E0 + i * 8, statePtr) || statePtr == 0) continue;
		if (!SafeRead64(static_cast<std::uintptr_t>(statePtr), vtblVal)) continue;
		if (vtblVal < rdata.begin || vtblVal >= rdata.end) {
			spdlog::warn("state {} vtable +{:X} not in .rdata, skipped", i, vtblVal - base);
			continue;
		}
		void** vtbl = reinterpret_cast<void**>(vtblVal);
		if (FindStateVt(reinterpret_cast<const void*>(statePtr)) || g_nStateVt >= 16) continue;
		StateVt& vt = g_stateVts[g_nStateVt];
		vt.vtbl = vtbl;
		vt.index = i;
		vt.update = reinterpret_cast<StateUpdateFn>(vtbl[0x0B]);
		vt.getTrans = reinterpret_cast<StateGetTransFn>(vtbl[0x0D]);
		vt.getRot = reinterpret_cast<StateGetRotFn>(vtbl[0x0C]);
		++g_nStateVt;
		const bool ok = PatchSlot(vtbl, 0x0B, reinterpret_cast<void*>(&HookedStateUpdate))
		                && PatchSlot(vtbl, 0x0D, reinterpret_cast<void*>(&HookedGetTranslation))
		                && PatchSlot(vtbl, 0x0C, reinterpret_cast<void*>(&HookedGetRotation));
		spdlog::info("state {} obj={:X} vtbl=+{:X} hooked={}", i, statePtr, vtblVal - base, ok);
	}
}

static void InstallCameraHook()
{
	if (g_origCamUpdate) {
		return;
	}
	std::uintptr_t rttiVtbl = 0;
	auto* cam = static_cast<RE::PlayerCamera*>(FindObjectByRtti(".?AVPlayerCamera@@", rttiVtbl));
	spdlog::info("PlayerCamera (rtti) = {}", static_cast<const void*>(cam));
	if (!cam) {
		return;
	}
	if (auto* root = cam->cameraRoot.get()) {
		const auto& w = root->world.translate;
		spdlog::info("cameraRoot = {} world=({:.1f} {:.1f} {:.1f})", static_cast<const void*>(root), w.x, w.y, w.z);
	} else {
		spdlog::info("cameraRoot is null");
	}
	{
		auto* root = cam->cameraRoot.get();
		const bool sane = root && std::isfinite(root->world.translate.x) && std::fabs(root->world.translate.x) < 1.0e7f
		                  && std::fabs(root->world.translate.y) < 1.0e7f && std::fabs(root->world.translate.z) < 1.0e7f;
		if (!sane) {
			spdlog::error("cameraRoot transform not plausible, not hooking");
			return;
		}
	}
	LoadEngineConfig();
	QueryPerformanceFrequency(&g_qpf);
	QueryPerformanceCounter(&g_qpc0);

	void** vtbl = *reinterpret_cast<void***>(cam);
	if (reinterpret_cast<std::uintptr_t>(vtbl) != rttiVtbl) {
		spdlog::error("vtable mismatch, not hooking");
		return;
	}
	DWORD oldProt = 0;
	if (!VirtualProtect(&vtbl[3], sizeof(void*), PAGE_READWRITE, &oldProt)) {
		spdlog::error("VirtualProtect failed: {}", GetLastError());
		return;
	}
	g_origCamUpdate = reinterpret_cast<CamUpdateFn>(vtbl[3]);
	vtbl[3] = reinterpret_cast<void*>(&HookedCamUpdate);
	VirtualProtect(&vtbl[3], sizeof(void*), oldProt, &oldProt);
	spdlog::info("hooked PlayerCamera::Update (vtable {} slot 3)", static_cast<const void*>(vtbl));
	g_cam = cam;
	g_shm = OpenStereoShm();
	spdlog::info("stereo shm {}", g_shm ? "open" : "FAILED");
	HookCameraStates(cam);
}

static void OnMessage(F4SE::MessagingInterface::Message* a_msg)
{
	if (a_msg->type == F4SE::MessagingInterface::kPostLoadGame || a_msg->type == F4SE::MessagingInterface::kNewGame) {
		spdlog::info("game loaded (msg {})", a_msg->type);
		InstallCameraHook();
	}
	if (a_msg->type == F4SE::MessagingInterface::kGameDataReady) {
		spdlog::info("game data ready");
		if (!g_pollRunning.exchange(true)) {
			std::thread(PollGamepad).detach();
		}
	}
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
	SetupLog();

	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = "fo4flat_probe";
	a_info->version = 1;

	if (a_f4se->IsEditor()) {
		spdlog::critical("loaded in editor");
		return false;
	}

	spdlog::info("runtime {}", a_f4se->RuntimeVersion().string());
	return true;
}

// Papyrus native: fo4stereo_hud.SetHud(hp, maxHp, reserve), called by Fo4StereoHud.psc.
// Writes the values into shared memory (seqlock) for the proxy's HUD panels.
static void PapyrusSetHud(std::monostate, float a_hp, float a_maxHp, float a_reserve)
{
	if (!g_shm) {
		return;
	}
	InterlockedIncrement(&g_shm->hudSeq);  // odd: writing
	g_shm->hudHp = a_hp;
	g_shm->hudMaxHp = a_maxHp;
	g_shm->hudReserve = a_reserve;
	InterlockedExchange(&g_shm->hudValid, 1);
	InterlockedIncrement(&g_shm->hudSeq);  // even: done
}

static void PapyrusNoop(std::monostate)
{
}

static bool F4SEAPI PapyrusRegister(RE::BSScript::IVirtualMachine* a_vm)
{
	if (!a_vm) {
		return false;
	}
	// TEST (18.11.): register a parameterless no-op to see whether registration itself crashes.
	a_vm->BindNativeMethod("fo4stereo_hud", "Ping", PapyrusNoop, std::nullopt, false);
	spdlog::info("papyrus: fo4stereo_hud.SetHud registered");
	return true;
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
	SetupLog();
	F4SE::Init(a_f4se);
	spdlog::info("probe loaded (runtime {})", a_f4se->RuntimeVersion().string());
	if (const auto* msg = F4SE::GetMessagingInterface()) {
		msg->RegisterListener(OnMessage);
	}
	// Papyrus native registration crashes on this runtime (18.11.2026); disabled.
	// See docs/TODO.md "Papyrus-natiivi kaatuu". Re-enable only after the crash is fixed.
	// if (const auto* papyrus = F4SE::GetPapyrusInterface()) {
	// 	papyrus->Register(PapyrusRegister);
	// }
	return true;
}
