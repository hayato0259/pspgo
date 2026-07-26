#include <pspdisplay.h>
#include <pspgu.h>
#include <psputility.h>
#include <string.h>
#include "osk.h"

#define OSK_TEXT_CHARS 64

static unsigned int g_osk_gu_list[262144] __attribute__((aligned(16)));
static unsigned short g_desc[] = {
    'S', 'e', 'a', 'r', 'c', 'h', 0
};
static unsigned short g_intext[OSK_TEXT_CHARS];
static unsigned short g_outtext[OSK_TEXT_CHARS];
static SceUtilityOskData g_data;
static SceUtilityOskParams g_params;
static int g_active = 0;
static int g_shutdown_started = 0;

static void ucs2_to_utf8(const unsigned short *src, char *dst, int dst_size)
{
    int used = 0;

    if (dst_size <= 0)
        return;
    while (*src) {
        unsigned int c = *src++;
        int need = (c < 0x80) ? 1 : (c < 0x800) ? 2 : 3;
        if (used + need >= dst_size)
            break;
        if (need == 1) {
            dst[used++] = (char)c;
        } else if (need == 2) {
            dst[used++] = (char)(0xC0 | (c >> 6));
            dst[used++] = (char)(0x80 | (c & 0x3F));
        } else {
            dst[used++] = (char)(0xE0 | (c >> 12));
            dst[used++] = (char)(0x80 | ((c >> 6) & 0x3F));
            dst[used++] = (char)(0x80 | (c & 0x3F));
        }
    }
    dst[used] = '\0';
}

int osk_begin(void)
{
    if (g_active)
        return -1;

    memset(g_intext, 0, sizeof(g_intext));
    memset(g_outtext, 0, sizeof(g_outtext));
    memset(&g_data, 0, sizeof(g_data));
    g_data.language = PSP_UTILITY_OSK_LANGUAGE_DEFAULT;
    g_data.inputtype = PSP_UTILITY_OSK_INPUTTYPE_ALL;
    g_data.lines = 1;
    g_data.unk_24 = 1;
    g_data.desc = g_desc;
    g_data.intext = g_intext;
    g_data.outtextlength = OSK_TEXT_CHARS;
    g_data.outtext = g_outtext;
    g_data.outtextlimit = OSK_TEXT_CHARS - 1;

    memset(&g_params, 0, sizeof(g_params));
    g_params.base.size = sizeof(g_params);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE,
                                &g_params.base.language);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_BUTTON_SWAP,
                                &g_params.base.buttonSwap);
    g_params.base.graphicsThread = 17;
    g_params.base.accessThread = 19;
    g_params.base.fontThread = 18;
    g_params.base.soundThread = 16;
    g_params.datacount = 1;
    g_params.data = &g_data;

    if (sceUtilityOskInitStart(&g_params) < 0)
        return -1;
    g_active = 1;
    g_shutdown_started = 0;
    return 0;
}

int osk_update(char *out, int out_size)
{
    if (!g_active || !out || out_size <= 0)
        return OSK_ERROR;

    /*
     * Utility ダイアログは、アプリ側の GU リストを完了・同期した後に
     * Update する必要がある。OSK 中もこの処理を毎フレーム続ける。
     */
    sceGuStart(GU_DIRECT, g_osk_gu_list);
    sceGuClearColor(0xFF030303);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);

    int status = sceUtilityOskGetStatus();
    if (status == PSP_UTILITY_DIALOG_VISIBLE) {
        if (sceUtilityOskUpdate(1) < 0) {
            g_active = 0;
            out[0] = '\0';
            return OSK_ERROR;
        }
    } else if (status == PSP_UTILITY_DIALOG_QUIT && !g_shutdown_started) {
        sceUtilityOskShutdownStart();
        g_shutdown_started = 1;
    }

    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();

    if (status != PSP_UTILITY_DIALOG_NONE)
        return OSK_RUNNING;

    g_active = 0;
    if (g_data.result == PSP_UTILITY_OSK_RESULT_CANCELLED) {
        out[0] = '\0';
        return OSK_CANCELLED;
    }
    ucs2_to_utf8(g_outtext, out, out_size);
    return OSK_ACCEPTED;
}
