#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <YYToolkit/YYTK_Shared.hpp>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdio>
#include <cstring>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

using namespace Aurie;
using namespace YYTK;

static YYTKInterface* g_ModuleInterface = nullptr;
static bool g_MenuOpen = false;
static bool g_ImGuiReady = false;
static ID3D11Device* g_Device = nullptr;
static ID3D11DeviceContext* g_Context = nullptr;
static HWND g_Window = nullptr;

static void* g_SetCursorPos = nullptr;
static uint8_t g_SetCursorPosByte = 0;
static bool g_SetCursorPosPatched = false;

static void ApplySetCursorPosPatch()
{
	if (g_SetCursorPosPatched)
		return;
	if (g_SetCursorPos == nullptr)
	{
		const HMODULE user32 = GetModuleHandleW(L"user32.dll");
		g_SetCursorPos = reinterpret_cast<void*>(GetProcAddress(user32, "SetCursorPos"));
	}
	if (g_SetCursorPos == nullptr)
		return;

	auto* byte = static_cast<uint8_t*>(g_SetCursorPos);
	if (*byte == 0xC3)
		return;

	DWORD old = 0;
	if (!VirtualProtect(g_SetCursorPos, 1, PAGE_EXECUTE_READWRITE, &old))
		return;
	g_SetCursorPosByte = *byte;
	*byte = 0xC3;
	VirtualProtect(g_SetCursorPos, 1, old, &old);
	FlushInstructionCache(GetCurrentProcess(), g_SetCursorPos, 1);
	g_SetCursorPosPatched = true;
}

static void RemoveSetCursorPosPatch()
{
	if (!g_SetCursorPosPatched || g_SetCursorPos == nullptr)
		return;
	DWORD old = 0;
	if (VirtualProtect(g_SetCursorPos, 1, PAGE_EXECUTE_READWRITE, &old))
	{
		*static_cast<uint8_t*>(g_SetCursorPos) = g_SetCursorPosByte;
		VirtualProtect(g_SetCursorPos, 1, old, &old);
		FlushInstructionCache(GetCurrentProcess(), g_SetCursorPos, 1);
	}
	g_SetCursorPosPatched = false;
}

static void UpdateCursorLock()
{
	if (g_MenuOpen)
	{
		ApplySetCursorPosPatch();
		ClipCursor(nullptr);
		return;
	}
	RemoveSetCursorPosPatch();
}

static bool EnsureImGui(IDXGISwapChain* swap)
{
	if (g_ImGuiReady)
		return true;
	if (g_Device != nullptr)
		return false;

	if (FAILED(swap->GetDevice(IID_PPV_ARGS(&g_Device))))
	{
		DbgPrintEx(LOG_SEVERITY_ERROR, "[Well Dweller] GetDevice failed");
		return false;
	}

	g_Device->GetImmediateContext(&g_Context);
	if (g_Context == nullptr)
		return false;

	DXGI_SWAP_CHAIN_DESC desc{};
	if (FAILED(swap->GetDesc(&desc)) || desc.OutputWindow == nullptr)
	{
		DbgPrintEx(LOG_SEVERITY_ERROR, "[Well Dweller] swap chain has no window");
		return false;
	}
	g_Window = desc.OutputWindow;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();
	ImGui_ImplWin32_Init(g_Window);
	ImGui_ImplDX11_Init(g_Device, g_Context);
	g_ImGuiReady = true;
	DbgPrintEx(LOG_SEVERITY_INFO, "[Well Dweller] Menu ready. Toggle with Insert or ~.");
	return true;
}

static bool g_InfiniteHealth = false;
static bool g_InfiniteVessel = false;
static bool g_InfiniteStamina = false;
static bool g_InfiniteHover = false;
static bool g_InfiniteFly = false;
static bool g_FreeTrinkets = false;
static bool g_ConfigWritable = true;
static int g_FlyBindKind = 0;
static int g_FlyBindCode = 0;
static bool g_FlyBindListening = false;

static bool ConfigPath(wchar_t* path, size_t count)
{
	const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(count));
	if (length == 0 || length >= count)
		return false;
	wchar_t* slash = wcsrchr(path, L'\\');
	if (slash == nullptr)
		return false;
	slash[1] = L'\0';
	if (wcslen(path) + 22 >= count)
		return false;
	wcscat_s(path, count, L"well-mod\\config.json");
	return true;
}

// The release zip has no well-mod folder, so create it before the first save.
static void EnsureConfigFolder(const wchar_t* configPath)
{
	wchar_t folder[MAX_PATH]{};
	wcscpy_s(folder, configPath);
	wchar_t* slash = wcsrchr(folder, L'\\');
	if (slash == nullptr)
		return;
	*slash = L'\0';
	CreateDirectoryW(folder, nullptr);
}

static const char* JsonBoolAfter(const char* text, const char* key)
{
	char pattern[64]{};
	sprintf_s(pattern, "\"%s\"", key);
	const char* found = strstr(text, pattern);
	if (found == nullptr)
		return nullptr;
	found += strlen(pattern);
	while (*found == ' ' || *found == '\t' || *found == '\r' || *found == '\n' || *found == ':')
		++found;
	return found;
}

static bool ParseJsonBool(const char* text, bool& out)
{
	if (strncmp(text, "true", 4) == 0)
	{
		out = true;
		return true;
	}
	if (strncmp(text, "false", 5) == 0)
	{
		out = false;
		return true;
	}
	return false;
}

static bool ParseJsonInt(const char* text, int& out)
{
	if (text == nullptr || *text < '0' || *text > '9')
		return false;
	int value = 0;
	while (*text >= '0' && *text <= '9')
	{
		if (value > 100000)
			return false;
		value = value * 10 + (*text - '0');
		++text;
	}
	out = value;
	return true;
}

static void SaveConfig()
{
	if (!g_ConfigWritable)
		return;
	wchar_t path[MAX_PATH]{};
	if (!ConfigPath(path, MAX_PATH))
		return;
	char body[1024]{};
	sprintf_s(
		body,
		"{\r\n"
		"    \"infinite_health\": %s,\r\n"
		"    \"infinite_vessel\": %s,\r\n"
		"    \"infinite_stamina\": %s,\r\n"
		"    \"infinite_hover\": %s,\r\n"
		"    \"infinite_fly\": %s,\r\n"
		"    \"free_trinkets\": %s,\r\n"
		"    \"fly_bind_kind\": %d,\r\n"
		"    \"fly_bind_code\": %d\r\n"
		"}\r\n",
		g_InfiniteHealth ? "true" : "false",
		g_InfiniteVessel ? "true" : "false",
		g_InfiniteStamina ? "true" : "false",
		g_InfiniteHover ? "true" : "false",
		g_InfiniteFly ? "true" : "false",
		g_FreeTrinkets ? "true" : "false",
		g_FlyBindKind,
		g_FlyBindCode);
	EnsureConfigFolder(path);
	const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		DbgPrintEx(LOG_SEVERITY_ERROR, "[Well Dweller] config save failed");
		return;
	}
	DWORD wrote = 0;
	WriteFile(file, body, static_cast<DWORD>(strlen(body)), &wrote, nullptr);
	CloseHandle(file);
}

static void LoadConfig()
{
	wchar_t path[MAX_PATH]{};
	if (!ConfigPath(path, MAX_PATH))
		return;
	const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		DbgPrintEx(LOG_SEVERITY_INFO, "[Well Dweller] no config yet (well-mod/config.json)");
		return;
	}
	char text[1024]{};
	DWORD read = 0;
	ReadFile(file, text, sizeof(text) - 1, &read, nullptr);
	CloseHandle(file);
	text[read] = '\0';

	const char* cursor = text;
	while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
		++cursor;
	if (*cursor != '{')
	{
		g_ConfigWritable = false;
		DbgPrintEx(LOG_SEVERITY_ERROR, "[Well Dweller] config is not an object (file left untouched)");
		return;
	}

	bool infiniteHealth = false;
	bool infiniteVessel = false;
	bool infiniteStamina = false;
	bool infiniteHover = false;
	bool infiniteFly = false;
	bool freeTrinkets = false;
	int flyBindKind = 0;
	int flyBindCode = 0;
	const char* healthText = JsonBoolAfter(text, "infinite_health");
	const char* vesselText = JsonBoolAfter(text, "infinite_vessel");
	const char* staminaText = JsonBoolAfter(text, "infinite_stamina");
	const char* hoverText = JsonBoolAfter(text, "infinite_hover");
	const char* flyText = JsonBoolAfter(text, "infinite_fly");
	const char* trinketText = JsonBoolAfter(text, "free_trinkets");
	const char* bindKindText = JsonBoolAfter(text, "fly_bind_kind");
	const char* bindCodeText = JsonBoolAfter(text, "fly_bind_code");
	const bool healthOk = healthText == nullptr || ParseJsonBool(healthText, infiniteHealth);
	const bool vesselOk = vesselText == nullptr || ParseJsonBool(vesselText, infiniteVessel);
	const bool staminaOk = staminaText == nullptr || ParseJsonBool(staminaText, infiniteStamina);
	const bool hoverOk = hoverText == nullptr || ParseJsonBool(hoverText, infiniteHover);
	const bool flyOk = flyText == nullptr || ParseJsonBool(flyText, infiniteFly);
	const bool trinketOk = trinketText == nullptr || ParseJsonBool(trinketText, freeTrinkets);
	const bool bindKindOk = bindKindText == nullptr || ParseJsonInt(bindKindText, flyBindKind);
	const bool bindCodeOk = bindCodeText == nullptr || ParseJsonInt(bindCodeText, flyBindCode);
	if (!healthOk || !vesselOk || !staminaOk || !hoverOk || !flyOk || !trinketOk || !bindKindOk || !bindCodeOk || flyBindKind < 0 || flyBindKind > 4)
	{
		g_ConfigWritable = false;
		DbgPrintEx(LOG_SEVERITY_ERROR, "[Well Dweller] config did not parse (file left untouched)");
		return;
	}
	if (healthText != nullptr)
		g_InfiniteHealth = infiniteHealth;
	if (vesselText != nullptr)
		g_InfiniteVessel = infiniteVessel;
	if (staminaText != nullptr)
		g_InfiniteStamina = infiniteStamina;
	if (hoverText != nullptr)
		g_InfiniteHover = infiniteHover;
	if (flyText != nullptr)
		g_InfiniteFly = infiniteFly;
	if (trinketText != nullptr)
		g_FreeTrinkets = freeTrinkets;
	if (bindKindText != nullptr)
		g_FlyBindKind = flyBindKind == 1 ? 1 : 0;
	if (bindCodeText != nullptr)
		g_FlyBindCode = flyBindCode;
	DbgPrintEx(
		LOG_SEVERITY_INFO,
		"[Well Dweller] config loaded health %s vessel %s stamina %s hover %s fly %s trinkets %s",
		g_InfiniteHealth ? "on" : "off",
		g_InfiniteVessel ? "on" : "off",
		g_InfiniteStamina ? "on" : "off",
		g_InfiniteHover ? "on" : "off",
		g_InfiniteFly ? "on" : "off",
		g_FreeTrinkets ? "on" : "off");
}

static bool ReadGlobalReal(const char* name, double& out)
{
	if (g_ModuleInterface == nullptr)
		return false;
	const RValue value = g_ModuleInterface->CallBuiltin("variable_global_get", { RValue(name) });
	if (value.m_Kind == VALUE_UNDEFINED || value.m_Kind == VALUE_UNSET)
		return false;
	out = value.ToDouble();
	return true;
}

static void WriteGlobalReal(const char* name, double value)
{
	if (g_ModuleInterface == nullptr)
		return;
	g_ModuleInterface->CallBuiltin("variable_global_set", { RValue(name), RValue(value) });
}

// The oPlayer object index never changes during a run, so look it up once.
static bool FindPlayer(int& out)
{
	static double objectIndex = -1.0;
	if (objectIndex < 0.0)
	{
		const RValue index = g_ModuleInterface->CallBuiltin("asset_get_index", { RValue("oPlayer") });
		if (index.m_Kind == VALUE_UNSET || index.ToDouble() < 0.0)
			return false;
		objectIndex = index.ToDouble();
	}
	const RValue found = g_ModuleInterface->CallBuiltin("instance_find", { RValue(objectIndex), RValue(0.0) });
	if (found.m_Kind == VALUE_UNSET || found.ToInt32() < 0)
		return false;
	out = found.ToInt32();
	return true;
}

static bool ReadPlayerReal(const char* name, double& out)
{
	if (g_ModuleInterface == nullptr)
		return false;
	int player = -1;
	if (!FindPlayer(player))
		return false;
	const RValue value = g_ModuleInterface->CallBuiltin(
		"variable_instance_get",
		{ RValue(static_cast<double>(player)), RValue(name) });
	if (value.m_Kind == VALUE_UNDEFINED || value.m_Kind == VALUE_UNSET)
		return false;
	out = value.ToDouble();
	return true;
}

static void WritePlayerReal(const char* name, double value)
{
	if (g_ModuleInterface == nullptr)
		return;
	int player = -1;
	if (!FindPlayer(player))
		return;
	g_ModuleInterface->CallBuiltin(
		"variable_instance_set",
		{ RValue(static_cast<double>(player)), RValue(name), RValue(value) });
}

static void ApplyHealth()
{
	if (!g_InfiniteHealth)
		return;
	// Only write when the game has a real maximum, so the cap is never raised
	// and no globals are created before the game sets them up.
	double cap = 0.0;
	if (!ReadGlobalReal("player_max_health", cap) || cap <= 0.0)
		return;
	WriteGlobalReal("player_health", cap);
	WriteGlobalReal("health_meter", 1.0);
}

static void ApplyVessel()
{
	if (!g_InfiniteVessel)
		return;
	double cap = 0.0;
	if (!ReadGlobalReal("hit_bar_max", cap) || cap <= 0.0)
		return;
	WriteGlobalReal("hit_bar", cap);
}

static void ApplyStamina()
{
	if (!g_InfiniteStamina)
		return;
	double cap = 0.0;
	if (!ReadPlayerReal("stamina_max", cap) || cap <= 0.0)
		return;
	WritePlayerReal("stamina", 0.0);
}

static void ApplyHover()
{
	if (!g_InfiniteHover)
		return;
	double cap = 0.0;
	if (!ReadPlayerReal("fly_stamina_max", cap) || cap <= 0.0)
		return;
	WritePlayerReal("fly_stamina", 0.0);
}

static bool g_FlyHeld = false;

static void ApplyFly()
{
	double current = 0.0;
	if (!ReadPlayerReal("fly_tm", current))
		return;
	if (g_InfiniteFly)
	{
		double cap = 60.0;
		ReadGlobalReal("fly_time", cap);
		if (cap <= 0.0)
			cap = 60.0;
		WritePlayerReal("fly_tm", cap);
		g_FlyHeld = true;
		return;
	}
	if (!g_FlyHeld)
		return;
	WritePlayerReal("fly_tm", -1.0);
	g_FlyHeld = false;
}

// Original trinket costs, in the order the cost walk visits them. Kept so the
// costs come back when Free Trinket Costs is turned off.
static std::vector<double> g_TrinketOriginals;
static bool g_TrinketsZeroed = false;

static bool IsNumber(const RValue* value)
{
	return value != nullptr &&
		(value->m_Kind == VALUE_REAL || value->m_Kind == VALUE_INT32 || value->m_Kind == VALUE_INT64);
}

static double NumberOf(const RValue* value)
{
	if (value->m_Kind == VALUE_INT32)
		return static_cast<double>(value->m_i32);
	if (value->m_Kind == VALUE_INT64)
		return static_cast<double>(value->m_i64);
	return value->m_Real;
}

static void SetNumber(RValue* value, double number)
{
	if (value->m_Kind == VALUE_REAL)
		value->m_Real = number;
	else if (value->m_Kind == VALUE_INT32)
		value->m_i32 = static_cast<int32_t>(number);
	else if (value->m_Kind == VALUE_INT64)
		value->m_i64 = static_cast<int64_t>(number);
}

// zero == true: remember each nonzero cost, then set it to 0.
// zero == false: put the remembered costs back.
static void VisitCost(RValue* value, bool zero, size_t& slot)
{
	if (!IsNumber(value))
		return;
	if (slot >= g_TrinketOriginals.size())
		g_TrinketOriginals.resize(slot + 1, 0.0);
	if (zero)
	{
		const double current = NumberOf(value);
		if (current != 0.0)
			g_TrinketOriginals[slot] = current;
		SetNumber(value, 0.0);
	}
	else if (g_TrinketOriginals[slot] != 0.0)
		SetNumber(value, g_TrinketOriginals[slot]);
	++slot;
}

static void WalkCosts(RValue* value, int depth, bool zero, size_t& slot)
{
	if (value == nullptr || depth > 2 || g_ModuleInterface == nullptr)
		return;
	if (IsNumber(value))
	{
		VisitCost(value, zero, slot);
		return;
	}
	if (value->m_Kind == VALUE_ARRAY)
	{
		size_t size = 0;
		if (!AurieSuccess(g_ModuleInterface->GetArraySize(*value, size)))
			return;
		for (size_t index = 0; index < size && index < 128; ++index)
		{
			RValue* entry = nullptr;
			if (!AurieSuccess(g_ModuleInterface->GetArrayEntry(*value, index, entry)))
				continue;
			WalkCosts(entry, depth + 1, zero, slot);
		}
		return;
	}
	if (value->m_Kind == VALUE_OBJECT)
	{
		RValue* cost = nullptr;
		if (AurieSuccess(g_ModuleInterface->GetInstanceMember(*value, "cost", cost)))
			VisitCost(cost, zero, slot);
	}
}

static void ApplyTrinkets()
{
	if (g_ModuleInterface == nullptr)
		return;
	if (!g_FreeTrinkets && !g_TrinketsZeroed)
		return;
	RValue costs = g_ModuleInterface->CallBuiltin("variable_global_get", { RValue("upgrade_cost") });
	if (costs.m_Kind != VALUE_ARRAY)
		return;
	size_t slot = 0;
	WalkCosts(&costs, 0, g_FreeTrinkets, slot);
	g_TrinketsZeroed = g_FreeTrinkets;
}

static void AddCurrency(double amount)
{
	double current = 0.0;
	if (!ReadGlobalReal("currency", current))
		return;
	WriteGlobalReal("currency", current + amount);
}

static void FlyBindLabel(char* out, size_t count)
{
	if (g_FlyBindKind != 1)
	{
		strcpy_s(out, count, "None");
		return;
	}

	LONG param = static_cast<LONG>(MapVirtualKeyA(static_cast<UINT>(g_FlyBindCode), MAPVK_VK_TO_VSC)) << 16;
	switch (g_FlyBindCode)
	{
	case VK_LEFT:
	case VK_UP:
	case VK_RIGHT:
	case VK_DOWN:
	case VK_PRIOR:
	case VK_NEXT:
	case VK_HOME:
	case VK_END:
	case VK_INSERT:
	case VK_DELETE:
	case VK_DIVIDE:
	case VK_NUMLOCK:
		param |= 1 << 24;
		break;
	default:
		break;
	}
	if (GetKeyNameTextA(param, out, static_cast<int>(count)) > 0)
		return;
	sprintf_s(out, count, "Key %d", g_FlyBindCode);
}

// GetAsyncKeyState sees the whole system, so only act on keys while the game
// window is in front. Otherwise typing ~ in another app would open the menu.
static bool GameHasFocus()
{
	return g_Window != nullptr && GetForegroundWindow() == g_Window;
}

static void PollFlyBind(bool focused)
{
	static bool keyWas[256]{};
	static bool primed = false;

	int hitCode = 0;
	for (int vk = 1; vk < 256; ++vk)
	{
		if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2)
			continue;
		const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
		if (down && !keyWas[vk] && hitCode == 0)
			hitCode = vk;
		keyWas[vk] = down;
	}

	if (!primed)
	{
		primed = true;
		return;
	}
	if (hitCode == 0 || !focused)
		return;
	if (g_FlyBindListening)
	{
		if (hitCode == VK_ESCAPE)
		{
			g_FlyBindListening = false;
			return;
		}
		// Insert and ~ open the menu, so they can't be the fly key.
		if (hitCode == VK_INSERT || hitCode == VK_OEM_3)
			return;
		g_FlyBindKind = 1;
		g_FlyBindCode = hitCode;
		g_FlyBindListening = false;
		SaveConfig();
		return;
	}
	if (g_FlyBindKind == 1 && hitCode == g_FlyBindCode)
	{
		g_InfiniteFly = !g_InfiniteFly;
		ApplyFly();
		SaveConfig();
	}
}

static void DrawMenu()
{
	const bool tilde = (GetAsyncKeyState(VK_OEM_3) & 0x8000) != 0;
	const bool insert = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
	static bool wasDown = false;
	const bool focused = GameHasFocus();
	const bool down = tilde || insert;
	if (down && !wasDown && focused)
		g_MenuOpen = !g_MenuOpen;
	wasDown = down;
	PollFlyBind(focused);

	UpdateCursorLock();
	if (!g_MenuOpen || !g_ImGuiReady)
		return;

	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
	ImGuiIO& io = ImGui::GetIO();
	io.MouseDrawCursor = true;

	const float pad = 16.0f;
	float top = pad;
	float bottom = io.DisplaySize.y - pad;
	if (g_Window != nullptr)
	{
		POINT origin{0, 0};
		ClientToScreen(g_Window, &origin);
		MONITORINFO info{};
		info.cbSize = sizeof(info);
		if (GetMonitorInfoW(MonitorFromWindow(g_Window, MONITOR_DEFAULTTONEAREST), &info))
		{
			const float workTop = static_cast<float>(info.rcWork.top - origin.y);
			const float workBottom = static_cast<float>(info.rcWork.bottom - origin.y);
			if (workBottom > workTop + 200.0f)
			{
				top = workTop + pad;
				bottom = workBottom - pad;
			}
		}
	}

	const float width = 420.0f;
	const float height = bottom - top;
	const float x = io.DisplaySize.x - width - pad;
	ImGui::SetNextWindowPos(ImVec2(x, top), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);

	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;
	ImGui::Begin("Well Dweller", &g_MenuOpen, flags);

	static int tab = 0;
	const float spacing = ImGui::GetStyle().ItemSpacing.x;
	const float tabWidth = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
	const ImVec4 accent(0.12f, 0.55f, 0.58f, 1.0f);
	const ImVec4 idle(0.18f, 0.19f, 0.22f, 1.0f);
	ImGui::PushStyleColor(ImGuiCol_Button, tab == 0 ? accent : idle);
	if (ImGui::Button("Cheats##tab", ImVec2(tabWidth, 0.0f)))
		tab = 0;
	ImGui::PopStyleColor();
	ImGui::SameLine();
	ImGui::PushStyleColor(ImGuiCol_Button, tab == 1 ? accent : idle);
	if (ImGui::Button("Settings##tab", ImVec2(tabWidth, 0.0f)))
		tab = 1;
	ImGui::PopStyleColor();
	ImGui::Separator();

	if (tab == 0)
	{
		if (ImGui::CollapsingHeader("Toggles##section", ImGuiTreeNodeFlags_DefaultOpen))
		{
			double health = 0.0;
			double healthCap = 0.0;
			const bool haveHealth = ReadGlobalReal("player_health", health) && ReadGlobalReal("player_max_health", healthCap);
			ImGui::TextUnformatted("Health");
			ImGui::SameLine();
			if (haveHealth)
				ImGui::TextDisabled("%.0f / %.0f", health, healthCap);
			else
				ImGui::TextDisabled("waiting for player");
			if (ImGui::Checkbox("Infinite Health##cheat", &g_InfiniteHealth))
			{
				ApplyHealth();
				SaveConfig();
			}

			double vessel = 0.0;
			double vesselCap = 0.0;
			const bool haveVessel = ReadGlobalReal("hit_bar", vessel) && ReadGlobalReal("hit_bar_max", vesselCap);
			ImGui::TextUnformatted("Vessel");
			ImGui::SameLine();
			if (haveVessel)
				ImGui::TextDisabled("%.0f / %.0f", vessel, vesselCap);
			else
				ImGui::TextDisabled("waiting for player");
			if (ImGui::Checkbox("Infinite Vessel##cheat", &g_InfiniteVessel))
			{
				ApplyVessel();
				SaveConfig();
			}

			double stamina = 0.0;
			double staminaCap = 0.0;
			const bool haveStamina = ReadPlayerReal("stamina", stamina) && ReadPlayerReal("stamina_max", staminaCap);
			ImGui::TextUnformatted("Stamina");
			ImGui::SameLine();
			if (haveStamina)
			{
				const double full = staminaCap - stamina;
				ImGui::TextDisabled("%.0f / %.0f", full < 0.0 ? 0.0 : full, staminaCap);
			}
			else
				ImGui::TextDisabled("waiting for player");
			if (ImGui::Checkbox("Infinite Stamina##cheat", &g_InfiniteStamina))
			{
				ApplyStamina();
				SaveConfig();
			}

			double hover = 0.0;
			double hoverCap = 0.0;
			const bool haveHover = ReadPlayerReal("fly_stamina", hover) && ReadPlayerReal("fly_stamina_max", hoverCap);
			ImGui::TextUnformatted("Hover");
			ImGui::SameLine();
			if (haveHover)
			{
				const double full = hoverCap - hover;
				ImGui::TextDisabled("%.0f / %.0f", full < 0.0 ? 0.0 : full, hoverCap);
			}
			else
				ImGui::TextDisabled("waiting for player");
			if (ImGui::Checkbox("Infinite Hover##cheat", &g_InfiniteHover))
			{
				ApplyHover();
				SaveConfig();
			}

			double fly = 0.0;
			double flyCap = 60.0;
			const bool haveFly = ReadPlayerReal("fly_tm", fly);
			ReadGlobalReal("fly_time", flyCap);
			if (flyCap <= 0.0)
				flyCap = 60.0;
			ImGui::TextUnformatted("Fly");
			ImGui::SameLine();
			if (haveFly)
			{
				const double shown = fly < 0.0 ? 0.0 : fly;
				ImGui::TextDisabled("%.0f / %.0f", shown, flyCap);
			}
			else
				ImGui::TextDisabled("waiting for player");
			if (ImGui::Checkbox("Infinite Fly##cheat", &g_InfiniteFly))
			{
				ApplyFly();
				SaveConfig();
			}
			ImGui::TextDisabled("Turn off to fall through platforms.");

			double points = 0.0;
			const bool havePoints = ReadGlobalReal("upgrade_points", points);
			ImGui::TextUnformatted("Trinkets");
			ImGui::SameLine();
			if (havePoints)
				ImGui::TextDisabled("%.0f points", points);
			else
				ImGui::TextDisabled("waiting for player");
			if (ImGui::Checkbox("Free Trinket Costs##cheat", &g_FreeTrinkets))
			{
				ApplyTrinkets();
				SaveConfig();
			}
		}
		if (ImGui::CollapsingHeader("Quick add##section", ImGuiTreeNodeFlags_DefaultOpen))
		{
			double amount = 0.0;
			const bool haveCurrency = ReadGlobalReal("currency", amount);
			ImGui::TextUnformatted("Currency");
			ImGui::SameLine();
			if (haveCurrency)
				ImGui::TextDisabled("%.0f", amount);
			else
				ImGui::TextDisabled("waiting");
			if (ImGui::Button("Add 5000 Currency##cheat", ImVec2(-1.0f, 0.0f)) && haveCurrency)
				AddCurrency(5000.0);
		}
	}
	else if (ImGui::CollapsingHeader("Window##section", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::TextUnformatted("Toggle");
		ImGui::SameLine();
		ImGui::TextDisabled("Insert or ~");
		ImGui::TextUnformatted("Dock");
		ImGui::SameLine();
		ImGui::TextDisabled("right");
		ImGui::TextUnformatted("Config");
		ImGui::SameLine();
		ImGui::TextDisabled("well-mod/config.json");
		ImGui::TextUnformatted("Fly hotkey");
		ImGui::SameLine();
		char bindLabel[64]{};
		if (g_FlyBindListening)
			ImGui::TextDisabled("Press a key");
		else
		{
			FlyBindLabel(bindLabel, sizeof(bindLabel));
			ImGui::TextDisabled("%s", bindLabel);
		}
		if (ImGui::Button(g_FlyBindListening ? "Cancel##flybind" : "Set##flybind"))
			g_FlyBindListening = !g_FlyBindListening;
		if (g_FlyBindKind != 0)
		{
			ImGui::SameLine();
			if (ImGui::Button("Clear##flybind"))
			{
				g_FlyBindKind = 0;
				g_FlyBindCode = 0;
				g_FlyBindListening = false;
				SaveConfig();
			}
		}
		ImGui::TextDisabled("Esc cancels. Turn fly off to fall through platforms.");
	}

	ImGui::End();
	ImGui::Render();
}

static WNDPROC g_PreviousWnd = nullptr;

static LRESULT CALLBACK MenuWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	const bool toggle = msg == WM_KEYDOWN && (wp == VK_OEM_3 || wp == VK_INSERT);
	if (toggle)
		return 1;

	if (g_MenuOpen && g_ImGuiReady)
	{
		ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
		const ImGuiIO& io = ImGui::GetIO();
		const bool mouse = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_INPUT;
		const bool key = msg >= WM_KEYFIRST && msg <= WM_KEYLAST;
		if (mouse || (key && io.WantCaptureKeyboard))
			return 1;
	}

	return CallWindowProcW(g_PreviousWnd, hwnd, msg, wp, lp);
}

static void OnPresent(IDXGISwapChain* swap)
{
	if (swap == nullptr)
		return;

	UINT presentCount = 0;
	if (SUCCEEDED(swap->GetLastPresentCount(&presentCount)))
	{
		static UINT lastDrawn = UINT_MAX;
		if (presentCount == lastDrawn)
			return;
		lastDrawn = presentCount;
	}

	if (!EnsureImGui(swap))
		return;

	if (g_PreviousWnd == nullptr && g_Window != nullptr)
	{
		g_PreviousWnd = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
			g_Window,
			GWLP_WNDPROC,
			reinterpret_cast<LONG_PTR>(MenuWndProc)));
	}

	ApplyHealth();
	ApplyVessel();
	ApplyStamina();
	ApplyHover();
	ApplyFly();
	ApplyTrinkets();
	DrawMenu();
	if (!g_MenuOpen || ImGui::GetDrawData() == nullptr)
		return;

	ID3D11Texture2D* back = nullptr;
	if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&back))))
		return;

	ID3D11RenderTargetView* target = nullptr;
	const HRESULT created = g_Device->CreateRenderTargetView(back, nullptr, &target);
	back->Release();
	if (FAILED(created) || target == nullptr)
		return;

	g_Context->OMSetRenderTargets(1, &target, nullptr);
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	target->Release();
}

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

static PresentFn g_OrigPresent = nullptr;
static Present1Fn g_OrigPresent1 = nullptr;

static HRESULT STDMETHODCALLTYPE HkPresent(IDXGISwapChain* swap, UINT sync, UINT flags)
{
	OnPresent(swap);
	return g_OrigPresent(swap, sync, flags);
}

static HRESULT STDMETHODCALLTYPE HkPresent1(IDXGISwapChain1* swap, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params)
{
	OnPresent(swap);
	return g_OrigPresent1(swap, sync, flags, params);
}

static void InstallPresentHooks(IDXGISwapChain* swap)
{
	auto** vtable = *reinterpret_cast<void***>(swap);
	PVOID trampoline = nullptr;
	AurieStatus status = MmCreateHook(g_ArSelfModule, "MenuPresent", vtable[8], reinterpret_cast<PVOID>(HkPresent), &trampoline);
	g_OrigPresent = reinterpret_cast<PresentFn>(trampoline);
	DbgPrintEx(LOG_SEVERITY_INFO, "[Well Dweller] Present hook %s", AurieStatusToString(status));

	trampoline = nullptr;
	status = MmCreateHook(g_ArSelfModule, "MenuPresent1", vtable[22], reinterpret_cast<PVOID>(HkPresent1), &trampoline);
	g_OrigPresent1 = reinterpret_cast<Present1Fn>(trampoline);
	DbgPrintEx(LOG_SEVERITY_INFO, "[Well Dweller] Present1 hook %s", AurieStatusToString(status));
}

static void WndProcCallback(FWWndProc& WindowContext)
{
	auto& args = WindowContext.Arguments();
	const HWND hwnd = std::get<0>(args);
	const UINT msg = std::get<1>(args);
	const WPARAM wp = std::get<2>(args);
	const LPARAM lp = std::get<3>(args);

	// Once the window is subclassed, MenuWndProc sees every message first and
	// already hands it to ImGui. Stop here so ImGui does not get it twice.
	if (g_PreviousWnd != nullptr)
		return;

	const bool toggle = msg == WM_KEYDOWN && (wp == VK_OEM_3 || wp == VK_INSERT);
	if (toggle)
	{
		WindowContext.Override(1);
		return;
	}

	if (!g_MenuOpen || !g_ImGuiReady)
		return;

	ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
	const ImGuiIO& io = ImGui::GetIO();
	const bool mouse = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_INPUT;
	const bool key = msg >= WM_KEYFIRST && msg <= WM_KEYLAST;
	if (mouse || (key && io.WantCaptureKeyboard))
		WindowContext.Override(1);
}

EXPORTED AurieStatus ModuleInitialize(
	IN AurieModule* Module,
	IN const fs::path& ModulePath
)
{
	UNREFERENCED_PARAMETER(ModulePath);

	g_ModuleInterface = YYTK::GetInterface();
	if (!g_ModuleInterface)
		return AURIE_MODULE_DEPENDENCY_NOT_RESOLVED;

	AurieStatus last_status = g_ModuleInterface->CreateCallback(Module, EVENT_WNDPROC, WndProcCallback, 0);
	if (!AurieSuccess(last_status))
		return last_status;

	RValue info;
	last_status = g_ModuleInterface->CallBuiltinEx(info, "os_get_info", nullptr, nullptr, {});
	if (!AurieSuccess(last_status))
	{
		DbgPrintEx(LOG_SEVERITY_ERROR, "[Well Dweller] os_get_info failed: %s", AurieStatusToString(last_status));
		return AURIE_SUCCESS;
	}

	RValue swapValue;
	last_status = g_ModuleInterface->CallBuiltinEx(
		swapValue,
		"ds_map_find_value",
		nullptr,
		nullptr,
		{ info, "video_d3d11_swapchain" });
	if (!AurieSuccess(last_status))
	{
		DbgPrintEx(LOG_SEVERITY_ERROR, "[Well Dweller] swapchain lookup failed: %s", AurieStatusToString(last_status));
		return AURIE_SUCCESS;
	}

	auto* swap = static_cast<IDXGISwapChain*>(swapValue.m_Pointer);
	if (swap != nullptr)
		InstallPresentHooks(swap);

	LoadConfig();
	DbgPrintEx(LOG_SEVERITY_INFO, "[Well Dweller] Plugin loaded.");
	return AURIE_SUCCESS;
}
