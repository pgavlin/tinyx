/*
 * Internal server lifecycle interface.
 *
 * These functions separate the reusable server lifecycle from the native
 * process entry point.  They preserve the existing process-global singleton
 * and FatalError behavior; neither is a public embedding API.
 */
#ifndef TINYX_DIX_LIFECYCLE_H
#define TINYX_DIX_LIFECYCLE_H

#include "misc.h"

typedef struct {
    unsigned int requestsProcessed;
    Bool immediateWork;
    Bool generationFinished;
    /* Milliseconds until the next timer, or -1 when no timer is armed. */
    int nextTimeoutMillis;
} TinyXServerStepResult;

void TinyXServerInitialize(int argc, char **argv, char **envp);
void TinyXServerInitializeGeneration(int argc, char **argv);
void TinyXServerDispatchGeneration(void);
/* A requestBudget of zero means unlimited. This operation never blocks. */
void TinyXServerStep(unsigned int requestBudget, TinyXServerStepResult *result);
void TinyXServerRequestTermination(void);
Bool TinyXServerCloseGeneration(void);
void TinyXServerShutdown(void);

#endif /* TINYX_DIX_LIFECYCLE_H */
