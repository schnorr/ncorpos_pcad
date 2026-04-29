/*
This file is part of "NCorpos @ PCAD".

"NCorpos @ PCAD" is free software: you can redistribute it and/or
modify it under the terms of the GNU General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

Graphical client for the N-body simulation.
Inspired by grafica.c from Fractal @ PCAD.
Uses raylib for window management and rendering.

Thread model:
  - main thread (raylib)    : handle_input + draw
  - render_thread            : dequeue responses → update particle positions
  - net_thread_send_payload  : dequeue payload → TCP send
  - net_thread_receive_response : TCP recv → enqueue response

Controls:
  N       – start a new simulation with current settings
  P +/-   – increase / decrease particle count
  I +/-   – increase / decrease iteration count
  S +/-   – increase / decrease space scale (1 pixel = N km)
  ESC     – quit
*/
#include <raylib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <netdb.h>
#include <sys/types.h>
#include <signal.h>
#include <stdatomic.h>
#include <math.h>
#include <time.h>

#include "ncorpos.h"
#include "galaxy_ic.h"
#include "connection.h"
#include "queue.h"

/* ---------------------------------------------------------------
 * Utility macros
 * --------------------------------------------------------------- */
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

/* ---------------------------------------------------------------
 * Global state
 * --------------------------------------------------------------- */
atomic_int shutdown_requested;

static queue_t payload_queue  = {0};
static queue_t response_queue = {0};

/* Current particle positions on screen (updated by render_thread) */
static pthread_mutex_t g_particles_mutex = PTHREAD_MUTEX_INITIALIZER;
static Vector2 *g_screen_positions = NULL; /* [max_particles] */
static int     *g_particle_ids      = NULL; /* [max_particles] */
static double  *g_particle_masses   = NULL; /* [max_particles] */
static int      g_num_particles     = 0;
static bool     g_particles_changed = false;

/* Simulation settings (modified only from main/UI thread) */
static int    g_num_particles_setting = 2500;
static int    g_num_iterations        = 500;
static double g_space_size            = GALAXY_RADIUS * 2.0; /* ~40 kpc diameter */
static int    g_max_workers           = 1;
static float  g_show_settings_timer  = 0.0f;

/* ---------------------------------------------------------------
 * Space ↔ screen coordinate mapping
 * --------------------------------------------------------------- */
static Vector2 space_to_screen(double x, double y,
                                double space_w, double space_h,
                                int    screen_w, int    screen_h)
{
  return (Vector2){
    .x = (float)((x / space_w + 0.5) * screen_w),
    .y = (float)((y / space_h + 0.5) * screen_h)
  };
}

/* ---------------------------------------------------------------
 * Particle display radius — proportional to mass on a log scale.
 *
 * The BH (id == 0) gets a fixed prominent radius (6 px).
 * All other particles are mapped from log10(mass) to [1.0, 4.0] px
 * using a reference range of [1e37, 1e42] kg, which covers the
 * full range of per-particle masses for any reasonable N.
 * --------------------------------------------------------------- */
static float particle_radius(double mass, int id)
{
  if (id == 0)
    return 6.0f; /* central black hole — always prominent */
  double t = (log10(mass) - 37.0) / (42.0 - 37.0);
  if (t < 0.0) t = 0.0;
  if (t > 1.0) t = 1.0;
  return (float)(1.0 + t * 3.0); /* [1.0, 4.0] px */
}

/* ---------------------------------------------------------------
 * Shutdown helper
 * --------------------------------------------------------------- */
static void request_shutdown(int connection)
{
  if (atomic_exchange(&shutdown_requested, 1) == 1)
    return;
  printf("Shutdown requested.\n");
  shutdown(connection, SHUT_RDWR);
  queue_enqueue(&payload_queue,  NULL);
  queue_enqueue(&response_queue, NULL);
}

/* ---------------------------------------------------------------
 * render_thread_function
 *
 * Dequeues responses from the coordinator and updates the shared
 * g_screen_positions array so the main thread can draw them.
 * --------------------------------------------------------------- */
static void *render_thread_function(void *arg)
{
  (void)arg;

  while (!atomic_load(&shutdown_requested)) {
    response_t *r = (response_t *)queue_dequeue(&response_queue);
    if (r == NULL) break; /* poison pill */

    pthread_mutex_lock(&g_particles_mutex);

    /* Resize position buffer if needed */
    int total = r->first_particle + r->num_particles_slice;
    if (total > g_num_particles) {
      g_screen_positions = realloc(g_screen_positions,
                                   (size_t)total * sizeof(Vector2));
      g_particle_ids     = realloc(g_particle_ids,
                                   (size_t)total * sizeof(int));
      g_particle_masses  = realloc(g_particle_masses,
                                   (size_t)total * sizeof(double));
      if (!g_screen_positions || !g_particle_ids || !g_particle_masses) {
        perror("realloc failed");
        pthread_mutex_unlock(&g_particles_mutex);
        free_response(r);
        break;
      }
      g_num_particles = total;
    }

    int screen_w = GetScreenWidth();
    int screen_h = GetScreenHeight();

    for (int i = 0; i < r->num_particles_slice; i++) {
      int idx = r->first_particle + i;
      particle_t *pt = &r->particles[i];

      g_screen_positions[idx] =
        space_to_screen(pt->x, pt->y,
                        g_space_size, g_space_size,
                        screen_w, screen_h);
      g_particle_ids[idx]    = pt->id;
      g_particle_masses[idx] = pt->mass;
    }
    g_particles_changed = true;
    pthread_mutex_unlock(&g_particles_mutex);

    free_response(r);
  }
  pthread_exit(NULL);
}

/* ---------------------------------------------------------------
 * net_thread_send_payload
 * --------------------------------------------------------------- */
static void *net_thread_send_payload(void *arg)
{
  int connection = *(int *)arg;

  while (!atomic_load(&shutdown_requested)) {
    payload_t *p = (payload_t *)queue_dequeue(&payload_queue);
    if (p == NULL) break;

    if (send(connection, p, sizeof(payload_t), 0) <= 0) {
      fprintf(stderr, "Send failed (payload header). Killing thread.\n");
      free_payload(p);
      pthread_exit(NULL);
    }

    if (p->generation != PAYLOAD_GENERATION_SHUTDOWN && p->particles) {
      size_t buf = (size_t)p->num_particles * sizeof(particle_t);
      if (send(connection, p->particles, buf, 0) <= 0) {
        fprintf(stderr, "Send failed (particles). Killing thread.\n");
        free_payload(p);
        pthread_exit(NULL);
      }
      printf("(%d) %s: payload sent (%d particles, %d iters).\n",
             p->generation, __func__, p->num_particles, p->num_iterations);
    }
    free_payload(p);
  }
  pthread_exit(NULL);
}

/* ---------------------------------------------------------------
 * net_thread_receive_response
 * --------------------------------------------------------------- */
static void *net_thread_receive_response(void *arg)
{
  int connection = *(int *)arg;

  while (!atomic_load(&shutdown_requested)) {
    response_t *r = calloc(1, sizeof(response_t));
    if (!r) { fprintf(stderr, "calloc failed\n"); pthread_exit(NULL); }

    if (recv_all(connection, r, sizeof(response_t), 0) <= 0) {
      fprintf(stderr, "Receive failed (response header). Killing thread.\n");
      free(r);
      pthread_exit(NULL);
    }

    int slice = r->num_particles_slice;
    r->particles = malloc((size_t)slice * sizeof(particle_t));
    if (!r->particles) {
      fprintf(stderr, "malloc failed\n");
      free(r);
      pthread_exit(NULL);
    }

    if (recv_all(connection, r->particles,
                 (size_t)slice * sizeof(particle_t), 0) <= 0) {
      fprintf(stderr, "Receive failed (particle array). Killing thread.\n");
      free_response(r);
      pthread_exit(NULL);
    }

    queue_enqueue(&response_queue, r);
    r = NULL;
  }
  pthread_exit(NULL);
}

/* ---------------------------------------------------------------
 * build_payload  –  create a galaxy initial condition for simulation
 * --------------------------------------------------------------- */
static payload_t *build_payload(int screen_width, int screen_height)
{
  payload_t *p = calloc(1, sizeof(payload_t));
  if (!p) { perror("calloc"); exit(1); }

  static int generation = 0;
  p->generation    = ++generation;
  p->num_particles = g_num_particles_setting;
  p->num_iterations= g_num_iterations;
  p->num_workers   = g_max_workers;
  p->space_width   = g_space_size;
  p->space_height  = g_space_size;
  p->screen_width  = screen_width;
  p->screen_height = screen_height;

  p->particles = malloc((size_t)p->num_particles * sizeof(particle_t));
  if (!p->particles) { perror("malloc"); exit(1); }

  generate_galaxy_ic(p->particles, p->num_particles, (long)time(NULL));

  return p;
}

/* ---------------------------------------------------------------
 * handle_input  –  process keyboard input and dispatch payloads
 * --------------------------------------------------------------- */
static void handle_input(int screen_width, int screen_height)
{
  float dt = GetFrameTime();

  /* N – send a new simulation request */
  if (IsKeyPressed(KEY_N)) {
    payload_t *p = build_payload(screen_width, screen_height);
    payload_print(__func__, "Enqueueing payload", p);
    queue_enqueue(&payload_queue, p);
    p = NULL;
  }

  /* P +/- – particle count */
  if (IsKeyDown(KEY_P)) {
    float change = (float)g_num_particles_setting * dt;
    if (change < 1.0f) change = 1.0f;
    if (IsKeyDown(KEY_EQUAL))
      g_num_particles_setting += (int)change;
    if (IsKeyDown(KEY_MINUS))
      g_num_particles_setting -= (int)change;
    g_num_particles_setting = (int)CLAMP(g_num_particles_setting,
                                         NCORPOS_MIN_PARTICLES,
                                         NCORPOS_MAX_PARTICLES);
    g_show_settings_timer = 2.0f;
  }

  /* I +/- – iteration count */
  if (IsKeyDown(KEY_I)) {
    float change = (float)g_num_iterations * dt;
    if (change < 1.0f) change = 1.0f;
    if (IsKeyDown(KEY_EQUAL))
      g_num_iterations += (int)change;
    if (IsKeyDown(KEY_MINUS))
      g_num_iterations -= (int)change;
    g_num_iterations = (int)CLAMP(g_num_iterations, 1, 1000000);
    g_show_settings_timer = 2.0f;
  }

  /* S +/- – space scale */
  if (IsKeyDown(KEY_S)) {
    double factor = IsKeyDown(KEY_EQUAL) ? 1.0 + (double)dt
                                         : 1.0 - (double)dt;
    g_space_size = CLAMP(g_space_size * factor, 1.0e15, 1.0e22);
    g_show_settings_timer = 2.0f;
  }

  if (g_show_settings_timer > 0.0f)
    g_show_settings_timer -= dt;
}

/* ---------------------------------------------------------------
 * main
 * --------------------------------------------------------------- */
int main(int argc, char *argv[])
{
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <host> <port>\n", argv[0]);
    return 1;
  }

  int connection = open_connection(argv[1], (uint16_t)atoi(argv[2]));

  atomic_init(&shutdown_requested, 0);
  signal(SIGPIPE, SIG_IGN);

  /* Handshake: discover worker count */
  if (recv_all(connection, &g_max_workers, sizeof(int), 0) <= 0) {
    fprintf(stderr, "Failed to receive worker count.\n");
    return 1;
  }
  printf("Coordinator has %d workers.\n", g_max_workers);

  /* Init raylib */
  SetConfigFlags(FLAG_FULLSCREEN_MODE);
  InitWindow(0, 0, "NCorpos @ PCAD");
  int screen_width  = GetScreenWidth();
  int screen_height = GetScreenHeight();
  SetWindowSize(screen_width, screen_height);
  ToggleFullscreen();
  SetTargetFPS(60);

  queue_init(&payload_queue,  1,      free_payload);
  queue_init(&response_queue, 65536,  free_response);

  pthread_mutex_init(&g_particles_mutex, NULL);

  pthread_t render_thread   = 0;
  pthread_t payload_thread  = 0;
  pthread_t response_thread = 0;

  pthread_create(&render_thread,   NULL, render_thread_function,     NULL);
  pthread_create(&payload_thread,  NULL, net_thread_send_payload,    &connection);
  pthread_create(&response_thread, NULL, net_thread_receive_response, &connection);

  /* Draw loop */
  while (!WindowShouldClose()) {
    handle_input(screen_width, screen_height);

    BeginDrawing();
    ClearBackground(BLACK);

    /* Draw particles */
    pthread_mutex_lock(&g_particles_mutex);
    if (g_particles_changed) {
      for (int i = 0; i < g_num_particles; i++) {
        int    pid  = g_particle_ids[i];
        double mass = g_particle_masses[i];
        float  rad  = particle_radius(mass, pid);
        Color  c;
        if (pid == 0) {
          c = WHITE; /* central black hole */
        } else {
          unsigned char hue = (unsigned char)((pid * 37) & 0xFF);
          c = ColorFromHSV((float)hue / 255.0f * 360.0f, 0.9f, 0.9f);
        }
        DrawCircleV(g_screen_positions[i], rad, c);
      }
    }
    pthread_mutex_unlock(&g_particles_mutex);

    /* HUD */
    DrawText("Press N to start a new simulation", 10, 10, 20, DARKGRAY);
    DrawText(TextFormat("Workers: %d", g_max_workers),       10, 35, 20, DARKGRAY);
    DrawText(TextFormat("Particles: %d  (P+/-)", g_num_particles_setting),
             10, 60, 20, DARKGRAY);
    DrawText(TextFormat("Iterations: %d  (I+/-)", g_num_iterations),
             10, 85, 20, DARKGRAY);
    DrawText(TextFormat("Space: %.2e m  (S+/-)", g_space_size),
             10, 110, 20, DARKGRAY);

    if (g_show_settings_timer > 0.0f) {
      DrawText(TextFormat("Particles: %d", g_num_particles_setting),
               screen_width / 4, screen_height / 2 - 30, 60, WHITE);
      DrawText(TextFormat("Iterations: %d", g_num_iterations),
               screen_width / 4, screen_height / 2 + 30, 60, WHITE);
    }

    EndDrawing();
  }

  /* Cleanup */
  request_shutdown(connection);

  pthread_join(render_thread,   NULL);
  pthread_join(payload_thread,  NULL);
  pthread_join(response_thread, NULL);

  CloseWindow();

  free(g_screen_positions);
  free(g_particle_ids);
  free(g_particle_masses);
  pthread_mutex_destroy(&g_particles_mutex);

  close(connection);
  queue_destroy(&payload_queue);
  queue_destroy(&response_queue);

  return 0;
}
