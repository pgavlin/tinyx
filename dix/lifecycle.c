/***********************************************************

Copyright 1987, 1998  The Open Group

Permission to use, copy, modify, distribute, and sell this software and its
documentation for any purpose is hereby granted without fee, provided that
the above copyright notice appear in all copies and that both that copyright
notice and this permission notice appear in supporting documentation.

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
OPEN GROUP BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Except as contained in this notice, the name of The Open Group shall not be
used in advertising or otherwise to promote the sale, use or other dealings
in this Software without prior written authorization from The Open Group.

Copyright 1987 by Digital Equipment Corporation, Maynard, Massachusetts.

                        All Rights Reserved

Permission to use, copy, modify, and distribute this software and its
documentation for any purpose and without fee is hereby granted,
provided that the above copyright notice appear in all copies and that
both that copyright notice and this permission notice appear in
supporting documentation, and that the name of Digital not be
used in advertising or publicity pertaining to distribution of the
software without specific, written prior permission.

DIGITAL DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING
ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL
DIGITAL BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR
ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
SOFTWARE.

******************************************************************/

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <X11/X.h>
#include <X11/Xos.h>
#include "scrnintstr.h"
#include "misc.h"
#include "os.h"
#include "windowstr.h"
#include "resource.h"
#include "dixstruct.h"
#include "gcstruct.h"
#include "extension.h"
#include "cursorstr.h"
#include "opaque.h"
#include "site.h"
#include "dixfont.h"
#include "extnsionst.h"
#include "dixevents.h"
#include "lifecycle.h"

#ifdef DPMSExtension
#define DPMS_SERVER
#include <X11/extensions/dpms.h>
#include "dpmsproc.h"
#endif

extern int InitClientPrivates(ClientPtr client);
extern void Dispatch(void);
extern char *ConnectionInfo;

/* Helpers retained in main.c with the DIX screen and connection data. */
extern Bool CreateConnectionBlock(void);
extern void FreeScreen(ScreenPtr pScreen);

/* SetInputCheck retains these addresses for the life of the process. */
static HWEventQueueType alwaysCheckForInput[2] = { 0, 1 };

void
TinyXServerInitialize(int argc, char **argv, char **envp)
{
    char *xauthfile;

    display = "0";

    /* Quartz support on Mac OS X requires that the Cocoa event loop be in
     * the main thread. This allows the X server main to be called again
     * from another thread. */
    CheckUserParameters(argc, argv, envp);
    CheckUserAuthorization();
    InitConnectionLimits();

    /* Prepare the X authority file from the environment. A command-line
     * option processed below may override it. */
    xauthfile = getenv("XAUTHORITY");
    if (xauthfile)
        InitAuthorization(xauthfile);
    ProcessCommandLine(argc, argv);
}

void
TinyXServerInitializeGeneration(int argc, char **argv)
{
    int i;

    serverGeneration++;
    ScreenSaverTime = defaultScreenSaverTime;
    ScreenSaverInterval = defaultScreenSaverInterval;
    ScreenSaverBlanking = defaultScreenSaverBlanking;
    ScreenSaverAllowExposures = defaultScreenSaverAllowExposures;
#ifdef DPMSExtension
    DPMSStandbyTime = DEFAULT_SCREEN_SAVER_TIME;
    DPMSSuspendTime = DEFAULT_SCREEN_SAVER_TIME;
    DPMSOffTime = DEFAULT_SCREEN_SAVER_TIME;
    DPMSEnabled = TRUE;
    DPMSPowerLevel = 0;
#endif

    InitBlockAndWakeupHandlers();
    OsInit();
    if (serverGeneration == 1) {
        CreateWellKnownSockets();
        for (i = 1; i < MAXCLIENTS; i++)
            clients[i] = NullClient;
        serverClient = malloc(sizeof(ClientRec));
        if (!serverClient)
            FatalError("couldn't create server client");
        InitClient(serverClient, 0, (pointer) NULL);
    }
    else {
        ResetWellKnownSockets();
    }

    clients[0] = serverClient;
    currentMaxClients = 1;

    if (!InitClientResources(serverClient))
        FatalError("couldn't init server resources");

    SetInputCheck(&alwaysCheckForInput[0], &alwaysCheckForInput[1]);
    screenInfo.numScreens = 0;

    InitAtoms();
    InitEvents();
    InitGlyphCaching();
    ResetExtensionPrivates();
    ResetClientPrivates();
    ResetScreenPrivates();
    ResetWindowPrivates();
    ResetGCPrivates();
    ResetPixmapPrivates();
    ResetColormapPrivates();
    ResetFontPrivateIndex();
    ResetDevicePrivateIndex();
    InitCallbackManager();
    InitVisualWrap();
    InitOutput(&screenInfo, argc, argv);

    if (screenInfo.numScreens < 1)
        FatalError("no screens found");

    InitExtensions(argc, argv);
    if (!InitClientPrivates(serverClient))
        FatalError("failed to allocate serverClient devprivates");

    for (i = 0; i < screenInfo.numScreens; i++) {
        ScreenPtr pScreen = screenInfo.screens[i];

        if (!CreateScratchPixmapsForScreen(i))
            FatalError("failed to create scratch pixmaps");
        if (pScreen->CreateScreenResources &&
            !(*pScreen->CreateScreenResources) (pScreen))
            FatalError("failed to create screen resources");
        if (!CreateGCperDepth(i))
            FatalError("failed to create scratch GCs");
        if (!CreateDefaultStipple(i))
            FatalError("failed to create default stipple");
        if (!CreateRootWindow(pScreen))
            FatalError("failed to create root window");
    }

    InitInput(argc, argv);
    if (InitAndStartDevices() != Success)
        FatalError("failed to initialize core devices");

    InitFonts();
    if (SetDefaultFontPath(defaultFontPath) != Success)
        ErrorF("failed to set default font path '%s'", defaultFontPath);
    if (!SetDefaultFont(defaultTextFont))
        FatalError("could not open default font '%s'", defaultTextFont);
    if (!(rootCursor = CreateRootCursor(defaultCursorFont, 0)))
        FatalError("could not open default cursor font '%s'",
                   defaultCursorFont);

#ifdef DPMSExtension
    /* Check all screens for DPMS capabilities. */
    DPMSCapableFlag = DPMSSupported();
    if (!DPMSCapableFlag)
        DPMSEnabled = FALSE;
#endif

    for (i = 0; i < screenInfo.numScreens; i++)
        InitRootWindow(WindowTable[i]);
    DefineInitialRootWindow(WindowTable[0]);
    SaveScreens(SCREEN_SAVER_FORCER, ScreenSaverReset);

    if (!CreateConnectionBlock())
        FatalError("could not create connection block info");
}

void
TinyXServerDispatchGeneration(void)
{
    Dispatch();
}

Bool
TinyXServerCloseGeneration(void)
{
    Bool terminating = (dispatchException & DE_TERMINATE) != 0;
    int i;

    if (screenIsSaved == SCREEN_SAVER_ON)
        SaveScreens(SCREEN_SAVER_OFF, ScreenSaverReset);
    FreeScreenSaverTimer();
    CloseDownExtensions();
    FreeAllResources();

    memset(WindowTable, 0, sizeof(WindowTable));
    CloseDownDevices();
    for (i = screenInfo.numScreens - 1; i >= 0; i--) {
        FreeScratchPixmapsForScreen(i);
        FreeGCperDepth(i);
        FreeDefaultStipple(i);
        (*screenInfo.screens[i]->CloseScreen) (i, screenInfo.screens[i]);
        FreeScreen(screenInfo.screens[i]);
        screenInfo.numScreens = i;
    }
    CloseDownEvents();
    FreeFonts();

    free(serverClient->devPrivates);
    serverClient->devPrivates = NULL;

    if (terminating)
        CloseWellKnownConnections();

    OsCleanup(terminating);

    if (!terminating) {
        free(ConnectionInfo);
        ConnectionInfo = NULL;
    }

    return terminating;
}

void
TinyXServerShutdown(void)
{
    ddxGiveUp();
}
