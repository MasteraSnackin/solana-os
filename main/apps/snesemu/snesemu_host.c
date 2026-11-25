// snes9x2002 (PocketSNES / Snes9x 1.39) glue for Solana OS.
// Interpreter only, APU muted, 256x224 RGB565, whole ROM in PSRAM (max 4 MB).

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "core/os.h"
#include "core/os_mem.h"
#include "apps/emu_pad.h"
#include "snesemu_host.h"

#include "snes9x.h"
#include "memmap.h"
#include "cpuexec.h"
#include "apu.h"
#include "ppu.h"
#include "gfx.h"
#include "soundux.h"
#include "display.h"

static const char *TAG = "snesemu";

#ifndef SNES_FRAMESKIP
#define SNES_FRAMESKIP  3   // draw 1 of 4 PPU frames; interpreter on 240 MHz
#endif

#define SNES_PITCH_BYTES  640   // gfx.h GFX_PITCH
#define SNES_ZPITCH       320
#define SNES_FB_H         239   // SNES_HEIGHT_EXTENDED

bool8 ROMAPUEnabled = 0;
bool overclock_cycles = false;
int one_c, slow_one_c, two_c;

unsigned char *s9x_stream_buffer = NULL;
unsigned long long s9x_stream_buffer_size = 0;

static bool s_booted;
static bool s_inited;
static volatile int s_drawn;
static uint32_t s_joy;
static int s_frameskip = SNES_FRAMESKIP;
static int s_skip_i;
static uint8_t *s_screen;
static uint8_t *s_zbuf;
static uint8_t *s_subz;
static uint8_t *s_save_snap;
static size_t s_save_bytes;
static char s_title[ROM_NAME_LEN];

void S9xSetStreamBuffer(unsigned char *buffer, unsigned long long size)
{
    s9x_stream_buffer = buffer;
    s9x_stream_buffer_size = size;
}

uint32 S9xReadJoypad(int which1)
{
    if (which1 != 0) {
        return 0;
    }
    uint32 pad = 0;
    if (s_joy & EMU_PAD_A) {
        pad |= SNES_A_MASK;
    }
    if (s_joy & EMU_PAD_B) {
        pad |= SNES_B_MASK;
    }
    if (s_joy & EMU_PAD_START) {
        pad |= SNES_START_MASK;
    }
    if (s_joy & EMU_PAD_SELECT) {
        pad |= SNES_SELECT_MASK;
    }
    if (s_joy & EMU_PAD_UP) {
        pad |= SNES_UP_MASK;
    }
    if (s_joy & EMU_PAD_DOWN) {
        pad |= SNES_DOWN_MASK;
    }
    if (s_joy & EMU_PAD_LEFT) {
        pad |= SNES_LEFT_MASK;
    }
    if (s_joy & EMU_PAD_RIGHT) {
        pad |= SNES_RIGHT_MASK;
    }
    if (s_joy & EMU_PAD_L) {
        pad |= SNES_TL_MASK;
    }
    if (s_joy & EMU_PAD_R) {
        pad |= SNES_TR_MASK;
    }
    return pad;
}

bool8_32 S9xDeinitUpdate(int width, int height)
{
    (void)width;
    (void)height;
    s_drawn = 1;
    return TRUE;
}

void S9xGenerateSound(void) {}

void S9xMessage(int type, int number, const char *message)
{
    (void)type;
    (void)number;
    if (message && message[0]) {
        ESP_LOGI(TAG, "%s", message);
    }
}

void S9xExit(void)
{
    ESP_LOGW(TAG, "S9xExit");
}

void S9xLoadSDD1Data(void) {}

void S9xInitCheatData(void) {}

void S9xApplyCheats(void) {}

bool8_32 S9xReadMousePosition(int which1_0_to_1, int *x, int *y, uint32 *buttons)
{
    (void)which1_0_to_1;
    (void)x;
    (void)y;
    (void)buttons;
    return FALSE;
}

bool8_32 S9xReadSuperScopePosition(int *x, int *y, uint32 *buttons)
{
    (void)x;
    (void)y;
    (void)buttons;
    return FALSE;
}

void S9xToggleSoundChannel(int channel)
{
    (void)channel;
}

void S9xSyncSpeed(void) {}

static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "%s: SRAM %u free, PSRAM %u free, largest PSRAM block %u",
             when,
             (unsigned)heap_caps_get_free_size(OS_MEM_SRAM),
             (unsigned)heap_caps_get_free_size(OS_MEM_PSRAM),
             (unsigned)heap_caps_get_largest_free_block(OS_MEM_PSRAM));
}

static size_t sram_bytes(void)
{
    if (!Memory.SRAMSize) {
        return 0;
    }
    size_t n = ((size_t)1 << (Memory.SRAMSize + 3)) * 128u;
    if (n > 0x20000) {
        n = 0x20000;
    }
    return n;
}

static void settings_defaults(void)
{
    memset(&Settings, 0, sizeof(Settings));
    Settings.JoystickEnabled = FALSE;
    Settings.SoundPlaybackRate = 0;
    Settings.Stereo = FALSE;
    Settings.SoundBufferSize = 0;
    Settings.CyclesPercentage = 100;
    Settings.DisableSoundEcho = TRUE;
    Settings.APUEnabled = FALSE;
    Settings.H_Max = SNES_CYCLES_PER_SCANLINE;
    Settings.SkipFrames = SNES_FRAMESKIP;
    Settings.Shutdown = Settings.ShutdownMaster = TRUE;
    Settings.FrameTimePAL = 20000;
    Settings.FrameTimeNTSC = 16667;
    Settings.FrameTime = Settings.FrameTimeNTSC;
    Settings.DisableSampleCaching = TRUE;
    Settings.DisableMasterVolume = TRUE;
    Settings.Mouse = FALSE;
    Settings.SuperScope = FALSE;
    Settings.MultiPlayer5 = FALSE;
    Settings.ControllerOption = SNES_JOYPAD;
    Settings.ForceTransparency = FALSE;
    Settings.Transparency = TRUE;
    Settings.SixteenBit = TRUE;
    Settings.SupportHiRes = FALSE;
    Settings.AutoSaveDelay = 30;
    Settings.ApplyCheats = FALSE;
    Settings.TurboMode = FALSE;
    Settings.SoundSync = FALSE;
    Settings.SpeedHacks = TRUE;
    Settings.HBlankStart = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;
    Settings.InterpolatedSound = FALSE;
    CPU.Flags = 0;
}

static bool gfx_alloc(void)
{
    size_t screen = (size_t)SNES_PITCH_BYTES * SNES_FB_H;
    size_t z = (size_t)SNES_ZPITCH * SNES_FB_H;
    s_screen = heap_caps_calloc(1, screen * 2, os_mem_bulk_caps());
    s_zbuf = heap_caps_calloc(1, z, os_mem_bulk_caps());
    s_subz = heap_caps_calloc(1, z, os_mem_bulk_caps());
    if (!s_screen || !s_zbuf || !s_subz) {
        ESP_LOGE(TAG, "GFX alloc failed screen %p z %p subz %p",
                 (void *)s_screen, (void *)s_zbuf, (void *)s_subz);
        return false;
    }
    GFX.Screen_buffer = s_screen;
    GFX.Screen = s_screen;
    GFX.SubScreen = s_screen + screen;
    GFX.ZBuffer_buffer = s_zbuf;
    GFX.ZBuffer = s_zbuf;
    GFX.SubZBuffer_buffer = s_subz;
    GFX.SubZBuffer = s_subz;
    GFX.Pitch = SNES_PITCH_BYTES;
    return true;
}

static void gfx_free(void)
{
    heap_caps_free(s_screen);
    heap_caps_free(s_zbuf);
    heap_caps_free(s_subz);
    s_screen = s_zbuf = s_subz = NULL;
    GFX.Screen_buffer = GFX.Screen = GFX.SubScreen = NULL;
    GFX.ZBuffer_buffer = GFX.ZBuffer = NULL;
    GFX.SubZBuffer_buffer = GFX.SubZBuffer = NULL;
}

static bool load_rom(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "open %s failed", path);
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || (unsigned long)sz > (unsigned long)MAX_ROM_SIZE + 0x200) {
        ESP_LOGE(TAG, "ROM %s size %ld (max %u)", path, sz, (unsigned)MAX_ROM_SIZE + 0x200);
        fclose(f);
        return false;
    }
    size_t n = fread(Memory.ROM, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        ESP_LOGE(TAG, "short read %s %u/%ld", path, (unsigned)n, sz);
        return false;
    }
    s9x_rom_bytes = (unsigned long)n;
    if (!LoadROM()) {
        ESP_LOGE(TAG, "LoadROM failed %s", path);
        s9x_rom_bytes = 0;
        return false;
    }
    s9x_rom_bytes = 0;
    Settings.APUEnabled = FALSE;
    CPU.APU_APUExecuting = FALSE;
    S9xSetSoundMute(TRUE);
    return true;
}

static void load_sav(const char *path)
{
    size_t n = sram_bytes();
    if (!n || !Memory.SRAM || !path || !path[0]) {
        return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return;
    }
    size_t got = fread(Memory.SRAM, 1, n, f);
    fclose(f);
    ESP_LOGI(TAG, "loaded %u B SRAM from %s", (unsigned)got, path);
}

bool snes_core_boot(const char *rom_path, const char *sav_path)
{
    snes_core_halt();
    log_heap("boot");
    settings_defaults();
    if (!s_inited) {
        if (!MemoryInit() || !S9xInitAPU() || !S9xInitSound()) {
            ESP_LOGE(TAG, "Memory/APU init failed");
            S9xDeinitAPU();
            MemoryDeinit();
            return false;
        }
        s_inited = true;
    }
    if (!gfx_alloc() || !S9xGraphicsInit()) {
        ESP_LOGE(TAG, "graphics init failed");
        snes_core_halt();
        return false;
    }
    log_heap("after init");
    if (!load_rom(rom_path)) {
        snes_core_halt();
        return false;
    }
    load_sav(sav_path);
    s_save_bytes = sram_bytes();
    if (s_save_bytes) {
        s_save_snap = heap_caps_malloc(s_save_bytes, os_mem_bulk_caps());
        if (s_save_snap) {
            memcpy(s_save_snap, Memory.SRAM, s_save_bytes);
        }
    }
    memset(s_title, 0, sizeof(s_title));
    strlcpy(s_title, Memory.ROMName, sizeof(s_title));
    s_skip_i = 0;
    s_drawn = 0;
    s_booted = true;
    ESP_LOGI(TAG, "booted '%s' %u KB (LoROM %d SRAM %u skip %d)",
             s_title, (unsigned)(Memory.CalculatedSize / 1024),
             (int)Memory.LoROM, (unsigned)s_save_bytes, s_frameskip);
    log_heap("running");
    return true;
}

void snes_core_halt(void)
{
    s_booted = false;
    heap_caps_free(s_save_snap);
    s_save_snap = NULL;
    s_save_bytes = 0;
    S9xGraphicsDeinit();
    if (s_inited) {
        S9xDeinitAPU();
        MemoryDeinit();
        s_inited = false;
    }
    gfx_free();
    s9x_rom_bytes = 0;
    s_title[0] = 0;
}

int snes_core_frame(uint32_t joy, const uint16_t **fb)
{
    if (!s_booted) {
        return 0;
    }
    s_joy = joy;
    int n = 0;
    s_drawn = 0;
    while (!s_drawn && n < 12) {
        s_skip_i++;
        if (s_skip_i > s_frameskip) {
            s_skip_i = 0;
        }
        IPPU.RenderThisFrame = (s_skip_i == 0) ? TRUE : FALSE;
        S9xMainLoop();
        n++;
    }
    if (fb) {
        *fb = s_drawn ? (const uint16_t *)GFX.Screen : NULL;
    }
    return n;
}

void snes_core_save(const char *sav_path, bool force)
{
    (void)force;
    if (!s_booted || !sav_path || !sav_path[0] || !s_save_bytes || !Memory.SRAM) {
        return;
    }
    if (s_save_snap && memcmp(s_save_snap, Memory.SRAM, s_save_bytes) == 0) {
        return;
    }
    FILE *f = fopen(sav_path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "save %s failed", sav_path);
        return;
    }
    size_t w = fwrite(Memory.SRAM, 1, s_save_bytes, f);
    fclose(f);
    if (s_save_snap) {
        memcpy(s_save_snap, Memory.SRAM, s_save_bytes);
    }
    ESP_LOGI(TAG, "wrote %u B %s", (unsigned)w, sav_path);
}

bool snes_core_save_dirty(void)
{
    if (!s_booted || !s_save_bytes || !s_save_snap || !Memory.SRAM) {
        return false;
    }
    return memcmp(s_save_snap, Memory.SRAM, s_save_bytes) != 0;
}

const char *snes_core_title(void)
{
    return s_title;
}

int snes_core_frameskip(void)
{
    return s_frameskip;
}
