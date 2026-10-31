// tasref: the N64 reference for the TAS. Runs SM64 (US ROM) in Mupen64Plus with a Mupen64 movie
// and writes, at each movie sample, the line the pc-endjinn host build writes with SM64_TRACE.
// tools/tasref/compare.py then finds the first sample where the port and the N64 game differ.
//
//   tools/tasref/build.sh
//   build/tasref/tasref baserom.us.z64 movie.m64 n64.trace [workdir]
//
// The workdir (default build/tasref/run) holds Mupen's config and saves; it is emptied first, so
// the game starts with a blank EEPROM, as the movie does (power-on).
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M64P_CORE_PROTOTYPES 1
#include <mupen64plus/m64p_common.h>
#include <mupen64plus/m64p_config.h>
#include <mupen64plus/m64p_frontend.h>
#include <mupen64plus/m64p_types.h>

#ifndef MUPEN_LIB_DIR
#define MUPEN_LIB_DIR "/opt/homebrew/lib"
#endif
#ifndef MUPEN_DATA_DIR
#define MUPEN_DATA_DIR "/opt/homebrew/share/mupen64plus"
#endif

static unsigned int (*sDone)(void);
static unsigned int (*sSample)(void);
static int sLimiterOff;
static unsigned int sStopAt;   // TASREF_STOP_AT=n: stop at movie sample n

static unsigned char *read_file(const char *path, long *size) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc((size_t) *size);
    if (data == NULL || fread(data, 1, (size_t) *size, f) != (size_t) *size) {
        free(data);
        data = NULL;
    }
    fclose(f);
    return data;
}

static void debug_cb(void *context, int level, const char *message) {
    if (level <= M64MSG_WARNING) {
        fprintf(stderr, "%s: %s\n", (const char *) context, message);
    }
}

static void frame_cb(unsigned int frame) {
    if (!sLimiterOff) {
        int off = 0;
        CoreDoCommand(M64CMD_CORE_STATE_SET, M64CORE_SPEED_LIMITER, &off);
        sLimiterOff = 1;
    }
    if (frame % 20000 == 0) {
        fprintf(stderr, "tasref: VI %u, sample %u\n", frame, sSample());
    }
    if (sDone() >= 600 || (sStopAt != 0 && sSample() >= sStopAt)) {
        CoreDoCommand(M64CMD_STOP, 0, NULL);
    }
}

static void *plugin(const char *path, m64p_dynlib_handle core, const char *name) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (h == NULL) {
        fprintf(stderr, "tasref: %s\n", dlerror());
        exit(1);
    }
    ptr_PluginStartup startup = (ptr_PluginStartup) dlsym(h, "PluginStartup");
    if (startup == NULL || startup(core, (void *) name, debug_cb) != M64ERR_SUCCESS) {
        fprintf(stderr, "tasref: %s does not start\n", path);
        exit(1);
    }
    return h;
}

static void attach(m64p_plugin_type type, void *h) {
    if (CoreAttachPlugin(type, h) != M64ERR_SUCCESS) {
        fprintf(stderr, "tasref: plugin type %d does not attach\n", type);
        exit(1);
    }
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s rom.z64 movie.m64 out.trace [workdir]\n", argv[0]);
        return 2;
    }
    const char *work = argc > 4 ? argv[4] : "build/tasref/run";
    if (getenv("TASREF_STOP_AT") != NULL) {
        sStopAt = (unsigned int) atoi(getenv("TASREF_STOP_AT"));
    }
    char cmd[1024], here[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s/save'", work, work);
    if (system(cmd) != 0) {
        return 1;
    }
    // Our plugins sit next to this program.
    snprintf(here, sizeof(here), "%s", argv[0]);
    char *slash = strrchr(here, '/');
    if (slash != NULL) *slash = 0; else strcpy(here, ".");

    long romSize, movieSize;
    unsigned char *rom = read_file(argv[1], &romSize);
    unsigned char *movie = read_file(argv[2], &movieSize);
    if (rom == NULL || movie == NULL || movieSize < 0x400 || memcmp(movie, "M64\x1a", 4) != 0) {
        fprintf(stderr, "tasref: cannot read the ROM or the movie\n");
        return 1;
    }
    FILE *trace = fopen(argv[3], "w");
    if (trace == NULL) {
        return 1;
    }

    m64p_dynlib_handle core = dlopen(MUPEN_LIB_DIR "/libmupen64plus.dylib", RTLD_NOW | RTLD_GLOBAL);
    if (core == NULL || CoreStartup(0x020001, work, MUPEN_DATA_DIR, "core", debug_cb, NULL, NULL)
                            != M64ERR_SUCCESS) {
        fprintf(stderr, "tasref: Mupen64Plus core does not start\n");
        return 1;
    }
    m64p_handle section;
    char save[1100];
    snprintf(save, sizeof(save), "%s/save", work);
    ConfigOpenSection("Core", &section);
    ConfigSetParameter(section, "SaveSRAMPath", M64TYPE_STRING, save);
    ConfigSetParameter(section, "SaveStatePath", M64TYPE_STRING, save);
    int zero = 0;
    ConfigSetParameter(section, "OnScreenDisplay", M64TYPE_BOOL, &zero);

    if (CoreDoCommand(M64CMD_ROM_OPEN, (int) romSize, rom) != M64ERR_SUCCESS) {
        fprintf(stderr, "tasref: the core does not take the ROM\n");
        return 1;
    }
    char path[1200];
    snprintf(path, sizeof(path), "%s/tasref_video.dylib", here);
    void *video = plugin(path, core, "video");
    snprintf(path, sizeof(path), "%s/tasref_input.dylib", here);
    void *input = plugin(path, core, "input");
    void *rsp = plugin(MUPEN_LIB_DIR "/mupen64plus/mupen64plus-rsp-hle.dylib", core, "rsp");

    void (*setup)(unsigned char *(*)(void), unsigned char *, unsigned int, FILE *) =
        (void (*)(unsigned char *(*)(void), unsigned char *, unsigned int, FILE *)) dlsym(input, "tasref_setup");
    sDone = (unsigned int (*)(void)) dlsym(input, "tasref_done");
    sSample = (unsigned int (*)(void)) dlsym(input, "tasref_sample");
    unsigned int samples = (unsigned int) ((movieSize - 0x400) / 4);
    setup((unsigned char *(*)(void)) dlsym(video, "tasref_rdram"), movie, samples, trace);

    attach(M64PLUGIN_GFX, video);
    attach(M64PLUGIN_INPUT, input);
    attach(M64PLUGIN_RSP, rsp);   // no audio plugin: the core's dummy one

    CoreDoCommand(M64CMD_SET_FRAME_CALLBACK, 0, (void *) frame_cb);
    fprintf(stderr, "tasref: %u samples\n", samples);
    CoreDoCommand(M64CMD_EXECUTE, 0, NULL);
    fclose(trace);
    fprintf(stderr, "tasref: stopped at sample %u of %u\n", sSample(), samples);

    CoreDetachPlugin(M64PLUGIN_GFX);
    CoreDetachPlugin(M64PLUGIN_INPUT);
    CoreDetachPlugin(M64PLUGIN_RSP);
    CoreDoCommand(M64CMD_ROM_CLOSE, 0, NULL);
    CoreShutdown();
    return 0;
}
