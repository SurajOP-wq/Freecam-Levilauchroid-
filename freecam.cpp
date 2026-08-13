#include <jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <dlfcn.h>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <sys/mman.h>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

// Gloss и Mod хедеры
#include "pl/Gloss.h"
#include "pl/legacy/LegacyMod.h"
#include "pl/legacy/LegacyInput.h"

#define TAG "FreeCam"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// ── состояние ───────────────────────────────────────────────────────────────
static bool  g_active  = false;
static float g_camX=0, g_camY=0, g_camZ=0;
static float g_camYaw=0, g_camPitch=0;
static float g_speed = 0.3f;
static int   g_screenW=1080, g_screenH=2400;
static float g_joyX=0, g_joyY=0;
static float g_lastTX=-1, g_lastTY=-1;
static bool  g_rotating=false;
static bool  g_initialized=false;

// Буфер для накопления введённых символов в чате
static std::string g_typedBuffer = "";

// ── оригинальные функции ────────────────────────────────────────────────────
static void (*g_orig_getCamPos)(void*,float*) = nullptr;
static void (*g_orig_getCamRot)(void*,float*) = nullptr;
static int  (*g_orig_getPerspective)(void*)   = nullptr;
static EGLBoolean (*g_orig_swap)(EGLDisplay,EGLSurface) = nullptr;

// ── хуки камеры ──────────────────────────────────────────────────────────────
static void hook_getCamPos(void* s, float* o) {
    if (!o) return;
    if (g_active) { o[0]=g_camX; o[1]=g_camY; o[2]=g_camZ; return; }
    if (g_orig_getCamPos) {
        g_orig_getCamPos(s, o);
        g_camX = o[0];
        g_camY = o[1];
        g_camZ = o[2];
    }
}

static void hook_getCamRot(void* s, float* o) {
    if (!o) return;
    if (g_active) { o[0]=g_camYaw; o[1]=g_camPitch; return; }
    if (g_orig_getCamRot) {
        g_orig_getCamRot(s, o);
        g_camYaw = o[0];
        g_camPitch = o[1];
    }
}

static int hook_getPerspective(void* s) {
    if (g_active) return 0;
    return g_orig_getPerspective ? g_orig_getPerspective(s) : 0;
}

// ── поиск vtable ────────────────────────────────────────────────────────────
struct Segment {
    uintptr_t start;
    uintptr_t end;
    bool exec;
};

static std::vector<Segment> getMinecraftSegments() {
    std::vector<Segment> segments;
    std::ifstream m("/proc/self/maps");
    std::string line;
    while (std::getline(m, line)) {
        if (line.find("minecraft") == std::string::npos) continue;
        uintptr_t s = 0, e = 0;
        char perm[16] = {0};
        if (sscanf(line.c_str(), "%lx-%lx %15s", &s, &e, perm) != 3) continue;
        if (perm[0] != 'r') continue;
        segments.push_back({s, e, perm[2] == 'x'});
    }
    return segments;
}

static uintptr_t findTypeName(const std::vector<Segment>& segments, const char* name) {
    size_t len = strlen(name);
    for (const auto& seg : segments) {
        if (seg.exec) continue; // Skip executable code segments to avoid literal pools
        for (uintptr_t a = seg.start; a + len <= seg.end; a++) {
            if (!memcmp((void*)a, name, len + 1)) {
                return a;
            }
        }
    }
    return 0;
}

static uintptr_t findTypeInfo(const std::vector<Segment>& segments, uintptr_t nameAddr) {
    for (const auto& seg : segments) {
        if (seg.exec) continue;
        uintptr_t startAligned = (seg.start + 7) & ~7UL;
        for (uintptr_t a = startAligned; a + 8 <= seg.end; a += 8) {
            if (*(uintptr_t*)a == nameAddr) {
                return a - 8;
            }
        }
    }
    return 0;
}

static uintptr_t findVtableFromTypeInfo(const std::vector<Segment>& segments, uintptr_t tiAddr) {
    for (const auto& seg : segments) {
        if (seg.exec) continue;
        uintptr_t startAligned = (seg.start + 7) & ~7UL;
        for (uintptr_t a = startAligned + 8; a + 8 <= seg.end; a += 8) {
            if (*(uintptr_t*)a == tiAddr) {
                if (*(uintptr_t*)(a - 8) == 0) { // Verify offset-to-top is 0
                    return a + 8;
                }
            }
        }
    }
    return 0;
}

static uintptr_t findVtable(const char* name) {
    auto segments = getMinecraftSegments();
    if (segments.empty()) return 0;

    uintptr_t nameAddr = findTypeName(segments, name);
    if (!nameAddr) {
        LOGE("Typeinfo name %s not found", name);
        return 0;
    }

    uintptr_t tiAddr = findTypeInfo(segments, nameAddr);
    if (!tiAddr) {
        LOGE("Typeinfo structure for %s not found", name);
        return 0;
    }

    uintptr_t vt = findVtableFromTypeInfo(segments, tiAddr);
    if (!vt) {
        LOGE("Vtable for %s not found", name);
        return 0;
    }

    LOGI("Vtable for %s found successfully at %p", name, (void*)vt);
    return vt;
}

static bool patchSlot(uintptr_t vt, int slot, void* hook, void** orig) {
    if (!vt) return false;
    uintptr_t* p = (uintptr_t*)(vt + slot * 8);
    *orig = (void*)(*p);

    // Получаем реальный размер страницы памяти (поддерживает 4KB, 16KB, 64KB на новых Android)
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) pageSize = 4096;

    uintptr_t pg = (uintptr_t)p & ~(pageSize - 1);
    if (mprotect((void*)pg, pageSize, PROT_READ | PROT_WRITE) != 0) {
        LOGE("mprotect failed slot %d", slot);
        return false;
    }
    *p = (uintptr_t)hook;
    mprotect((void*)pg, pageSize, PROT_READ);
    LOGI("slot %d hooked", slot);
    return true;
}

// ── тач управление ──────────────────────────────────────────────────────────
static inline bool inButton(float x, float y) {
    // кнопка в правом верхнем углу 80x80
    return x >= (g_screenW - 90) && x <= g_screenW && y >= 10 && y <= 90;
}

static inline bool inJoy(float x, float y) {
    // джойстик в левом нижнем углу, круг R=80
    float dx = x - 90.f, dy = y - (g_screenH - 90.f);
    return (dx*dx + dy*dy) <= 80.f*80.f;
}

static bool onTouch(int action, int /*pointerId*/, float x, float y) {
    // action: 0=DOWN, 1=UP, 2=MOVE
    if (action == 0) {
        if (inButton(x, y)) {
            g_active = !g_active;
            if (!g_active) { g_joyX = 0; g_joyY = 0; g_rotating = false; }
            LOGI("FreeCam %s", g_active ? "ON" : "OFF");
            return true;
        }
        if (g_active) {
            if (inJoy(x, y)) {
                g_joyX = 0; g_joyY = 0;
            } else {
                g_lastTX = x; g_lastTY = y; g_rotating = true;
            }
            return true; // поглощаем тач при активном фрикаме
        }
    }
    if (action == 2 && g_active) {
        if (g_rotating && g_lastTX >= 0) {
            g_camYaw   += (x - g_lastTX) * 0.2f;
            g_camPitch += (y - g_lastTY) * 0.2f;
            if (g_camPitch >  90.f) g_camPitch =  90.f;
            if (g_camPitch < -90.f) g_camPitch = -90.f;
            g_lastTX = x; g_lastTY = y;
        }
        if (inJoy(x, y)) {
            g_joyX = (x - 90.f) / 80.f;
            g_joyY = (y - (g_screenH - 90.f)) / 80.f;
        }
        return true; // поглощаем тач при активном фрикаме
    }
    if (action == 1) {
        if (g_active) {
            g_rotating = false; g_lastTX = -1;
            if (!inJoy(x, y)) { g_joyX = 0; g_joyY = 0; }
            return true; // поглощаем тач при активном фрикаме
        }
    }
    return false;
}

// ── ввод текста (чат активация "fc") ──────────────────────────────────────────
static bool onTextInput(const char* text, size_t length) {
    if (!text) return false;

    // Накапливаем символы для поддержки посимвольного ввода (Android IME)
    for (size_t i = 0; i < length; ++i) {
        char c = text[i];
        if (c >= 32 && c <= 126) {
            g_typedBuffer += c;
        } else if (c == '\n' || c == '\r') {
            g_typedBuffer.clear(); // очищаем при переходе на новую строку
        }
    }

    // Ограничиваем размер буфера, чтобы избежать переполнения
    if (g_typedBuffer.size() > 50) {
        g_typedBuffer = g_typedBuffer.substr(g_typedBuffer.size() - 50);
    }

    // Очищаем отступы и пробелы в конце, чтобы поддерживать автоисправление/пробелы клавиатур
    std::string clean = g_typedBuffer;
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '\n' || clean.back() == '\r' || clean.back() == '\t')) {
        clean.pop_back();
    }

    // Проверяем, оканчивается ли буфер ввода на "fc" или "FC" как отдельное слово/команду
    if (clean.size() >= 2) {
        std::string lastTwo = clean.substr(clean.size() - 2);
        if (lastTwo == "fc" || lastTwo == "FC") {
            // Проверяем, что перед "fc" идет пробел, косая черта, или это начало строки (чтобы избежать ложных срабатываний на "kfc")
            bool match = false;
            if (clean.size() == 2) {
                match = true;
            } else {
                char prevChar = clean[clean.size() - 3];
                if (prevChar == ' ' || prevChar == '/' || prevChar == '.' || prevChar == '!') {
                    match = true;
                }
            }

            if (match) {
                g_active = !g_active;
                if (!g_active) { g_joyX = 0; g_joyY = 0; g_rotating = false; }
                LOGI("FreeCam %s via text input", g_active ? "ON" : "OFF");
                g_typedBuffer.clear(); // очищаем буфер, чтобы избежать двойного срабатывания
                return false; // возвращаем false, чтобы символы продолжали отображаться в чате
            }
        }
    }

    return false;
}

// ── EGL хук для тика движения ──────────────────────────────────────────────
static EGLBoolean hook_swap(EGLDisplay dpy, EGLSurface surf) {
    if (g_active) {
        // обновляем размер экрана
        EGLint w = 0, h = 0;
        eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
        eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
        if (w > 0) g_screenW = w;
        if (h > 0) g_screenH = h;

        // 3D движение камеры по джойстику
        if (g_joyX != 0.f || g_joyY != 0.f) {
            float yr = g_camYaw * (float)M_PI / 180.f;
            float pr = g_camPitch * (float)M_PI / 180.f;

            // Вектор направления вперед:
            float fx = -sinf(yr) * cosf(pr);
            float fy = -sinf(pr);
            float fz = cosf(yr) * cosf(pr);

            // Вектор направления вправо:
            float rx = cosf(yr);
            float rz = sinf(yr);

            float move_forward = -g_joyY; // joyY равен -1 при движении вперед
            float move_right = g_joyX;

            g_camX += (move_forward * fx + move_right * rx) * g_speed;
            g_camY += (move_forward * fy) * g_speed;
            g_camZ += (move_forward * fz + move_right * rz) * g_speed;
        }
    }
    return g_orig_swap ? g_orig_swap(dpy, surf) : EGL_FALSE;
}

// ── точка входа мода ───────────────────────────────────────────────────────
extern "C" __attribute__((visibility("default")))
void LeviMod_Load(JavaVM* /*vm*/, const PLModInfo* info) {
    LOGI("FreeCam mod loading... id=%s", info ? info->mod_id : "?");

    if (g_initialized) { LOGI("Already initialized"); return; }

    GlossInit(true);

    // Выполняем хук камеры на главном потоке во время загрузки (предотвращает SIGSYS краши на потоке рендеринга)
    uintptr_t vt = findVtable("16VanillaCameraAPI");
    if (!vt) vt = findVtable("13VanillaCamera");
    if (!vt) vt = findVtable("6Camera");
    if (!vt) vt = findVtable("12CameraSystem");
    if (!vt) vt = findVtable("12RenderCamera");

    if (vt) {
        patchSlot(vt, 7, (void*)hook_getPerspective, (void**)&g_orig_getPerspective);
        patchSlot(vt, 8, (void*)hook_getCamPos,      (void**)&g_orig_getCamPos);
        patchSlot(vt, 9, (void*)hook_getCamRot,      (void**)&g_orig_getCamRot);
        LOGI("Camera hooked successfully!");
    } else {
        LOGE("No camera vtables found (checked VanillaCameraAPI, VanillaCamera, Camera, CameraSystem, RenderCamera) — FreeCam will not work!");
    }

    // хук eglSwapBuffers для тика движения
    void* libEGL = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    if (libEGL) {
        void* swapSym = dlsym(libEGL, "eglSwapBuffers");
        if (swapSym) {
            GlossHook(swapSym, (void*)hook_swap, (void**)&g_orig_swap);
            LOGI("eglSwapBuffers hooked");
        } else {
            LOGE("eglSwapBuffers not found");
        }
    } else {
        LOGE("libEGL.so not found");
    }

    // регистрация колбэков через GetPreloaderInput с поиском dlsym в RTLD_DEFAULT
    typedef PreloaderInput_Interface* (*GetPI_fn)();
    GetPI_fn getPI = (GetPI_fn)dlsym(RTLD_DEFAULT, "GetPreloaderInput");
    if (getPI) {
        auto* input = getPI();
        if (input) {
            if (input->RegisterTouchCallback) {
                input->RegisterTouchCallback(onTouch);
                LOGI("Touch callback registered");
            } else {
                LOGE("RegisterTouchCallback not available");
            }

            if (input->RegisterTextInputCallback) {
                input->RegisterTextInputCallback(onTextInput);
                LOGI("TextInput callback registered");
            } else {
                LOGE("RegisterTextInputCallback not available");
            }
        } else {
            LOGE("GetPreloaderInput returned null");
        }
    } else {
        LOGE("GetPreloaderInput not found via dlsym");
    }

    g_initialized = true;
    LOGI("FreeCam ready! Tap top-right corner or type 'fc' in chat to toggle.");
}
