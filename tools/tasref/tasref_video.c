// tasref's video plugin: draws nothing. It keeps the RDRAM pointer for tasref_input.c, and it
// raises the RDP's done interrupt after each display list, which the game waits for.
#include <stddef.h>
#include <string.h>

#define M64P_PLUGIN_PROTOTYPES 1
#include <mupen64plus/m64p_common.h>
#include <mupen64plus/m64p_plugin.h>

static GFX_INFO sGfx;

EXPORT unsigned char *CALL tasref_rdram(void) {
    return sGfx.RDRAM;
}

EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle core, void *context,
                                     void (*debug)(void *, int, const char *)) {
    (void) core, (void) context, (void) debug;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginShutdown(void) {
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *type, int *version, int *api, const char **name,
                                        int *caps) {
    if (type) *type = M64PLUGIN_GFX;
    if (version) *version = 0x010000;
    if (api) *api = 0x020200;
    if (name) *name = "tasref video";
    if (caps) *caps = 0;
    return M64ERR_SUCCESS;
}

EXPORT int CALL InitiateGFX(GFX_INFO info) {
    sGfx = info;
    return 1;
}

EXPORT void CALL ProcessDList(void) {
    *sGfx.MI_INTR_REG |= 0x20;   // MI_INTR_DP
    sGfx.CheckInterrupts();
}

EXPORT void CALL ProcessRDPList(void) {
    *sGfx.DPC_START_REG = *sGfx.DPC_CURRENT_REG = *sGfx.DPC_END_REG;
}

EXPORT int CALL RomOpen(void) {
    return 1;
}

EXPORT void CALL ReadScreen2(void *dest, int *width, int *height, int front) {
    (void) dest, (void) front;
    *width = *height = 0;
}

EXPORT void CALL RomClosed(void) {}
EXPORT void CALL ChangeWindow(void) {}
EXPORT void CALL MoveScreen(int x, int y) { (void) x, (void) y; }
EXPORT void CALL ShowCFB(void) {}
EXPORT void CALL UpdateScreen(void) {}
EXPORT void CALL ViStatusChanged(void) {}
EXPORT void CALL ViWidthChanged(void) {}
EXPORT void CALL SetRenderingCallback(void (*callback)(int)) { (void) callback; }
EXPORT void CALL ResizeVideoOutput(int width, int height) { (void) width, (void) height; }
EXPORT void CALL FBRead(unsigned int addr) { (void) addr; }
EXPORT void CALL FBWrite(unsigned int addr, unsigned int size) { (void) addr, (void) size; }
EXPORT void CALL FBGetFrameBufferInfo(void *p) { memset(p, 0, 4 * 6 * sizeof(int)); }
