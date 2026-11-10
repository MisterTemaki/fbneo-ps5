/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 MisterTemaki */
/*
 * fclose / fflush for a link with --wrap=fclose --wrap=fflush: every call in the program's own code (and the static
 * libc++) comes here first, so open_memstream's buffers are kept up to date (src/ps5rt_memstream.c). A program with
 * wrappers of its own leaves this object out (ps5rt-radv-link.sh --own-stdio-wrap) and calls ps5rt_fclose /
 * ps5rt_fflush from them.
 */
#include <stdio.h>

#include "ps5rt/platform.h"

int __wrap_fclose(FILE *stream);
int __wrap_fflush(FILE *stream);

int __wrap_fclose(FILE *stream)
{
	return ps5rt_fclose(stream);
}

int __wrap_fflush(FILE *stream)
{
	return ps5rt_fflush(stream);
}
