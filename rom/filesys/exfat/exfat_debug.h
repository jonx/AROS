/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#ifdef __AROS__
#include <aros/debug.h>
#else
#if DEBUG > 0
#include <clib/debug_protos.h>
#define D(x) x
#define bug kprintf
#else
#define D(x)
#endif
#if DEBUG > 1
#define DB2(x) x
#else
#define DB2(x)
#endif
#endif
