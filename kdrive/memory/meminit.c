#ifdef HAVE_CONFIG_H
#include <kdrive-config.h>
#endif

#include <stdlib.h>
#include <string.h>

#define XK_PUBLISHING
#include <X11/keysym.h>

#include "memory.h"
#include "kkeymap.h"
#include "tinyx-display.h"
#include "tinyx-input.h"

static int
MemoryOsInit(void)
{
    return 1;
}

static void
MemoryOsNoop(void)
{
}

static Bool
MemorySpecialKey(KeySym key)
{
    (void)key;
    return FALSE;
}

/* Native KDrive hosts use this hook for VT setup. The memory host has none. */
void
OsVendorInit(void)
{
}

static const KdOsFuncs memoryOsFuncs = {
    MemoryOsInit,
    MemoryOsNoop,
    MemorySpecialKey,
    MemoryOsNoop,
    MemoryOsNoop,
    MemoryOsNoop
};

static Bool
MemoryMouseInit(void)
{
    return TRUE;
}

static void
MemoryMouseFini(void)
{
}

static const KdMouseFuncs memoryMouseFuncs = {
    MemoryMouseInit,
    MemoryMouseFini
};

static void
MemorySetKey(int keycode, KeySym normal, KeySym shifted)
{
    KeySym *row = kdKeymap +
        (keycode - KD_MIN_KEYCODE) * KD_MAX_WIDTH;

    row[0] = normal;
    row[1] = shifted;
}

/* Stable US core keymap. Hosts inject these X keycodes, not host scan codes. */
static void
MemoryKeyboardLoad(void)
{
    memset(kdKeymap, 0, sizeof(KeySym) * KD_MAX_LENGTH * KD_MAX_WIDTH);
    kdMinScanCode = 0;
    kdMaxScanCode = TINYX_INPUT_MAX_KEYCODE - KD_MIN_KEYCODE;

    MemorySetKey(9, XK_Escape, NoSymbol);
    MemorySetKey(10, XK_1, XK_exclam);
    MemorySetKey(11, XK_2, XK_at);
    MemorySetKey(12, XK_3, XK_numbersign);
    MemorySetKey(13, XK_4, XK_dollar);
    MemorySetKey(14, XK_5, XK_percent);
    MemorySetKey(15, XK_6, XK_asciicircum);
    MemorySetKey(16, XK_7, XK_ampersand);
    MemorySetKey(17, XK_8, XK_asterisk);
    MemorySetKey(18, XK_9, XK_parenleft);
    MemorySetKey(19, XK_0, XK_parenright);
    MemorySetKey(20, XK_minus, XK_underscore);
    MemorySetKey(21, XK_equal, XK_plus);
    MemorySetKey(22, XK_BackSpace, NoSymbol);
    MemorySetKey(23, XK_Tab, XK_ISO_Left_Tab);
    MemorySetKey(24, XK_q, XK_Q);
    MemorySetKey(25, XK_w, XK_W);
    MemorySetKey(26, XK_e, XK_E);
    MemorySetKey(27, XK_r, XK_R);
    MemorySetKey(28, XK_t, XK_T);
    MemorySetKey(29, XK_y, XK_Y);
    MemorySetKey(30, XK_u, XK_U);
    MemorySetKey(31, XK_i, XK_I);
    MemorySetKey(32, XK_o, XK_O);
    MemorySetKey(33, XK_p, XK_P);
    MemorySetKey(34, XK_bracketleft, XK_braceleft);
    MemorySetKey(35, XK_bracketright, XK_braceright);
    MemorySetKey(36, XK_Return, NoSymbol);
    MemorySetKey(37, XK_Control_L, NoSymbol);
    MemorySetKey(38, XK_a, XK_A);
    MemorySetKey(39, XK_s, XK_S);
    MemorySetKey(40, XK_d, XK_D);
    MemorySetKey(41, XK_f, XK_F);
    MemorySetKey(42, XK_g, XK_G);
    MemorySetKey(43, XK_h, XK_H);
    MemorySetKey(44, XK_j, XK_J);
    MemorySetKey(45, XK_k, XK_K);
    MemorySetKey(46, XK_l, XK_L);
    MemorySetKey(47, XK_semicolon, XK_colon);
    MemorySetKey(48, XK_apostrophe, XK_quotedbl);
    MemorySetKey(49, XK_grave, XK_asciitilde);
    MemorySetKey(50, XK_Shift_L, NoSymbol);
    MemorySetKey(51, XK_backslash, XK_bar);
    MemorySetKey(52, XK_z, XK_Z);
    MemorySetKey(53, XK_x, XK_X);
    MemorySetKey(54, XK_c, XK_C);
    MemorySetKey(55, XK_v, XK_V);
    MemorySetKey(56, XK_b, XK_B);
    MemorySetKey(57, XK_n, XK_N);
    MemorySetKey(58, XK_m, XK_M);
    MemorySetKey(59, XK_comma, XK_less);
    MemorySetKey(60, XK_period, XK_greater);
    MemorySetKey(61, XK_slash, XK_question);
    MemorySetKey(62, XK_Shift_R, NoSymbol);
    MemorySetKey(63, XK_KP_Multiply, NoSymbol);
    MemorySetKey(64, XK_Alt_L, XK_Meta_L);
    MemorySetKey(65, XK_space, NoSymbol);
    MemorySetKey(66, XK_Caps_Lock, NoSymbol);
    MemorySetKey(67, XK_F1, NoSymbol);
    MemorySetKey(68, XK_F2, NoSymbol);
    MemorySetKey(69, XK_F3, NoSymbol);
    MemorySetKey(70, XK_F4, NoSymbol);
    MemorySetKey(71, XK_F5, NoSymbol);
    MemorySetKey(72, XK_F6, NoSymbol);
    MemorySetKey(73, XK_F7, NoSymbol);
    MemorySetKey(74, XK_F8, NoSymbol);
    MemorySetKey(75, XK_F9, NoSymbol);
    MemorySetKey(76, XK_F10, NoSymbol);
    MemorySetKey(77, XK_Num_Lock, NoSymbol);
    MemorySetKey(78, XK_Scroll_Lock, NoSymbol);
    MemorySetKey(79, XK_KP_Home, XK_KP_7);
    MemorySetKey(80, XK_KP_Up, XK_KP_8);
    MemorySetKey(81, XK_KP_Prior, XK_KP_9);
    MemorySetKey(82, XK_KP_Subtract, NoSymbol);
    MemorySetKey(83, XK_KP_Left, XK_KP_4);
    MemorySetKey(84, XK_KP_Begin, XK_KP_5);
    MemorySetKey(85, XK_KP_Right, XK_KP_6);
    MemorySetKey(86, XK_KP_Add, NoSymbol);
    MemorySetKey(87, XK_KP_End, XK_KP_1);
    MemorySetKey(88, XK_KP_Down, XK_KP_2);
    MemorySetKey(89, XK_KP_Next, XK_KP_3);
    MemorySetKey(90, XK_KP_Insert, XK_KP_0);
    MemorySetKey(91, XK_KP_Delete, XK_KP_Decimal);
    MemorySetKey(95, XK_F11, NoSymbol);
    MemorySetKey(96, XK_F12, NoSymbol);
    MemorySetKey(104, XK_KP_Enter, NoSymbol);
    MemorySetKey(105, XK_Control_R, NoSymbol);
    MemorySetKey(106, XK_KP_Divide, NoSymbol);
    MemorySetKey(108, XK_Alt_R, XK_Meta_R);
    MemorySetKey(110, XK_Home, NoSymbol);
    MemorySetKey(111, XK_Up, NoSymbol);
    MemorySetKey(112, XK_Prior, NoSymbol);
    MemorySetKey(113, XK_Left, NoSymbol);
    MemorySetKey(114, XK_Right, NoSymbol);
    MemorySetKey(115, XK_End, NoSymbol);
    MemorySetKey(116, XK_Down, NoSymbol);
    MemorySetKey(117, XK_Next, NoSymbol);
    MemorySetKey(118, XK_Insert, NoSymbol);
    MemorySetKey(119, XK_Delete, NoSymbol);
    MemorySetKey(133, XK_Super_L, NoSymbol);
    MemorySetKey(134, XK_Super_R, NoSymbol);
    MemorySetKey(135, XK_Menu, NoSymbol);
}

static int
MemoryKeyboardInit(void)
{
    return Success;
}

static void
MemoryKeyboardLeds(int leds)
{
    TinyXInputNotifyLeds((unsigned int)leds);
}

static void
MemoryKeyboardBell(int volume, int frequency, int duration)
{
    TinyXInputNotifyBell(volume, frequency, duration);
}

static void
MemoryKeyboardFini(void)
{
}

static const KdKeyboardFuncs memoryKeyboardFuncs = {
    MemoryKeyboardLoad,
    MemoryKeyboardInit,
    MemoryKeyboardLeds,
    MemoryKeyboardBell,
    MemoryKeyboardFini,
    0
};

void
InitCard(char *name)
{
    (void)name;
    KdCardInfoAdd(&TinyXMemoryCardFuncs, NULL);
}

void
InitOutput(ScreenInfo *pScreenInfo, int argc, char **argv)
{
    if (!TinyXMemoryDisplayIsConfigured()) {
        TinyXMemoryDisplayConfig config;

        memset(&config, 0, sizeof(config));
        config.width = 1024;
        config.height = 768;
        if (!TinyXMemoryDisplayConfigure(&config))
            FatalError("could not configure default memory display\n");
    }

    KdOsInit(&memoryOsFuncs);
    KdInitOutput(pScreenInfo, argc, argv);
}

void
InitInput(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!kdMouseInfo)
        KdParseMouse("memory,5");
    KdInitInput(&memoryMouseFuncs, &memoryKeyboardFuncs);
}

void
ddxUseMsg(void)
{
    KdUseMsg();
    ErrorF("\nXmemory uses a depth-24, 32-bpp memory framebuffer.\n\n");
}

int
ddxProcessArgument(int argc, char **argv, int i)
{
    if (!strcmp(argv[i], "-version")) {
        kdVersion("Xmemory");
        exit(0);
    }
    return KdProcessArgument(argc, argv, i);
}
