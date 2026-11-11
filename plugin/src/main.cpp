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
#include <cstdlib>
#include <mutex>
#include <string>
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
static float g_aimIpdScale = 0.0f;    // [engine] aim_ipd_scale
static unsigned g_vrCamMask = (1u << 0) | (1u << 7);  // [vr] vr_cam_states
static RE::NiPoint3 g_lastWritten{};
static RE::NiPoint3 g_lastOffset{};
static bool g_haveWritten = false;
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
	GetPrivateProfileStringA("engine", "mode", "wobble", buf, sizeof(buf), ini.c_str());
	g_engStereo = std::strcmp(buf, "stereo") == 0;
	GetPrivateProfileStringA("engine", "ipd_units", "4.48", buf, sizeof(buf), ini.c_str());
	g_engIpd = static_cast<float>(std::atof(buf));
	GetPrivateProfileStringA("engine", "compensate", "3", buf, sizeof(buf), ini.c_str());
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
	GetPrivateProfileStringA("vr", "vr_cam_states", "0,7", buf, sizeof(buf), ini.c_str());
	g_vrCamMask = 0;
	for (char* p = buf; *p;) {
		const long v = std::strtol(p, &p, 10);
		if (v >= 0 && v < 32) g_vrCamMask |= 1u << v;
		while (*p == ',' || *p == ' ') ++p;
		if (*p && (*p < '0' || *p > '9')) break;
	}
	spdlog::info("vr camera states mask=0x{:X}", g_vrCamMask);
	spdlog::info("engine mode={} ipd_units={} compensate={} head_rotation={} head_rot_compensate={} head_position={} units_per_meter={}",
		g_engStereo ? "stereo" : "wobble", g_engIpd, g_compensate, g_headRotMode, g_headRotComp, g_headPos, g_unitsPerMeter);
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

static void HookedStateUpdate(RE::TESCameraState* a_self, void* a_next)
{
	// New frame of camera updates. The proxy owns the eye: it is the parity of the
	// number of frames presented so far (the frame being prepared is the next one).
	long eye = g_eye.load();
	if (g_engStereo) {
		eye = g_shm ? (g_shm->presents & 1) : (eye ^ 1);
		g_eye.store(eye);
		if (g_shm) {
			InterlockedExchange(&g_shm->eye, eye);
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
	}
	// stereo/head tracking only for the camera states shown in VR ([vr] vr_cam_states)
	const bool firstPerson = g_curStateIdx >= 0 && g_curStateIdx < 32 && (g_vrCamMask & (1u << g_curStateIdx));
	const bool aiming = g_shm && g_shm->zoom == 1;

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

	RE::NiPoint3 applied{ 0.0f, 0.0f, 0.0f };
	if (g_engEnabled && firstPerson) {
		float shift = g_engStereo ? (eye == 0 ? -0.5f : 0.5f) * g_engIpd : CurrentShift();
		if (aiming) shift *= g_aimIpdScale;  // sights right at the eye: mono avoids double vision
		applied.x += right.x * shift;
		applied.y += right.y * shift;
		applied.z += right.z * shift;
	}

	// Head pose from the proxy (OpenVR seated space, metres; x right, y up, -z forward).
	float pos[3]{}, q[4]{ 1.0f, 0.0f, 0.0f, 0.0f };
	g_lastPoseValid = firstPerson && g_shm && ReadHmdPose(g_shm, pos, q);
	float headQ[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
	if (g_lastPoseValid) {
		std::memcpy(g_lastHmdPos, pos, sizeof(pos));
		if (g_headPos) {
			const float s = g_unitsPerMeter;
			applied.x += (right.x * pos[0] - fwd.x * pos[2] + up.x * pos[1]) * s;
			applied.y += (right.y * pos[0] - fwd.y * pos[2] + up.y * pos[1]) * s;
			applied.z += (right.z * pos[0] - fwd.z * pos[2] + up.z * pos[1]) * s;
		}
		if (g_headRotMode != 0) {
			// VR (x right, y up, z back) -> game (x right, y forward, z up): v -> (x, -z, y)
			headQ[0] = q[0];
			headQ[1] = q[1];
			headQ[2] = -q[3];
			headQ[3] = q[2];
			QuatNormalize(headQ);
		}
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

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
	SetupLog();
	F4SE::Init(a_f4se);
	spdlog::info("probe loaded (runtime {})", a_f4se->RuntimeVersion().string());
	if (const auto* msg = F4SE::GetMessagingInterface()) {
		msg->RegisterListener(OnMessage);
	}
	return true;
}
