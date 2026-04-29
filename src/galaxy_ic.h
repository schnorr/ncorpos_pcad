#ifndef __GALAXY_IC_H__
#define __GALAXY_IC_H__

#include "ncorpos.h"

/*
 * generate_galaxy_ic
 *
 * Populate `particles` (array of length N) with a realistic
 * galaxy initial condition composed of:
 *   - 1 central black hole particle (index 0)
 *   - ~5%  of remaining N as bulge  (Hernquist sphere, small scale)
 *   - ~15% of remaining N as disk   (exponential surface density)
 *   - ~80% of remaining N as halo   (Hernquist sphere, large scale)
 *
 * All velocities are set to the local circular velocity derived from
 * the combined gravitational potential, plus a small random dispersion.
 *
 * Parameters:
 *   particles  - pre-allocated array of N particle_t structs
 *   N          - total number of particles (must be >= 10)
 *   seed       - random seed for reproducibility
 */
void generate_galaxy_ic(particle_t *particles, int N, long seed);

#endif /* __GALAXY_IC_H__ */
