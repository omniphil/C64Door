/*
 * jxl_enc.c -- JPEG XL pictures for the JPEG XL graphics mode.
 *
 * libjxl is loaded when first wanted rather than linked, so the door builds on a box without libjxl's development
 * files and still runs, minus this one option, on a box without libjxl at all. The declarations come from
 * jxl_include/ (libjxl 0.11.1's own headers); every function used here has been in libjxl since 0.7.
 *
 * Measured on DOOM's attract-mode demos (320x200): about 2 ms a frame to encode at effort 3, and 7 KB at distance 3.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#include <jxl/encode.h>

#include "jxl_enc.h"

#define EFFORT 3        /* 1-9: 3 costs about 2 ms a frame and is within a few percent of the smallest */

static struct
{
    JxlEncoder *(*Create)(const JxlMemoryManager *);
    void (*Reset)(JxlEncoder *);
    JxlEncoderStatus (*UseContainer)(JxlEncoder *, JXL_BOOL);
    JxlEncoderFrameSettings *(*FrameSettingsCreate)(JxlEncoder *, const JxlEncoderFrameSettings *);
    JxlEncoderStatus (*SetFrameDistance)(JxlEncoderFrameSettings *, float);
    JxlEncoderStatus (*SetFrameLossless)(JxlEncoderFrameSettings *, JXL_BOOL);
    JxlEncoderStatus (*FrameSettingsSetOption)(JxlEncoderFrameSettings *, JxlEncoderFrameSettingId, int64_t);
    void (*InitBasicInfo)(JxlBasicInfo *);
    JxlEncoderStatus (*SetBasicInfo)(JxlEncoder *, const JxlBasicInfo *);
    void (*ColorEncodingSetToSRGB)(JxlColorEncoding *, JXL_BOOL);
    JxlEncoderStatus (*SetColorEncoding)(JxlEncoder *, const JxlColorEncoding *);
    JxlEncoderStatus (*AddImageFrame)(const JxlEncoderFrameSettings *, const JxlPixelFormat *, const void *, size_t);
    void (*CloseInput)(JxlEncoder *);
    JxlEncoderStatus (*ProcessOutput)(JxlEncoder *, uint8_t **, size_t *);
} J;

static int         g_state;        /* 0 not tried, 1 loaded, -1 unavailable */
static JxlEncoder *g_enc;
static uint8_t    *g_out, *g_packed;
static size_t      g_out_cap, g_packed_cap;

static bool load(void)
{
    static const char *const NAMES[] = { "libjxl.so.0.11", "libjxl.so.0.12", "libjxl.so.0.10", "libjxl.so.0.9",
                                         "libjxl.so.0.8", "libjxl.so.0.7", "libjxl.so" };
    void *lib = NULL;

    for (size_t i = 0; lib == NULL && i < sizeof(NAMES) / sizeof(NAMES[0]); i++)
        lib = dlopen(NAMES[i], RTLD_NOW | RTLD_LOCAL);
    if (lib == NULL)
        return false;

#define SYM(field, name) if ((*(void **)&J.field = dlsym(lib, name)) == NULL) return false
    SYM(Create, "JxlEncoderCreate");
    SYM(Reset, "JxlEncoderReset");
    SYM(UseContainer, "JxlEncoderUseContainer");
    SYM(FrameSettingsCreate, "JxlEncoderFrameSettingsCreate");
    SYM(SetFrameDistance, "JxlEncoderSetFrameDistance");
    SYM(SetFrameLossless, "JxlEncoderSetFrameLossless");
    SYM(FrameSettingsSetOption, "JxlEncoderFrameSettingsSetOption");
    SYM(InitBasicInfo, "JxlEncoderInitBasicInfo");
    SYM(SetBasicInfo, "JxlEncoderSetBasicInfo");
    SYM(ColorEncodingSetToSRGB, "JxlColorEncodingSetToSRGB");
    SYM(SetColorEncoding, "JxlEncoderSetColorEncoding");
    SYM(AddImageFrame, "JxlEncoderAddImageFrame");
    SYM(CloseInput, "JxlEncoderCloseInput");
    SYM(ProcessOutput, "JxlEncoderProcessOutput");
#undef SYM

    g_enc = J.Create(NULL);
    return g_enc != NULL;
}

bool jxl_enc_available(void)
{
    if (g_state == 0)
        g_state = load() ? 1 : -1;
    return g_state == 1;
}

static bool grow(uint8_t **buf, size_t *cap, size_t need)
{
    uint8_t *grown;
    if (*cap >= need)
        return true;
    grown = realloc(*buf, need);
    if (grown == NULL)
        return false;
    *buf = grown;
    *cap = need;
    return true;
}

size_t jxl_enc_rgb(const uint8_t *rgb, int stride, int w, int h, float distance, const uint8_t **out)
{
    JxlEncoderFrameSettings *settings;
    JxlBasicInfo info;
    JxlColorEncoding color;
    JxlPixelFormat format = { 3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
    bool lossless = distance < 0.05f;
    size_t row = (size_t)w * 3, used = 0;

    if (!jxl_enc_available() || w <= 0 || h <= 0)
        return 0;

    /* libjxl takes the rows packed together, so a piece of a wider picture is copied out first */
    if (stride != (int)row)
    {
        if (!grow(&g_packed, &g_packed_cap, row * (size_t)h))
            return 0;
        for (int y = 0; y < h; y++)
            memcpy(g_packed + row * (size_t)y, rgb + (size_t)stride * (size_t)y, row);
        rgb = g_packed;
    }

    J.Reset(g_enc);
    J.UseContainer(g_enc, JXL_FALSE);

    J.InitBasicInfo(&info);
    info.xsize = (uint32_t)w;
    info.ysize = (uint32_t)h;
    info.bits_per_sample = 8;
    info.num_color_channels = 3;
    info.uses_original_profile = lossless ? JXL_TRUE : JXL_FALSE;
    if (J.SetBasicInfo(g_enc, &info) != JXL_ENC_SUCCESS)
        return 0;
    J.ColorEncodingSetToSRGB(&color, JXL_FALSE);
    if (J.SetColorEncoding(g_enc, &color) != JXL_ENC_SUCCESS)
        return 0;

    settings = J.FrameSettingsCreate(g_enc, NULL);
    if (settings == NULL)
        return 0;
    J.FrameSettingsSetOption(settings, JXL_ENC_FRAME_SETTING_EFFORT, EFFORT);
    if (lossless)
        J.SetFrameLossless(settings, JXL_TRUE);
    else
        J.SetFrameDistance(settings, distance);

    if (J.AddImageFrame(settings, &format, rgb, row * (size_t)h) != JXL_ENC_SUCCESS)
        return 0;
    J.CloseInput(g_enc);

    for (;;)
    {
        uint8_t *next;
        size_t avail;
        JxlEncoderStatus status;

        if (!grow(&g_out, &g_out_cap, used + 16384))
            return 0;
        next = g_out + used;
        avail = g_out_cap - used;
        status = J.ProcessOutput(g_enc, &next, &avail);
        used = (size_t)(next - g_out);
        if (status == JXL_ENC_SUCCESS)
            break;
        if (status != JXL_ENC_NEED_MORE_OUTPUT)
            return 0;
        if (!grow(&g_out, &g_out_cap, g_out_cap * 2))
            return 0;
    }
    *out = g_out;
    return used;
}
