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
static uint32_t g_rgba_buf[VRAM_W * VRAM_H];
static int      g_vram_dirty = 1;

// GP1 상태 및 디스플레이 설정
static uint32_t g_gpu_stat = 0x14802000;
static int g_disp_disabled = 1;
static int g_disp_x = 0, g_disp_y = 0;
static int g_disp_w = 320, g_disp_h = 240;
static int g_is_24bit = 0;

// GP0 그리기 환경 (4단계 폴리곤 렌더링용)
static int g_draw_x1 = 0, g_draw_y1 = 0;
static int g_draw_x2 = 1023, g_draw_y2 = 511;
static int g_draw_off_x = 0, g_draw_off_y = 0;

// GP0 명령 파서 및 VRAM 전송 상태
static uint32_t g_cmd_buf[16];
static int      g_cmd_idx = 0;
static int      g_cmd_len = 0;
static int      g_in_polyline = 0;

// CPU <-> VRAM 전송 상태 (0: 없음, 1: CPU->VRAM, 2: VRAM->CPU)
static int g_xfer_mode = 0;
static int g_xfer_x = 0, g_xfer_y = 0;
static int g_xfer_w = 0, g_xfer_h = 0;
static int g_xfer_cur_x = 0, g_xfer_cur_y = 0;

// ------------------------------------------------------------
// 보조 함수: RGB555 -> RGBA8888 변환 및 텍스처 업로드
// ------------------------------------------------------------
static inline uint32_t rgb555_to_rgba8888(uint16_t c) {
    uint32_t r = ((c >> 0) & 0x1F) << 3;
    uint32_t g = ((c >> 5) & 0x1F) << 3;
    uint32_t b = ((c >> 10) & 0x1F) << 3;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

static inline uint16_t rgb888_to_rgb555(uint32_t rgb) {
    uint16_t r = (uint16_t)((rgb >> 3) & 0x1F);
    uint16_t g = (uint16_t)((rgb >> 11) & 0x1F);
    uint16_t b = (uint16_t)((rgb >> 19) & 0x1F);
    return (b << 10) | (g << 5) | r;
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
// GP0 명령 길이 테이블 및 실행기
// ------------------------------------------------------------
static int GetGP0CommandLength(uint8_t cmd) {
    if (cmd == 0x02) return 3; // Fill Rect
    if (cmd >= 0x20 && cmd <= 0x23) return 4;
    if (cmd >= 0x24 && cmd <= 0x27) return 7;
    if (cmd >= 0x28 && cmd <= 0x2B) return 5;
    if (cmd >= 0x2C && cmd <= 0x2F) return 9;
    if (cmd >= 0x30 && cmd <= 0x33) return 6;
    if (cmd >= 0x34 && cmd <= 0x37) return 9;
    if (cmd >= 0x38 && cmd <= 0x3B) return 8;
    if (cmd >= 0x3C && cmd <= 0x3F) return 12;
    if (cmd >= 0x40 && cmd <= 0x47) return 3;
    if (cmd >= 0x50 && cmd <= 0x57) return 4;
    if (cmd >= 0x60 && cmd <= 0x63) return 3;
    if (cmd >= 0x64 && cmd <= 0x67) return 4;
    if (cmd >= 0x68 && cmd <= 0x6B) return 2;
    if (cmd >= 0x70 && cmd <= 0x73) return 2;
    if (cmd >= 0x74 && cmd <= 0x77) return 3;
    if (cmd >= 0x78 && cmd <= 0x7B) return 2;
    if (cmd >= 0x7C && cmd <= 0x7F) return 3;
    if (cmd >= 0x80 && cmd <= 0x9F) return 4; // VRAM -> VRAM
    if (cmd >= 0xA0 && cmd <= 0xBF) return 3; // CPU -> VRAM
    if (cmd >= 0xC0 && cmd <= 0xDF) return 3; // VRAM -> CPU
    return 1; // NOP, E1~E6 등
}

static void ExecuteGP0Command(void) {
    uint8_t cmd = (uint8_t)(g_cmd_buf[0] >> 24);

    if (cmd == 0x02) {
        // Fill Rectangle in VRAM
        uint16_t color = rgb888_to_rgb555(g_cmd_buf[0]);
        int x = (g_cmd_buf[1] & 0x3F0);
        int y = (g_cmd_buf[1] >> 16) & 0x1FF;
        int w = ((g_cmd_buf[2] & 0x3FF) + 0x0F) & ~0x0F;
        int h = (g_cmd_buf[2] >> 16) & 0x1FF;

        for (int dy = 0; dy < h; dy++) {
            int py = (y + dy) & 0x1FF;
            for (int dx = 0; dx < w; dx++) {
                g_vram[py][(x + dx) & 0x3FF] = color;
            }
        }
        g_vram_dirty = 1;
    }
    else if (cmd >= 0x80 && cmd <= 0x9F) {
        // VRAM-to-VRAM Copy
        int sx = g_cmd_buf[1] & 0x3FF;
        int sy = (g_cmd_buf[1] >> 16) & 0x1FF;
        int dx = g_cmd_buf[2] & 0x3FF;
        int dy = (g_cmd_buf[2] >> 16) & 0x1FF;
        int w = g_cmd_buf[3] & 0x3FF; if (w == 0) w = 1024;
        int h = (g_cmd_buf[3] >> 16) & 0x1FF; if (h == 0) h = 512;

        for (int row = 0; row < h; row++) {
            for (int col = 0; col < w; col++) {
                uint16_t px = g_vram[(sy + row) & 0x1FF][(sx + col) & 0x3FF];
                g_vram[(dy + row) & 0x1FF][(dx + col) & 0x3FF] = px;
            }
        }
        g_vram_dirty = 1;
    }
    else if (cmd >= 0xA0 && cmd <= 0xBF) {
        // CPU-to-VRAM Transfer 시작
        g_xfer_x = g_cmd_buf[1] & 0x3FF;
        g_xfer_y = (g_cmd_buf[1] >> 16) & 0x1FF;
        g_xfer_w = g_cmd_buf[2] & 0x3FF; if (g_xfer_w == 0) g_xfer_w = 1024;
        g_xfer_h = (g_cmd_buf[2] >> 16) & 0x1FF; if (g_xfer_h == 0) g_xfer_h = 512;
        g_xfer_cur_x = 0;
        g_xfer_cur_y = 0;
        g_xfer_mode = 1;
    }
    else if (cmd >= 0xC0 && cmd <= 0xDF) {
        // VRAM-to-CPU Transfer 시작
        g_xfer_x = g_cmd_buf[1] & 0x3FF;
        g_xfer_y = (g_cmd_buf[1] >> 16) & 0x1FF;
        g_xfer_w = g_cmd_buf[2] & 0x3FF; if (g_xfer_w == 0) g_xfer_w = 1024;
        g_xfer_h = (g_cmd_buf[2] >> 16) & 0x1FF; if (g_xfer_h == 0) g_xfer_h = 512;
        g_xfer_cur_x = 0;
        g_xfer_cur_y = 0;
        g_xfer_mode = 2;
    }
    else if (cmd == 0xE3) {
        // Set Drawing Area Top-Left
        g_draw_x1 = g_cmd_buf[0] & 0x3FF;
        g_draw_y1 = (g_cmd_buf[0] >> 10) & 0x1FF;
    }
    else if (cmd == 0xE4) {
        // Set Drawing Area Bottom-Right
        g_draw_x2 = g_cmd_buf[0] & 0x3FF;
        g_draw_y2 = (g_cmd_buf[0] >> 10) & 0x1FF;
    }
    else if (cmd == 0xE5) {
        // Set Drawing Offset (11-bit signed)
        int ox = g_cmd_buf[0] & 0x7FF;
        int oy = (g_cmd_buf[0] >> 11) & 0x7FF;
        if (ox & 0x400) ox -= 0x800;
        if (oy & 0x400) oy -= 0x800;
        g_draw_off_x = ox;
        g_draw_off_y = oy;
    }
}

static void WriteVramHalfword(uint16_t val) {
    g_vram[(g_xfer_y + g_xfer_cur_y) & 0x1FF][(g_xfer_x + g_xfer_cur_x) & 0x3FF] = val;
    g_xfer_cur_x++;
    if (g_xfer_cur_x >= g_xfer_w) {
        g_xfer_cur_x = 0;
        g_xfer_cur_y++;
        if (g_xfer_cur_y >= g_xfer_h) {
            g_xfer_mode = 0;
            g_vram_dirty = 1;
        }
    }
}

static uint16_t ReadVramHalfword(void) {
    uint16_t val = g_vram[(g_xfer_y + g_xfer_cur_y) & 0x1FF][(g_xfer_x + g_xfer_cur_x) & 0x3FF];
    g_xfer_cur_x++;
    if (g_xfer_cur_x >= g_xfer_w) {
        g_xfer_cur_x = 0;
        g_xfer_cur_y++;
        if (g_xfer_cur_y >= g_xfer_h) {
            g_xfer_mode = 0;
        }
    }
    return val;
}

static void ProcessGP0Word(uint32_t data) {
    // 1) CPU -> VRAM 픽셀 데이터 전송 중인 경우
    if (g_xfer_mode == 1) {
        WriteVramHalfword((uint16_t)(data & 0xFFFF));
        if (g_xfer_mode == 1) {
            WriteVramHalfword((uint16_t)(data >> 16));
        }
        return;
    }

    // 2) 폴리라인 종료 마커 처리
    if (g_in_polyline) {
        if ((data & 0xF000F000u) == 0x50005000u) {
            g_in_polyline = 0;
            g_cmd_idx = 0;
        }
        return;
    }

    // 3) 새 명령 시작
    if (g_cmd_idx == 0) {
        uint8_t cmd = (uint8_t)(data >> 24);
        if ((cmd >= 0x48 && cmd <= 0x4F) || (cmd >= 0x58 && cmd <= 0x5F)) {
            g_in_polyline = 1;
            return;
        }
        g_cmd_len = GetGP0CommandLength(cmd);
    }

    if (g_cmd_idx < 16) {
        g_cmd_buf[g_cmd_idx] = data;
    }
    g_cmd_idx++;

    if (g_cmd_idx >= g_cmd_len) {
        ExecuteGP0Command();
        g_cmd_idx = 0;
    }
}

// ------------------------------------------------------------
// 1. PSEmu Pro 메타데이터
// ------------------------------------------------------------
uint32_t CALLBACK_API PSEgetLibType(void) { return 2; }
char* CALLBACK_API PSEgetLibName(void) { return "MiniGL PSX GPU Plugin (Step 3)"; }
uint32_t CALLBACK_API PSEgetLibVersion(void) { return (1 << 16) | (0 << 8) | 3; }

// ------------------------------------------------------------
// 2. 초기화 / 종료 / 설정
// ------------------------------------------------------------
long CALLBACK_API GPUinit(void) {
    memset(g_vram, 0, sizeof(g_vram));
    g_vram_dirty = 1;
    return 0;
}

long CALLBACK_API GPUshutdown(void) { return 0; }
long CALLBACK_API GPUtest(void) { return 0; }

long CALLBACK_API GPUconfigure(void) {
    MessageBoxA(NULL, "MiniGL GPU Plugin - Step 3 (DMA + GP0 VRAM)", "Configure", MB_OK);
    return 0;
}

void CALLBACK_API GPUabout(void) {
    MessageBoxA(NULL, "MiniGL PSX OpenGL Plugin\nStep 3: DMA Chain & GP0 VRAM Transfer", "About", MB_OK);
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
// 4. 매 프레임 VRAM 출력
// ------------------------------------------------------------
void CALLBACK_API GPUupdateLace(void) {
    if (!g_hdc || !g_hrc) return;

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
// 5. GP1 제어 명령 처리
// ------------------------------------------------------------
void CALLBACK_API GPUwriteStatus(uint32_t gdata) {
    uint32_t cmd = (gdata >> 24) & 0xFF;

    switch (cmd) {
    case 0x00: // Reset GPU
        g_gpu_stat = 0x14802000;
        g_disp_disabled = 1;
        g_disp_x = 0; g_disp_y = 0;
        g_disp_w = 320; g_disp_h = 240;
        g_is_24bit = 0;
        g_cmd_idx = 0;
        g_xfer_mode = 0;
        g_in_polyline = 0;
        break;
    case 0x01: // Reset Command Buffer
        g_cmd_idx = 0;
        g_xfer_mode = 0;
        g_in_polyline = 0;
        break;
    case 0x03: // Display Enable
        g_disp_disabled = (gdata & 1);
        g_gpu_stat = (g_gpu_stat & ~(1u << 23)) | ((gdata & 1) << 23);
        break;
    case 0x05: // Display Area Start
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
    g_gpu_stat ^= 0x80000000u;
    return g_gpu_stat | 0x1C000000u;
}

// ------------------------------------------------------------
// 6. GP0 데이터 및 DMA 체인 처리
// ------------------------------------------------------------
void CALLBACK_API GPUwriteData(uint32_t gdata) {
    ProcessGP0Word(gdata);
}

void CALLBACK_API GPUwriteDataMem(uint32_t* pMem, int iSize) {
    if (!pMem || iSize <= 0) return;
    for (int i = 0; i < iSize; i++) {
        ProcessGP0Word(pMem[i]);
    }
}

uint32_t CALLBACK_API GPUreadData(void) {
    if (g_xfer_mode == 2) {
        uint32_t lo = ReadVramHalfword();
        uint32_t hi = (g_xfer_mode == 2) ? ReadVramHalfword() : 0;
        return lo | (hi << 16);
    }
    return 0;
}

void CALLBACK_API GPUreadDataMem(uint32_t* pMem, int iSize) {
    if (!pMem || iSize <= 0) return;
    for (int i = 0; i < iSize; i++) {
        pMem[i] = GPUreadData();
    }
}

long CALLBACK_API GPUdmaChain(uint32_t* baseAddrL, uint32_t addr) {
    if (!baseAddrL) return 0;

    int safety = 0;
    addr &= 0x1FFFFCu;

    while (addr != 0xFFFFFFu && safety++ < 65536) {
        uint32_t header = baseAddrL[addr >> 2];
        uint32_t count = header >> 24;
        uint32_t next = header & 0xFFFFFFu;

        for (uint32_t i = 1; i <= count; i++) {
            uint32_t word_addr = (addr + (i << 2)) & 0x1FFFFCu;
            ProcessGP0Word(baseAddrL[word_addr >> 2]);
        }

        if (next == 0xFFFFFFu || (next & 0x800000u)) break;
        addr = next & 0x1FFFFCu;
    }

    return 0;
}

long CALLBACK_API GPUfreeze(uint32_t ulGetFreezeData, void* pF) {
    (void)ulGetFreezeData; (void)pF;
    return 1;
}