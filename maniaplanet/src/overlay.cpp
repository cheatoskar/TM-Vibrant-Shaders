// The F8 menu in ManiaPlanet: the TMNF mod's menu (src/menu.cpp) on ImGui's D3D11 backend.
// Window messages arrive on the game's window thread, ImGui runs on the render thread: they are
// queued and handed over at Present.
#include "overlay.h"
#include "capture.h"
#include "config.h"
#include "engine_frame.h"
#include "hooks.h"
#include "log.h"
#include "menu.h"
#include "options.h"
#include "renderer.h"
#include "shaderdb.h"
#include "thumbnails11.h"
#include "photo11.h"
#include "shaders/PS_UiComposite.h"
#include "shaders/VS_Full.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include <shellapi.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace tmvs_mp {
namespace overlay {
namespace {

using tmshaders::Config;

HWND g_window = nullptr;
WNDPROC g_originalProc = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
bool g_ready = false;
std::atomic<bool> g_open{false};
std::atomic<bool> g_wantMouse{false};
std::atomic<bool> g_wantKeyboard{false};
ULONGLONG g_toastUntil = 0;
std::string g_toast;

struct Message {
    UINT msg;
    WPARAM w;
    LPARAM l;
};

template <typename T> void safeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

bool isSrgb(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

DXGI_FORMAT typelessOf(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    default: return f;
    }
}

// ImGui on an sRGB back buffer (ManiaPlanet's is B8G8R8A8_UNORM_SRGB): its display-value colours
// went through the view's sRGB encoding a second time and the menu came out pale and glaring. It
// draws into this UNORM layer instead, and ui.hlsl lays the layer over the frame in display values.
struct UiLayer {
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11RasterizerState* rs = nullptr;
    ID3D11BlendState* blend = nullptr;
    ID3D11DepthStencilState* dss = nullptr;
    ID3D11Texture2D* layer = nullptr;
    ID3D11RenderTargetView* layerRtv = nullptr;
    ID3D11ShaderResourceView* layerSrv = nullptr;
    ID3D11Texture2D* frame = nullptr; // copy of the back buffer
    ID3D11ShaderResourceView* frameSrv = nullptr;
    UINT w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool failed = false;

    void releaseTargets() {
        safeRelease(layerSrv);
        safeRelease(layerRtv);
        safeRelease(layer);
        safeRelease(frameSrv);
        safeRelease(frame);
        w = h = 0;
    }

    bool init(ID3D11Device* device) {
        if (vs) return true;
        bool ok = SUCCEEDED(device->CreateVertexShader(g_VS_Full, sizeof(g_VS_Full), nullptr, &vs));
        ok &= SUCCEEDED(device->CreatePixelShader(g_PS_UiComposite, sizeof(g_PS_UiComposite), nullptr, &ps));
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        ok &= SUCCEEDED(device->CreateRasterizerState(&rd, &rs));
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ok &= SUCCEEDED(device->CreateBlendState(&bd, &blend));
        D3D11_DEPTH_STENCIL_DESC dd = {};
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        ok &= SUCCEEDED(device->CreateDepthStencilState(&dd, &dss));
        if (!ok) TMVS_LOG("overlay: menu layer shaders failed, the menu draws straight into the back buffer");
        failed = !ok;
        return ok;
    }

    // The layer for a back buffer of this size and format; false: draw straight into it.
    bool ensure(ID3D11Device* device, ID3D11Texture2D* backBuffer) {
        if (failed || !init(device)) return false;
        D3D11_TEXTURE2D_DESC bd;
        backBuffer->GetDesc(&bd);
        if (!isSrgb(bd.Format) || bd.SampleDesc.Count > 1) return false;
        if (layer && w == bd.Width && h == bd.Height && format == bd.Format) return true;
        releaseTargets();
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = bd.Width;
        d.Height = bd.Height;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        bool ok = SUCCEEDED(device->CreateTexture2D(&d, nullptr, &layer)) &&
                  SUCCEEDED(device->CreateRenderTargetView(layer, nullptr, &layerRtv)) &&
                  SUCCEEDED(device->CreateShaderResourceView(layer, nullptr, &layerSrv));
        d.Format = typelessOf(bd.Format);
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = bd.Format;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        ok = ok && SUCCEEDED(device->CreateTexture2D(&d, nullptr, &frame)) &&
             SUCCEEDED(device->CreateShaderResourceView(frame, &sv, &frameSrv));
        if (!ok) {
            TMVS_LOG("overlay: menu layer %ux%u (format %d) failed, the menu draws straight into the back buffer", bd.Width, bd.Height,
                     bd.Format);
            releaseTargets();
            failed = true;
            return false;
        }
        w = bd.Width;
        h = bd.Height;
        format = bd.Format;
        TMVS_LOG("overlay: sRGB back buffer (format %d): the menu draws into its own layer", bd.Format);
        return true;
    }

    // The menu layer over the frame (state saved by the caller).
    void composite(ID3D11DeviceContext* ctx, ID3D11Texture2D* backBuffer, ID3D11RenderTargetView* backBufferRtv) {
        ctx->CopyResource(frame, backBuffer);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->RSSetState(rs);
        ctx->RSSetScissorRects(0, nullptr);
        const float factor[4] = {};
        ctx->OMSetBlendState(blend, factor, 0xffffffff);
        ctx->OMSetDepthStencilState(dss, 0);
        ctx->OMSetRenderTargets(1, &backBufferRtv, nullptr);
        D3D11_VIEWPORT vp = {0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ID3D11ShaderResourceView* srvs[2] = {frameSrv, layerSrv};
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none[2] = {};
        ctx->PSSetShaderResources(0, 2, none);
    }
};
UiLayer g_layer;
std::mutex g_queueLock;
std::vector<Message> g_queue;

bool isMouse(UINT msg) {
    return msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST;
}

bool isKeyboard(UINT msg) {
    return (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || msg == WM_CHAR;
}

bool isHotkey(WPARAM w) {
    return w == VK_F6 || w == VK_F7 || w == VK_F8 || w == VK_F12;
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    // Photo mode: focus click, aperture wheel and Esc, unless the menu has the mouse or keyboard
    // (the game may read the mouse directly too: it still sees them there).
    if (!(g_open && ((isMouse(msg) && g_wantMouse) || (isKeyboard(msg) && g_wantKeyboard))) && photo::handleMessage(hwnd, msg, w, l))
        return 0;
    if (g_open && (isMouse(msg) || isKeyboard(msg) || msg == WM_SETFOCUS || msg == WM_KILLFOCUS)) {
        {
            std::lock_guard<std::mutex> lock(g_queueLock);
            if (g_queue.size() < 512) g_queue.push_back({msg, w, l});
        }
        // Keep clicks and typing in the menu away from the game (its window messages; the game may
        // also read the mouse directly).
        if ((isMouse(msg) && g_wantMouse) || (isKeyboard(msg) && g_wantKeyboard && !isHotkey(w))) return 0;
    }
    return CallWindowProcW(g_originalProc, hwnd, msg, w, l);
}

void toast(const std::string& text, ULONGLONG ms = 3000) {
    g_toast = text;
    g_toastUntil = GetTickCount64() + ms;
}

bool pressed(int key, bool& was) {
    const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
    const bool edge = down && !was;
    was = down;
    return edge;
}

// The same keys as in TMNF: F8 menu, F7 effects on/off, F6 photo mode, F12 capture (photo in
// photo mode).
void hotkeys() {
    static bool f6 = false, f7 = false, f8 = false, f12 = false;
    if (GetForegroundWindow() != g_window) return;
    Config& config = Config::get();
    if (pressed(VK_F7, f7)) {
        config.settings.enabled = !config.settings.enabled;
        config.markDirty();
        toast(config.settings.enabled ? "TM Vibrant Shaders: on" : "TM Vibrant Shaders: off (the game's own image)");
    }
    if (pressed(VK_F6, f6)) photo::toggle();
    if (pressed(VK_F12, f12)) {
        if (photo::active()) {
            photo::request();
        } else {
            capture::request();
            toast("Frame captured (Documents\\ManiaPlanet\\TMVS\\captures)");
        }
    }
    if (pressed(VK_F8, f8)) config.showOverlay = !config.showOverlay;
}

// What the ManiaPlanet build can do so far; the menu shows only these.
bool supports(const tmshaders::Field& f) {
    return Effects::supports(f.key);
}

const char* placedText(hooks::Placed p) {
    switch (p) {
    case hooks::Placed::BeforeHud: return "before the interface";
    case hooks::Placed::EndOfFrame: return "at the end of the frame (interface included)";
    case hooks::Placed::Fallback: return "at the end of the frame: interface start not found";
    default: return "nowhere";
    }
}

// ManiaPlanet's own options and what the mod found in the engine (Settings page).
void gamePanel() {
    Options& o = options::current();
    const hooks::Stats& st = hooks::stats();
    const ImVec4 good(0.45f, 0.85f, 0.45f, 1), warn(0.95f, 0.75f, 0.3f, 1), bad(0.95f, 0.4f, 0.35f, 1);
    bool changed = ImGui::Checkbox("HDR stage (the look on the engine's HDR image)", &o.hdrStage);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("On: the mod replaces the engine's tone map; bright lights and sky keep colour and detail.\n"
                          "Off: the engine's tone map stays and the look goes on its finished image.");
    int placement = static_cast<int>(o.placement);
    changed |= ImGui::RadioButton("Before the interface", &placement, 0);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("End of frame (interface too)", &placement, 1);
    o.placement = static_cast<Placement>(placement);
    changed |= ImGui::Checkbox("End of frame when the interface start isn't found", &o.fallbackAtEnd);
    if (changed) options::save();
    int view = static_cast<int>(o.view);
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth());
    if (ImGui::Combo("View", &view,
                     "The mod\0Compare with the game (split)\0Check: depth\0Check: normals\0Check: motion\0Check: albedo\0"
                     "Check: world grid\0"))
        o.view = static_cast<View>(view);
    if (o.view == View::Split) {
        ImGui::SetNextItemWidth(ImGui::CalcItemWidth());
        ImGui::SliderFloat("Split", &o.split, 0.0f, 1.0f);
    }
    if (ImGui::Button("Capture this frame (F12)")) capture::request();
    if (!capture::lastFolder().empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Open capture")) ShellExecuteW(nullptr, L"open", capture::lastFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::Separator();
    if (st.toneMapSeen && st.toneMapped) ImGui::TextColored(good, "HDR stage replaces the engine's tone map");
    else if (st.toneMapSeen) ImGui::TextColored(good, "Scene found (the engine's tone map)");
    else ImGui::TextColored(warn, "No scene in this frame (menu)");
    const ImVec4 placedColour = st.placed == hooks::Placed::BeforeHud || st.placed == hooks::Placed::EndOfFrame ? good
                                : st.placed == hooks::Placed::Fallback                                         ? warn
                                                                                                              : bad;
    ImGui::TextColored(placedColour, "Finish %s", placedText(st.placed));
    if (st.cameraFound) ImGui::TextColored(good, "Camera found (eye %.0f %.0f %.0f)", st.eye[0], st.eye[1], st.eye[2]);
    else ImGui::TextDisabled("Camera not found in this frame");
    std::string found;
    for (int i = 0; i < kTexCount; i++) {
        if (!(st.textures & (1u << i))) continue;
        if (!found.empty()) found += ", ";
        found += engineTextureName(i);
    }
    ImGui::TextDisabled("Engine textures: %s", found.empty() ? "none" : found.c_str());
    ImGui::TextDisabled("Shaders recognised: %u of %u, %u draws, %ux%u", st.shadersKnown, st.shadersCreated, st.drawsLastFrame,
                        st.width, st.height);
}

} // namespace

void init(HWND window, ID3D11Device* device, ID3D11DeviceContext* ctx) {
    if (g_ready || !window) return;
    g_window = window;
    g_device = device;
    g_ctx = ctx;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    tmshaders::menu::setupStyle(window);
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(device, ctx);
    g_originalProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&windowProc)));
    g_ready = true;
    toast("TM Vibrant Shaders " TMVS_MP_VERSION " is running - F8 menu, F7 on/off", 8000);
    TMVS_LOG("overlay: ready on window %p", static_cast<void*>(window));
}

void frame(ID3D11RenderTargetView* backBuffer) {
    if (!g_ready || !backBuffer) return;
    Config& config = Config::get();
    config.tick(); // saves settings.ini a moment after a change
    hotkeys();
    g_open = config.showOverlay;
    const bool showToast = GetTickCount64() < g_toastUntil;
    const char* photoLine = photo::status();
    if (!config.showOverlay) config.previewPreset.clear();
    if (!config.showOverlay && !showToast && !*photoLine) return;

    std::vector<Message> messages;
    {
        std::lock_guard<std::mutex> lock(g_queueLock);
        messages.swap(g_queue);
    }
    for (const auto& m : messages) ImGui_ImplWin32_WndProcHandler(g_window, m.msg, m.w, m.l);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    // ImGui's pointer over the menu (the game hides the OS cursor), the game's elsewhere.
    io.MouseDrawCursor = config.showOverlay && io.WantCaptureMouse;
    ImGui::NewFrame();
    if (config.showOverlay) {
        // The effects' GPU times for the Performance page (as TMNF's pipeline reports them).
        struct Timings : tmshaders::menu::PassTimings {
            int count() const override { return kPassCount; }
            const char* name(int pass) const override { return passName(pass); }
            float time(int pass) const override {
                const Effects* fx = hooks::effects();
                return fx ? fx->profiler().time(pass) : 0.0f;
            }
            float total() const override {
                const Effects* fx = hooks::effects();
                return fx ? fx->profiler().total() : 0.0f;
            }
        };
        static Timings timings;
        static tmshaders::menu::Host host = [] {
            tmshaders::menu::Host h;
            h.picture = [](const std::string& preset) {
                return reinterpret_cast<ImTextureID>(thumbnails::get(g_device, preset));
            };
            h.supports = supports;
            h.timings = &timings;
            h.autoQualityReduced = [] { return hooks::autoQualityReduced(); };
            // The scene effects' debug views (scene.hlsl PS_Lighting; the stages use 20-49).
            h.debugViews = {{0, "Final image"},
                            {1, "Depth"},
                            {2, "Normals (world)"},
                            {3, "Ambient occlusion"},
                            {4, "Sun shadows (engine + contact)"},
                            {9, "Engine light: sun"},
                            {10, "Engine light: ambient"},
                            {11, "Engine light: the rest"},
                            {12, "Light sources (lamps, neon)"}};
            h.fNumber = photo::fNumber;
            h.gamePanelTitle = "ManiaPlanet";
            h.gamePanel = gamePanel;
            return h;
        }();
        tmshaders::menu::draw(host);
    }
    if (*photoLine) {
        // Photo mode line, top left. Drawn at Present: never in the photos.
        ImGui::SetNextWindowPos(ImVec2(16.0f * tmshaders::menu::scale(), 16.0f * tmshaders::menu::scale()));
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::Begin("##photo", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted(photoLine);
        ImGui::End();
    }
    if (showToast) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 40), ImGuiCond_Always, ImVec2(0.5f, 0));
        ImGui::SetNextWindowBgAlpha(0.8f);
        ImGui::Begin("##toast", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::TextUnformatted(g_toast.c_str());
        ImGui::End();
    }
    ImGui::Render();
    g_wantMouse = io.WantCaptureMouse;
    g_wantKeyboard = io.WantCaptureKeyboard;

    StateBackup backup;
    backup.save(g_ctx);
    ID3D11Resource* res = nullptr;
    backBuffer->GetResource(&res);
    ID3D11Texture2D* bb = nullptr;
    if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb));
    if (res) res->Release();
    if (bb && g_layer.ensure(g_device, bb)) {
        const float clear[4] = {};
        g_ctx->ClearRenderTargetView(g_layer.layerRtv, clear);
        g_ctx->OMSetRenderTargets(1, &g_layer.layerRtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_layer.composite(g_ctx, bb, backBuffer);
    } else {
        g_ctx->OMSetRenderTargets(1, &backBuffer, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    if (bb) bb->Release();
    backup.restore(g_ctx);
}

void releaseTargets() {
    g_layer.releaseTargets();
}

} // namespace overlay
} // namespace tmvs_mp
