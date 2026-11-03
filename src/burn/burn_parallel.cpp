// Burn - parallel parts (FBNeo PS5 2.3): see burn_parallel.h
#include "burnint.h"
#include "burn_parallel.h"

void (*BurnParallelRunHook)(BurnParallelJob job, void* ctx, INT32 parts) = NULL;
INT32 nBurnParallelThreads = 1;

void BurnParallelRun(BurnParallelJob job, void* ctx, INT32 parts)
{
	if (parts <= 0)
		return;
	if (BurnParallelRunHook && nBurnParallelThreads > 1 && parts > 1) {
		BurnParallelRunHook(job, ctx, parts);
		return;
	}
	for (INT32 i = 0; i < parts; i++)
		job(ctx, i);
}
