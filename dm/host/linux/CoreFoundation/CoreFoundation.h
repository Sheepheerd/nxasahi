// Linux build: the CoreFoundation the host uses -- CFMutableDictionary with NULL callbacks (pointer identity keys and values, hle_nvn.c's program key table) and CFRelease
#pragma once
#include <darwin_compat.h>
typedef const void *CFTypeRef;
typedef long CFIndex;
typedef const void *CFAllocatorRef;
typedef struct dm_cfdict *CFMutableDictionaryRef;
typedef const struct dm_cfdict *CFDictionaryRef;
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef a, CFIndex cap, const void *keycb, const void *valcb);   // callbacks must be NULL
const void *CFDictionaryGetValue(CFDictionaryRef d, const void *key);
void CFDictionarySetValue(CFMutableDictionaryRef d, const void *key, const void *value);
void CFDictionaryRemoveValue(CFMutableDictionaryRef d, const void *key);
CFIndex CFDictionaryGetCount(CFDictionaryRef d);
void CFRelease(CFTypeRef o);
