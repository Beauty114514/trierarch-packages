#ifndef TRIERARCH_ADRENO_AHB_CENSUS_H
#define TRIERARCH_ADRENO_AHB_CENSUS_H

#include <stdint.h>

/*
 * Read Android-owned AHardwareBuffer donors with deliberately different
 * geometries and report every geometry-dependent word in their private
 * native handles.  This is diagnostics only: it accepts no guest fd and
 * never writes donor metadata.
 */
void trierarch_adreno_ahb_census_run(uint32_t ahb_format, uint64_t usage);

#endif
