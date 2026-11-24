#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d9.lib")

namespace offset
{
	// player
	constexpr std::ptrdiff_t m_iHealth = 0x100;
	constexpr std::ptrdiff_t m_fFlags = 0x104;
	constexpr std::ptrdiff_t m_vecVelocity = 0x114;
}

namespace config
{
	constexpr float fontHeightFrac = 0.024f; // font size as a fraction of screen height, lower = smaller
	constexpr float yFrac = 0.90f;           // vertical position, 0 = top, 1 = bottom

	constexpr const char* fontName = "Segoe UI"; // Tahoma, Arial, Verdana or Consolas also work

	// text color, yellow
	constexpr std::uint32_t textR = 255;
	constexpr std::uint32_t textG = 224;
	constexpr std::uint32_t textB = 51;

	constexpr std::uint32_t shadowAlpha = 190; // 0 = no shadow, 255 = solid black
}

// horizontal speed, written by the bhop thread, read by the render hook
std::atomic<float> g_speed{ 0.f };

// scans the executable sections of a loaded module, '?' is a wildcard
std::uintptr_t FindPattern(const char* moduleName, const char* pattern) noexcept
{
	const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(moduleName));
	if (!base)
		return 0;

	std::vector<int> bytes;
	for (const char* p = pattern; *p;) {
		if (*p == ' ') { ++p; continue; }
		if (*p == '?') {
			bytes.push_back(-1);
			while (*p == '?') ++p;
			continue;
		}
		bytes.push_back(static_cast<int>(std::strtoul(p, nullptr, 16)));
		p += 2;
	}

	const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
	const auto section = IMAGE_FIRST_SECTION(nt);

	for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
		if (!(section[s].Characteristics & IMAGE_SCN_MEM_EXECUTE))
			continue;

		const auto start = reinterpret_cast<const std::uint8_t*>(base + section[s].VirtualAddress);
		const auto length = section[s].Misc.VirtualSize;

		for (std::size_t i = 0; i + bytes.size() <= length; ++i) {
			bool match = true;
			for (std::size_t j = 0; j < bytes.size(); ++j) {
				if (bytes[j] != -1 && start[i + j] != bytes[j]) {
					match = false;
					break;
				}
			}
			if (match)
				return reinterpret_cast<std::uintptr_t>(start + i);
		}
	}
	return 0;
}

// ---------------------------------------------------------------- overlay

using EndScene_t = HRESULT(__stdcall*)(IDirect3DDevice9*);
EndScene_t oEndScene = nullptr;
void** g_vtable = nullptr;
constexpr int kEndSceneIndex = 42;

// gdi draws the text into a bitmap, the bitmap is copied into a d3d9 texture, the texture is drawn as a quad
struct SpeedText
{
	HDC dc = nullptr;
	HBITMAP bitmap = nullptr;
	HFONT font = nullptr;
	HGDIOBJ oldBitmap = nullptr;
	HGDIOBJ oldFont = nullptr;
	void* bits = nullptr;
	IDirect3DTexture9* texture = nullptr;
	int texW = 0, texH = 0; // texture size, power of two
	int w = 0, h = 0;       // area the text actually uses
	int fontPx = 0;
	int shown = -1;         // value currently in the texture
} g_text;

int g_failedPx = 0;                  // font size that failed to build, so we don't retry every frame
std::atomic<bool> g_textFailed{ false };
std::atomic<bool> g_unloading{ false };
std::atomic<bool> g_freed{ false };

int NextPow2(int v) noexcept
{
	int p = 1;
	while (p < v)
		p <<= 1;
	return p;
}

void FreeText() noexcept
{
	if (g_text.texture)
		g_text.texture->Release();

	if (g_text.dc) {
		if (g_text.oldFont)
			SelectObject(g_text.dc, g_text.oldFont);
		if (g_text.oldBitmap)
			SelectObject(g_text.dc, g_text.oldBitmap);
	}

	if (g_text.font)
		DeleteObject(g_text.font);
	if (g_text.bitmap)
		DeleteObject(g_text.bitmap);
	if (g_text.dc)
		DeleteDC(g_text.dc);

	g_text = SpeedText{};
}

bool BuildText(IDirect3DDevice9* dev, int fontPx) noexcept
{
	FreeText();
	g_text.fontPx = fontPx;

	g_text.dc = CreateCompatibleDC(nullptr);
	if (!g_text.dc)
		return false;

	g_text.font = CreateFontA(-fontPx, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
		OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, config::fontName);
	if (!g_text.font)
		return false;
	g_text.oldFont = SelectObject(g_text.dc, g_text.font);

	// size the texture for the widest case, 4 digits, with room around it for the shadow
	SIZE sz{};
	GetTextExtentPoint32A(g_text.dc, "0000", 4, &sz);
	g_text.w = sz.cx + 10;
	g_text.h = sz.cy + 6;
	g_text.texW = NextPow2(g_text.w);
	g_text.texH = NextPow2(g_text.h);

	BITMAPINFO bmi{};
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = g_text.texW;
	bmi.bmiHeader.biHeight = -g_text.texH; // top-down
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	g_text.bitmap = CreateDIBSection(g_text.dc, &bmi, DIB_RGB_COLORS, &g_text.bits, nullptr, 0);
	if (!g_text.bitmap)
		return false;
	g_text.oldBitmap = SelectObject(g_text.dc, g_text.bitmap);

	SetBkMode(g_text.dc, TRANSPARENT);
	SetTextColor(g_text.dc, RGB(255, 255, 255));

	return SUCCEEDED(dev->CreateTexture(g_text.texW, g_text.texH, 1, 0,
		D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_text.texture, nullptr));
}

void UpdateText(int value) noexcept
{
	std::memset(g_text.bits, 0, static_cast<std::size_t>(g_text.texW) * g_text.texH * 4);

	char buf[16];
	const int n = sprintf_s(buf, "%d", value);
	RECT rc{ 0, 0, g_text.w, g_text.h };
	DrawTextA(g_text.dc, buf, n, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
	GdiFlush();

	D3DLOCKED_RECT lr{};
	if (FAILED(g_text.texture->LockRect(0, &lr, nullptr, 0)))
		return;

	const auto src = static_cast<const std::uint32_t*>(g_text.bits);
	const int shadowOffset = std::max(1, g_text.fontPx / 14);

	// white text on black, so the green channel is the coverage
	const auto coverage = [&](int x, int y) -> std::uint32_t {
		if (x < 0 || y < 0 || x >= g_text.texW || y >= g_text.texH)
			return 0;
		return (src[y * g_text.texW + x] >> 8) & 0xFF;
		};

	for (int y = 0; y < g_text.texH; ++y) {
		auto dst = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(lr.pBits) + y * lr.Pitch);
		for (int x = 0; x < g_text.texW; ++x) {
			const std::uint32_t a = coverage(x, y);
			const std::uint32_t s = coverage(x - shadowOffset, y - shadowOffset) * config::shadowAlpha / 255;

			// yellow text over a black shadow, composited into one texel
			const std::uint32_t outA = a + s * (255 - a) / 255;
			if (outA == 0) {
				dst[x] = 0;
				continue;
			}

			const std::uint32_t r = config::textR * a / outA;
			const std::uint32_t g = config::textG * a / outA;
			const std::uint32_t b = config::textB * a / outA;
			dst[x] = (outA << 24) | (r << 16) | (g << 8) | b;
		}
	}
	g_text.texture->UnlockRect(0);
	g_text.shown = value;
}

void DrawOverlay(IDirect3DDevice9* dev) noexcept
{
	// only draw on the backbuffer pass
	IDirect3DSurface9* rt = nullptr;
	IDirect3DSurface9* bb = nullptr;
	dev->GetRenderTarget(0, &rt);
	dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
	const bool onBackBuffer = rt && rt == bb;
	if (rt) rt->Release();
	if (bb) bb->Release();
	if (!onBackBuffer)
		return;

	D3DVIEWPORT9 vp{};
	if (FAILED(dev->GetViewport(&vp)) || vp.Width == 0)
		return;

	const int screenW = static_cast<int>(vp.Width);
	const int screenH = static_cast<int>(vp.Height);

	const int fontPx = std::max(12, static_cast<int>(screenH * config::fontHeightFrac));
	if (fontPx == g_failedPx)
		return;

	if (!g_text.texture || g_text.fontPx != fontPx) {
		if (!BuildText(dev, fontPx)) {
			FreeText();
			g_failedPx = fontPx;
			g_textFailed.store(true);
			return;
		}
	}

	const int value = std::max(0, std::min(static_cast<int>(g_speed.load() + 0.5f), 9999));
	if (value != g_text.shown)
		UpdateText(value);

	struct Vertex { float x, y, z, rhw, u, v; };

	// -0.5 lines texels up with pixels in d3d9
	const float x0 = static_cast<float>(static_cast<int>(vp.X) + screenW / 2 - g_text.w / 2) - 0.5f;
	const float y0 = static_cast<float>(vp.Y) + screenH * config::yFrac - 0.5f;
	const float x1 = x0 + g_text.w;
	const float y1 = y0 + g_text.h;
	const float u1 = static_cast<float>(g_text.w) / g_text.texW;
	const float v1 = static_cast<float>(g_text.h) / g_text.texH;

	const Vertex quad[4] = {
		{ x0, y0, 0.f, 1.f, 0.f, 0.f },
		{ x1, y0, 0.f, 1.f, u1,  0.f },
		{ x0, y1, 0.f, 1.f, 0.f, v1 },
		{ x1, y1, 0.f, 1.f, u1,  v1 },
	};

	// save the game's device state so drawing doesn't leak into its rendering
	IDirect3DStateBlock9* saved = nullptr;
	if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved)))
		return;

	dev->SetPixelShader(nullptr);
	dev->SetVertexShader(nullptr);
	dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
	dev->SetTexture(0, g_text.texture);

	dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
	dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
	dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
	dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

	dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

	dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
	dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

	dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));

	saved->Apply();
	saved->Release();
}

HRESULT __stdcall hkEndScene(IDirect3DDevice9* dev)
{
	if (g_unloading.load()) {
		// free the d3d resources on the render thread, then stop drawing
		if (!g_freed.load()) {
			FreeText();
			g_freed.store(true);
		}
	}
	else
		DrawOverlay(dev);

	return oEndScene(dev);
}

bool InstallOverlay() noexcept
{
	// pointer to the game's IDirect3DDevice9
	const auto sig = FindPattern("shaderapidx9.dll", "A1 ? ? ? ? 50 8B 08 FF 51 0C");
	if (!sig)
		return false;

	const auto device = **reinterpret_cast<IDirect3DDevice9***>(sig + 1);
	if (!device)
		return false;

	g_vtable = *reinterpret_cast<void***>(device);

	DWORD old = 0;
	if (!VirtualProtect(&g_vtable[kEndSceneIndex], sizeof(void*), PAGE_READWRITE, &old))
		return false;

	oEndScene = reinterpret_cast<EndScene_t>(g_vtable[kEndSceneIndex]);
	g_vtable[kEndSceneIndex] = reinterpret_cast<void*>(&hkEndScene);
	VirtualProtect(&g_vtable[kEndSceneIndex], sizeof(void*), old, &old);
	return true;
}

void RemoveOverlay() noexcept
{
	if (!g_vtable || !oEndScene)
		return;

	// give the render thread up to 500ms to free its resources
	g_unloading.store(true);
	for (int i = 0; i < 50 && !g_freed.load(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));

	DWORD old = 0;
	VirtualProtect(&g_vtable[kEndSceneIndex], sizeof(void*), PAGE_READWRITE, &old);
	g_vtable[kEndSceneIndex] = reinterpret_cast<void*>(oEndScene);
	VirtualProtect(&g_vtable[kEndSceneIndex], sizeof(void*), old, &old);

	// let any in-flight hook call finish before the dll goes away
	std::this_thread::sleep_for(std::chrono::milliseconds(250));
}

// ---------------------------------------------------------------- engine console

using CreateInterface_t = void* (__cdecl*)(const char*, int*);
using ClientCmdUnrestricted_t = void(__thiscall*)(void*, const char*, bool);

void* g_engine = nullptr;

bool InitEngine() noexcept
{
	const auto module = GetModuleHandleA("engine.dll");
	if (!module)
		return false;

	const auto createInterface = reinterpret_cast<CreateInterface_t>(GetProcAddress(module, "CreateInterface"));
	if (!createInterface)
		return false;

	g_engine = createInterface("VEngineClient014", nullptr);
	return g_engine != nullptr;
}

void ConsoleCmd(const char* cmd) noexcept
{
	if (!g_engine)
		return;

	const auto vtable = *reinterpret_cast<void***>(g_engine);
	reinterpret_cast<ClientCmdUnrestricted_t>(vtable[114])(g_engine, cmd, false); // ClientCmd_Unrestricted
}

// ---------------------------------------------------------------- bhop

void BunnyHop(const HMODULE instance) noexcept
{
	// wait for the game to load its modules
	while (!GetModuleHandleA("client.dll") || !GetModuleHandleA("shaderapidx9.dll"))
		std::this_thread::sleep_for(std::chrono::milliseconds(100));

	const auto localPlayerSig = FindPattern("client.dll", "8D 34 85 ? ? ? ? 89 15 ? ? ? ? 8B 41 08 8B 48 04 83 F9 FF");
	const auto forceJumpSig = FindPattern("client.dll", "8B 0D ? ? ? ? 8B D6 8B C1 83 CA 02");

	if (!localPlayerSig || !forceJumpSig) {
		MessageBoxA(nullptr, "sig scan failed", "bhop", 0);
		FreeLibraryAndExitThread(instance, 0);
	}

	// absolute addresses, we're internal so no need to subtract the base
	const auto localPlayerPtr = *reinterpret_cast<std::uintptr_t*>(localPlayerSig + 3) + 4;
	const auto forceJumpPtr = *reinterpret_cast<std::uintptr_t*>(forceJumpSig + 2);

	const bool overlay = InstallOverlay();
	if (!overlay)
		MessageBoxA(nullptr, "overlay sig failed, bhop still runs without the meter", "bhop", 0);

	if (!InitEngine())
		MessageBoxA(nullptr, "engine interface failed, run sv_enablebunnyhopping 1 by hand", "bhop", 0);

	bool inGame = false;
	bool warned = false;

	//loop
	while (!GetAsyncKeyState(VK_END)) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));

		// shown from this thread on purpose, so it never blocks the render thread
		if (g_textFailed.load() && !warned) {
			warned = true;
			MessageBoxA(nullptr, "speed text failed to build", "bhop", 0);
		}

		//get local player
		const auto localPlayer = *reinterpret_cast<std::uintptr_t*>(localPlayerPtr);

		// is valid
		if (!localPlayer) {
			g_speed.store(0.f);
			inGame = false;
			continue;
		}

		// fires once per map load, when the local player first exists
		if (!inGame) {
			inGame = true;
			ConsoleCmd("sv_enablebunnyhopping 1");
		}

		//checks if its alive
		const auto health = *reinterpret_cast<std::int32_t*>(localPlayer + offset::m_iHealth);
		if (!health) {
			g_speed.store(0.f);
			continue;
		}

		// horizontal speed for the meter
		const auto vel = reinterpret_cast<const float*>(localPlayer + offset::m_vecVelocity);
		g_speed.store(std::sqrt(vel[0] * vel[0] + vel[1] * vel[1]));

		// makes sure space is pressed
		if (!GetAsyncKeyState(VK_SPACE))
			continue;

		const auto flags = *reinterpret_cast<std::int32_t*>(localPlayer + offset::m_fFlags);

		//ongrounds
		*reinterpret_cast<std::uintptr_t*>(forceJumpPtr) = (flags & (1 << 0)) ? 6 : 4; // 6 = jump, 4 = reset
	}

	if (overlay)
		RemoveOverlay();

	//uninject
	FreeLibraryAndExitThread(instance, 0);
}

int __stdcall DllMain(
	const HMODULE instance,
	const std::uintptr_t reason,
	const void* reserved
)
{
	if (reason == 1) {
		DisableThreadLibraryCalls(instance);

		const auto thread = CreateThread(
			nullptr,
			0,
			reinterpret_cast<LPTHREAD_START_ROUTINE>(BunnyHop),
			instance,
			0,
			nullptr
		);

		if (thread)
			CloseHandle(thread);
	}
	return 1;
}