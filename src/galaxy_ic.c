/*
This file is part of "NCorpos @ PCAD".

Galaxy initial conditions generator.
Produces a Hernquist dark-matter halo + exponential stellar disk +
Hernquist bulge + central black hole, with circular-velocity
initialisation for approximate virial equilibrium.
*/
#include <math.h>
#include <stdlib.h>
#include "galaxy_ic.h"

/* ---------------------------------------------------------------
 * Returns a Gaussian-distributed random number with mean 0
 * and standard deviation 1, using the Box-Muller transform.
 * Uses drand48() — caller must have called srand48() beforehand.
 * --------------------------------------------------------------- */
static double gaussian_random(void)
{
    double u1, u2;
    do { u1 = drand48(); } while (u1 <= 1e-10);
    u2 = drand48();
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/* ---------------------------------------------------------------
 * Hernquist enclosed mass at radius r.
 *   M_enc(r) = M_total * r^2 / (r + a)^2
 * --------------------------------------------------------------- */
static double hernquist_m_enclosed(double M_total, double a, double r)
{
    double ra = r + a;
    return M_total * (r * r) / (ra * ra);
}

/* ---------------------------------------------------------------
 * Sample a radius drawn from the Hernquist density profile.
 * Analytic inverse CDF: r = a * sqrt(u) / (1 - sqrt(u))
 * where u ~ Uniform(0, 1).
 * Rejects samples beyond r_max to keep particles in the domain.
 * --------------------------------------------------------------- */
static double sample_hernquist_r(double a, double r_max)
{
    double r;
    do {
        double u  = drand48();
        double sq = sqrt(u);
        r = a * sq / (1.0 - sq + 1e-12);
    } while (r > r_max || r <= 0.0);
    return r;
}

/* ---------------------------------------------------------------
 * Sample a radius from an exponential surface density:
 *   Sigma(r) ∝ exp(-r / h)
 * Inverse CDF: r = -h * log(1 - u), u ~ Uniform(0,1)
 * Rejects beyond r_max.
 * --------------------------------------------------------------- */
static double sample_disk_r(double h, double r_max)
{
    double r;
    do {
        double u = drand48();
        r = -h * log(1.0 - u + 1e-12);
    } while (r > r_max || r <= 0.0);
    return r;
}

/* ---------------------------------------------------------------
 * Circular velocity at radius r given the total enclosed mass
 * from all components (halo + disk approximation + bulge).
 *
 * For the disk we use an approximate enclosed mass
 *   M_disk * (1 - exp(-r/h) * (1 + r/h))
 * which is the integral of the exponential surface density.
 *
 * v_circ = sqrt(G * M_enc_total / r)
 * --------------------------------------------------------------- */
static double circular_velocity(double r,
                                 double M_halo,  double a_halo,
                                 double M_bulge, double a_bulge,
                                 double M_disk,  double h_disk,
                                 double M_bh)
{
    if (r < 1e10) return 0.0; /* avoid singularity at r=0 */

    double m_halo  = hernquist_m_enclosed(M_halo,  a_halo,  r);
    double m_bulge = hernquist_m_enclosed(M_bulge, a_bulge, r);
    double x       = r / h_disk;
    double m_disk  = M_disk * (1.0 - exp(-x) * (1.0 + x));
    double m_total = m_halo + m_bulge + m_disk + M_bh;

    return sqrt(NCORPOS_G * m_total / r);
}

/* ---------------------------------------------------------------
 * generate_galaxy_ic — public interface
 * --------------------------------------------------------------- */
void generate_galaxy_ic(particle_t *particles, int N, long seed)
{
    srand48(seed);

    int id = 0;

    /* Component particle counts */
    int n_bh    = 1;
    int n_bulge = (int)(0.05 * (N - n_bh));
    int n_disk  = (int)(0.15 * (N - n_bh));
    int n_halo  = N - n_bh - n_bulge - n_disk;

    double M_disk = GALAXY_BULGE_MASS * 3.0; /* ~3x bulge mass */

    /* ---- 0) Black hole at center ---- */
    particles[id].id   = id;
    particles[id].x    = 0.0;
    particles[id].y    = 0.0;
    particles[id].vx   = 0.0;
    particles[id].vy   = 0.0;
    particles[id].ax   = 0.0;
    particles[id].ay   = 0.0;
    particles[id].mass = GALAXY_BH_MASS;
    id++;

    /* ---- 1) Halo particles (Hernquist profile) ---- */
    double m_per_halo = GALAXY_HALO_MASS / n_halo;
    for (int i = 0; i < n_halo; i++, id++) {
        double r     = sample_hernquist_r(GALAXY_HALO_SCALE, GALAXY_RADIUS);
        double theta = drand48() * 2.0 * M_PI;
        double vc    = circular_velocity(r,
                           GALAXY_HALO_MASS,  GALAXY_HALO_SCALE,
                           GALAXY_BULGE_MASS, GALAXY_BULGE_SCALE,
                           M_disk,            GALAXY_DISK_SCALE,
                           GALAXY_BH_MASS);

        particles[id].id   = id;
        particles[id].x    = r * cos(theta);
        particles[id].y    = r * sin(theta);
        /* circular orbit + 15% velocity dispersion */
        particles[id].vx   = -vc * sin(theta) + gaussian_random() * 0.15 * vc;
        particles[id].vy   =  vc * cos(theta) + gaussian_random() * 0.15 * vc;
        particles[id].ax   = 0.0;
        particles[id].ay   = 0.0;
        particles[id].mass = m_per_halo;
    }

    /* ---- 2) Disk particles (exponential surface density) ---- */
    double m_per_disk = M_disk / n_disk;
    for (int i = 0; i < n_disk; i++, id++) {
        double r     = sample_disk_r(GALAXY_DISK_SCALE, GALAXY_RADIUS);
        double theta = drand48() * 2.0 * M_PI;
        double vc    = circular_velocity(r,
                           GALAXY_HALO_MASS,  GALAXY_HALO_SCALE,
                           GALAXY_BULGE_MASS, GALAXY_BULGE_SCALE,
                           M_disk,            GALAXY_DISK_SCALE,
                           GALAXY_BH_MASS);

        particles[id].id   = id;
        particles[id].x    = r * cos(theta);
        particles[id].y    = r * sin(theta);
        /* disk is kinematically cold — only 8% dispersion */
        particles[id].vx   = -vc * sin(theta) + gaussian_random() * 0.08 * vc;
        particles[id].vy   =  vc * cos(theta) + gaussian_random() * 0.08 * vc;
        particles[id].ax   = 0.0;
        particles[id].ay   = 0.0;
        particles[id].mass = m_per_disk;
    }

    /* ---- 3) Bulge particles (Hernquist, small scale) ---- */
    double m_per_bulge = GALAXY_BULGE_MASS / n_bulge;
    for (int i = 0; i < n_bulge; i++, id++) {
        double r     = sample_hernquist_r(GALAXY_BULGE_SCALE,
                                          GALAXY_DISK_SCALE); /* confined */
        double theta = drand48() * 2.0 * M_PI;
        double vc    = circular_velocity(r,
                           GALAXY_HALO_MASS,  GALAXY_HALO_SCALE,
                           GALAXY_BULGE_MASS, GALAXY_BULGE_SCALE,
                           M_disk,            GALAXY_DISK_SCALE,
                           GALAXY_BH_MASS);

        particles[id].id   = id;
        particles[id].x    = r * cos(theta);
        particles[id].y    = r * sin(theta);
        /* bulge is pressure-supported — high dispersion, low net rotation */
        particles[id].vx   = gaussian_random() * 0.5 * vc;
        particles[id].vy   = gaussian_random() * 0.5 * vc;
        particles[id].ax   = 0.0;
        particles[id].ay   = 0.0;
        particles[id].mass = m_per_bulge;
    }
}
