#include "tracer.h"
#include "log.h"
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace tmshaders {
namespace tracer {
namespace {

int g_delay = -1;     // frames until tracing starts, -1 = disarmed
bool g_active = false;
int g_traceIndex = 0;
std::vector<std::string> g_lines;
char g_describe[4][160];
int g_describeSlot = 0;

char* nextBuffer() {
    g_describeSlot = (g_describeSlot + 1) & 3;
    return g_describe[g_describeSlot];
}

void flush() {
    wchar_t name[64];
    swprintf(name, 64, L"\\trace_%d.txt", g_traceIndex++);
    FILE* f = _wfopen((log::dataDir() + name).c_str(), L"w");
    if (f) {
        for (const std::string& line : g_lines) {
            fputs(line.c_str(), f);
            fputc('\n', f);
        }
        fclose(f);
    }
    TMVS_LOG("tracer: wrote %u events", static_cast<unsigned>(g_lines.size()));
    g_lines.clear();
}

} // namespace

void arm(int delayFrames) {
    if (g_delay < 0 && !g_active) g_delay = delayFrames;
}

bool tracing() {
    return g_active;
}

void onPresent(IDirect3DDevice9*) {
    if (g_active) {
        g_active = false;
        flush();
    }
    if (g_delay == 0) {
        g_delay = -1;
        g_active = true;
        g_lines.reserve(20000);
        event("==== frame start ====");
    } else if (g_delay > 0) {
        g_delay--;
    }
}

void event(const char* fmt, ...) {
    if (!g_active) return;
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    g_lines.emplace_back(buffer);
}

const char* formatName(D3DFORMAT format) {
    switch (static_cast<DWORD>(format)) {
        case D3DFMT_UNKNOWN: return "UNKNOWN";
        case D3DFMT_A8R8G8B8: return "A8R8G8B8";
        case D3DFMT_X8R8G8B8: return "X8R8G8B8";
        case D3DFMT_R5G6B5: return "R5G6B5";
        case D3DFMT_A2R10G10B10: return "A2R10G10B10";
        case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
        case D3DFMT_A32B32G32R32F: return "A32B32G32R32F";
        case D3DFMT_R16F: return "R16F";
        case D3DFMT_R32F: return "R32F";
        case D3DFMT_G16R16F: return "G16R16F";
        case D3DFMT_G32R32F: return "G32R32F";
        case D3DFMT_D16: return "D16";
        case D3DFMT_D24S8: return "D24S8";
        case D3DFMT_D24X8: return "D24X8";
        case D3DFMT_D32: return "D32";
        case D3DFMT_D24FS8: return "D24FS8";
        case D3DFMT_DXT1: return "DXT1";
        case D3DFMT_DXT3: return "DXT3";
        case D3DFMT_DXT5: return "DXT5";
        case D3DFMT_A8: return "A8";
        case D3DFMT_L8: return "L8";
        case D3DFMT_A8L8: return "A8L8";
        case MAKEFOURCC('I', 'N', 'T', 'Z'): return "INTZ";
        case MAKEFOURCC('D', 'F', '2', '4'): return "DF24";
        case MAKEFOURCC('D', 'F', '1', '6'): return "DF16";
        case MAKEFOURCC('R', 'A', 'W', 'Z'): return "RAWZ";
        case MAKEFOURCC('N', 'U', 'L', 'L'): return "NULL";
        default: {
            char* b = nextBuffer();
            snprintf(b, 160, "fmt%u", static_cast<unsigned>(format));
            return b;
        }
    }
}

const char* describe(IDirect3DSurface9* surface) {
    char* b = nextBuffer();
    if (!surface) {
        snprintf(b, 160, "null");
        return b;
    }
    D3DSURFACE_DESC desc{};
    surface->GetDesc(&desc);
    IDirect3DTexture9* parent = nullptr;
    bool isTexture = SUCCEEDED(surface->GetContainer(IID_IDirect3DTexture9, reinterpret_cast<void**>(&parent))) && parent;
    snprintf(b, 160, "%p[%ux%u %s ms%d%s%s]", static_cast<void*>(surface), desc.Width, desc.Height, formatName(desc.Format),
             static_cast<int>(desc.MultiSampleType), isTexture ? " tex" : "",
             (desc.Usage & D3DUSAGE_DEPTHSTENCIL) ? " ds" : "");
    if (parent) parent->Release();
    return b;
}

const char* describe(IDirect3DBaseTexture9* texture) {
    char* b = nextBuffer();
    if (!texture) {
        snprintf(b, 160, "null");
        return b;
    }
    if (texture->GetType() == D3DRTYPE_TEXTURE) {
        D3DSURFACE_DESC desc{};
        static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0, &desc);
        snprintf(b, 160, "%p[%ux%u %s%s%s]", static_cast<void*>(texture), desc.Width, desc.Height, formatName(desc.Format),
                 (desc.Usage & D3DUSAGE_RENDERTARGET) ? " rt" : "", (desc.Usage & D3DUSAGE_DEPTHSTENCIL) ? " ds" : "");
    } else {
        snprintf(b, 160, "%p[type%d]", static_cast<void*>(texture), static_cast<int>(texture->GetType()));
    }
    return b;
}

} // namespace tracer
} // namespace tmshaders
