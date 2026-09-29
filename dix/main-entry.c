/* Native process entry point. Kept separate so embedders may provide main(). */
#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include "lifecycle.h"

int
main(int argc, char *argv[], char *envp[])
{
    TinyXServerInitialize(argc, argv, envp);

    do {
        TinyXServerInitializeGeneration(argc, argv);
        TinyXServerDispatchGeneration();
    } while (!TinyXServerCloseGeneration());

    TinyXServerShutdown();
    return 0;
}
