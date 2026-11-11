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
	spdlog::info("engine mode={} ipd_units={}", g_engStereo ? "stereo" : "wobble", g_engIpd);
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
struct StateVt
{
	void** vtbl{ nullptr };
	StateUpdateFn update{ nullptr };
	StateGetTransFn getTrans{ nullptr };
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

static void HookedStateUpdate(RE::TESCameraState* a_self, void* a_next)
{
	// New frame of camera updates: switch eye before the camera position is queried.
	if (g_engStereo) {
		const long eye = g_eye.load() ^ 1;
		g_eye.store(eye);
		if (g_shm) {
			InterlockedExchange(&g_shm->eye, eye);
			InterlockedIncrement(&g_shm->seq);
		}
	}
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
	if (!g_engEnabled || g_engPeriod <= 0.0f || !g_cam) {
		return;
	}
	RE::NiNode* root = g_cam->cameraRoot.get();
	if (!root) {
		return;
	}
	const auto& m = root->world.rotate;
	const float rx = m.entry[0].pt[0], ry = m.entry[0].pt[1], rz = m.entry[0].pt[2];
	const float shift = g_engStereo ? (g_eye.load() == 0 ? -0.5f : 0.5f) * g_engIpd : CurrentShift();
	a_out.x += rx * shift;
	a_out.y += ry * shift;
	a_out.z += rz * shift;
	if (n <= 5 || n % 600 == 0) {
		spdlog::info("getTrans #{} state#{} shift={:.2f} right=({:.3f} {:.3f} {:.3f}) out=({:.1f} {:.1f} {:.1f}) stateUpdates={} camUpdates={}",
			n, vt ? vt->index : -1, shift, rx, ry, rz, a_out.x, a_out.y, a_out.z, g_stateUpdates.load(), g_camUpdates);
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
		++g_nStateVt;
		const bool ok = PatchSlot(vtbl, 0x0B, reinterpret_cast<void*>(&HookedStateUpdate))
		                && PatchSlot(vtbl, 0x0D, reinterpret_cast<void*>(&HookedGetTranslation));
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
