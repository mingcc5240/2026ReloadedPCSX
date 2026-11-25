#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <gl/gl.h>
#include <stdint.h>
#include <string.h>

#define CALLBACK_API __stdcall

#define VRAM_W 1024
#define VRAM_H 512

// OpenGL / Win32 핸들
static HWND   g_hwnd = NULL;
static HDC    g_hdc = NULL;
static HGLRC  g_hrc = NULL;
static GLuint g_vram_tex = 0;

// PS1 1MB VRAM (1024x512, 16-bit 0BBBBBGGGGGRRRRR)
static uint16_t g_vram[VRAM_H][VRAM_W];
// OpenGL 업로드용 32-bit RGBA 버퍼
static uint32_t g_rgba_buf[VRAM_W * VRAM_H];
static int      g_vram_dirty = 1;

// GPU 상태 및 디스플레이 설정 (GP1)
static uint32_t g_gpu_stat = 0x14802000;
static int g_disp_disabled = 0; // 1이면 화면 끔 (검은 화면)
static int g_disp_x = 0;        // VRAM 내 디스플레이 시작 X
static int g_disp_y = 0;        // VRAM 내 디스플레이 시작 Y
static int g_disp_w = 320;      // 현재 수평 해상도
static int g_disp_h = 240;      // 현재 수직 해상도
static int g_is_24bit = 0;      // 24비트 컬러 모드 여부

// ------------------------------------------------------------
// 보조 함수: RGB555 -> RGBA8888 변환 및 테스트 패턴 생성
// ------------------------------------------------------------
static inline uint32_t rgb555_to_rgba8888(uint16_t c) {
    uint32_t r = ((c >> 0) & 0x1F) << 3;
    uint32_t g = ((c >> 5) & 0x1F) << 3;
    uint32_t b = ((c >> 10) & 0x1F) << 3;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

static void FillVramTestPattern(void) {
    // 2단계 검증용 컬러 바 패턴 (VRAM 전체에 채움)
    static const uint16_t bars[8] = {
        0x7FFF, // White
        0x03FF, // Yellow
        0x7FE0, // Cyan
        0x03E0, // Green
        0x7C1F, // Magenta
        0x001F, // Red
        0x7C00, // Blue
        0x2108  // Dark Gray
    };
    for (int y = 0; y < VRAM_H; y++) {
        for (int x = 0; x < VRAM_W; x++) {
            int idx = (x / 40) & 7;
            // 격자선 추가해 해상도/비율 확인 용이하게 함
            if ((x % 32) == 0 || (y % 32) == 0) {
                g_vram[y][x] = 0x0000;
            }
            else {
                g_vram[y][x] = bars[idx];
            }
        }
    }
    g_vram_dirty = 1;
}

static void UploadVramTexture(void) {
    if (!g_vram_dirty || !g_vram_tex) return;

    const uint16_t* src = &g_vram[0][0];
    uint32_t* dst = g_rgba_buf;
    for (int i = 0; i < VRAM_W * VRAM_H; i++) {
        dst[i] = rgb555_to_rgba8888(src[i]);
    }

    glBindTexture(GL_TEXTURE_2D, g_vram_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VRAM_W, VRAM_H,
        GL_RGBA, GL_UNSIGNED_BYTE, g_rgba_buf);
    g_vram_dirty = 0;
}

// ------------------------------------------------------------
// 1. PSEmu Pro 메타데이터
// ------------------------------------------------------------
uint32_t CALLBACK_API PSEgetLibType(void) { return 2; }
char* CALLBACK_API PSEgetLibName(void) { return "MiniGL PSX GPU Plugin (Step 2)"; }
uint32_t CALLBACK_API PSEgetLibVersion(void) { return (1 << 16) | (0 << 8) | 2; }

// ------------------------------------------------------------
// 2. 초기화 / 종료 / 설정
// ------------------------------------------------------------
long CALLBACK_API GPUinit(void) {
    FillVramTestPattern();
    return 0;
}

long CALLBACK_API GPUshutdown(void) { return 0; }
long CALLBACK_API GPUtest(void) { return 0; }

long CALLBACK_API GPUconfigure(void) {
    MessageBoxA(NULL, "MiniGL GPU Plugin - Step 2 (VRAM Texture + GP1)", "Configure", MB_OK);
    return 0;
}

void CALLBACK_API GPUabout(void) {
    MessageBoxA(NULL, "MiniGL PSX OpenGL Plugin\nStep 2: 1024x512 VRAM & GP1", "About", MB_OK);
}

// ------------------------------------------------------------
// 3. OpenGL 컨텍스트 및 1024x512 텍스처 생성
// ------------------------------------------------------------
long CALLBACK_API GPUopen(HWND hwnd) {
    g_hwnd = hwnd;
    g_hdc = GetDC(g_hwnd);
    if (!g_hdc) return -1;

    PIXELFORMATDESCRIPTOR pfd;
    ZeroMemory(&pfd, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 16;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int pf = ChoosePixelFormat(g_hdc, &pfd);
    if (!pf || !SetPixelFormat(g_hdc, pf, &pfd)) return -1;

    g_hrc = wglCreateContext(g_hdc);
    if (!g_hrc || !wglMakeCurrent(g_hdc, g_hrc)) return -1;

    // 1024x512 OpenGL 텍스처 생성
    glGenTextures(1, &g_vram_tex);
    glBindTexture(GL_TEXTURE_2D, g_vram_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, VRAM_W, VRAM_H, 0,
        GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    g_vram_dirty = 1;
    return 0;
}

long CALLBACK_API GPUclose(void) {
    if (g_hrc) {
        if (g_vram_tex) {
            glDeleteTextures(1, &g_vram_tex);
            g_vram_tex = 0;
        }
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(g_hrc);
        g_hrc = NULL;
    }
    if (g_hdc && g_hwnd) {
        ReleaseDC(g_hwnd, g_hdc);
        g_hdc = NULL;
    }
    return 0;
}

// ------------------------------------------------------------
// 4. 매 프레임 VRAM 디스플레이 영역을 화면에 출력
// ------------------------------------------------------------
void CALLBACK_API GPUupdateLace(void) {
    if (!g_hdc || !g_hrc) return;

    // 창 크기 변경에 맞춰 뷰포트 갱신
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int win_w = (rc.right - rc.left) > 0 ? (rc.right - rc.left) : 640;
    int win_h = (rc.bottom - rc.top) > 0 ? (rc.bottom - rc.top) : 480;

    glViewport(0, 0, win_w, win_h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, 1.0, 1.0, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!g_disp_disabled && g_vram_tex) {
        UploadVramTexture();

        float u0 = (float)g_disp_x / (float)VRAM_W;
        float v0 = (float)g_disp_y / (float)VRAM_H;
        float u1 = (float)(g_disp_x + g_disp_w) / (float)VRAM_W;
        float v1 = (float)(g_disp_y + g_disp_h) / (float)VRAM_H;

        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, g_vram_tex);
        glColor3f(1.0f, 1.0f, 1.0f);

        glBegin(GL_QUADS);
        glTexCoord2f(u0, v0); glVertex2f(0.0f, 0.0f);
        glTexCoord2f(u1, v0); glVertex2f(1.0f, 0.0f);
        glTexCoord2f(u1, v1); glVertex2f(1.0f, 1.0f);
        glTexCoord2f(u0, v1); glVertex2f(0.0f, 1.0f);
        glEnd();

        glDisable(GL_TEXTURE_2D);
    }

    SwapBuffers(g_hdc);
}

// ------------------------------------------------------------
// 5. GP1 제어 명령 처리 (GPUwriteStatus)
// ------------------------------------------------------------
void CALLBACK_API GPUwriteStatus(uint32_t gdata) {
    uint32_t cmd = (gdata >> 24) & 0xFF;

    switch (cmd) {
    case 0x00: // Reset GPU
        g_gpu_stat = 0x14802000;
        g_disp_disabled = 1;
        g_disp_x = 0;
        g_disp_y = 0;
        g_disp_w = 320;
        g_disp_h = 240;
        g_is_24bit = 0;
        break;

    case 0x03: // Display Enable (0=On, 1=Off)
        g_disp_disabled = (gdata & 1);
        g_gpu_stat = (g_gpu_stat & ~(1u << 23)) | ((gdata & 1) << 23);
        break;

    case 0x05: // Start of Display Area in VRAM
        g_disp_x = gdata & 0x3FE;
        g_disp_y = (gdata >> 10) & 0x1FF;
        break;

    case 0x08: { // Display Mode
        static const int hres_table[4] = { 256, 320, 512, 640 };
        int hr2 = (gdata >> 6) & 1;
        int hr1 = gdata & 3;
        g_disp_w = hr2 ? 368 : hres_table[hr1];

        int vres = (gdata >> 2) & 1;
        int interlace = (gdata >> 5) & 1;
        g_disp_h = (vres && interlace) ? 480 : 240;

        g_is_24bit = (gdata >> 4) & 1;
        break;
    }

    default:
        break;
    }
}

uint32_t CALLBACK_API GPUreadStatus(void) {
    // 인터레이스 토글 비트(bit 31) 및 Ready 비트 유지
    g_gpu_stat ^= 0x80000000u;
    return g_gpu_stat | 0x1C000000u;
}

// ------------------------------------------------------------
// 6. GP0 / DMA 스텁 (3단계에서 구현)
// ------------------------------------------------------------
void CALLBACK_API GPUwriteData(uint32_t gdata) { (void)gdata; }
void CALLBACK_API GPUwriteDataMem(uint32_t* pMem, int iSize) { (void)pMem; (void)iSize; }
uint32_t CALLBACK_API GPUreadData(void) { return 0; }
void CALLBACK_API GPUreadDataMem(uint32_t* pMem, int iSize) {
    if (pMem && iSize > 0) memset(pMem, 0, iSize * sizeof(uint32_t));
}
long CALLBACK_API GPUdmaChain(uint32_t* baseAddrL, uint32_t addr) {
    (void)baseAddrL; (void)addr;
    return 0;
}
long CALLBACK_API GPUfreeze(uint32_t ulGetFreezeData, void* pF) {
    (void)ulGetFreezeData; (void)pF;
    return 1;
}