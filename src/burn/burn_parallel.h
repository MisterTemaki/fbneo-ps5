// Burn - parallel parts (FBNeo PS5 2.3): a driver's work that splits into independent parts (Sega Model 2's 3D, by
// bands of lines), run on the frontend's worker threads when it has some. The core has no threads of its own:
// without a frontend hook, or with one thread, the parts run in turn on the caller. A part must not touch what
// another part touches.
#ifndef BURN_PARALLEL_H
#define BURN_PARALLEL_H

typedef void (*BurnParallelJob)(void* ctx, INT32 part);

// set by the frontend: runs job(ctx, i) for each i < parts, on the caller and its workers, and returns when all are done
extern void (*BurnParallelRunHook)(BurnParallelJob job, void* ctx, INT32 parts);
// how many threads the hook has (the caller's included); 1 without a hook
extern INT32 nBurnParallelThreads;

void BurnParallelRun(BurnParallelJob job, void* ctx, INT32 parts);

#endif
