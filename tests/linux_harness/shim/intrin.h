#pragma once
#include <cpuid.h>
#undef __cpuid
inline void __cpuid(int r[4], int leaf){ __cpuid_count(leaf,0,r[0],r[1],r[2],r[3]); }
