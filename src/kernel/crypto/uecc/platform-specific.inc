/* Copyright 2015, Kenneth MacKay. Licensed under the BSD 2-clause license. */
/* AetherOS: use the kernel's RNG hook instead of host OS entropy. */

#ifndef _UECC_PLATFORM_SPECIFIC_H_
#define _UECC_PLATFORM_SPECIFIC_H_

#include "types.h"

extern "C" int aether_random_bytes(uint8_t *dest, unsigned size);
#define default_RNG aether_random_bytes
#define default_RNG_defined 1

#endif /* _UECC_PLATFORM_SPECIFIC_H_ */
