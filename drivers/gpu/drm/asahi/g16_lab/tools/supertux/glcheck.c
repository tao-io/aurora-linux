/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Observe the context SDL actually made current, without replacing any GL
 * implementation or altering the result of a draw. This is linked only into
 * the opt-in lab game launcher. */
void *SDL_GL_CreateContext(void *window)
{
   void *(*create)(void *) = dlsym(RTLD_NEXT, "SDL_GL_CreateContext");
   void *(*getproc)(const char *) = dlsym(RTLD_NEXT, "SDL_GL_GetProcAddress");
   if (!create || !getproc) {
      fputs("m4-supertux: SDL GL entry points unavailable\n", stderr);
      exit(1);
   }
   void *ctx = create(window);
   if (!ctx)
      return ctx;
   const unsigned char *(*getstring)(unsigned) = getproc("glGetString");
   const char *renderer = getstring ? (const char *)getstring(0x1f01) : NULL;
   const char *version = getstring ? (const char *)getstring(0x1f02) : NULL;
   fprintf(stderr, "m4-supertux: GL_RENDERER=%s\nm4-supertux: GL_VERSION=%s\n",
           renderer ? renderer : "(null)", version ? version : "(null)");
   if (!renderer || !strstr(renderer, "zink") ||
       !(strstr(renderer, "M4") || strstr(renderer, "T8132")) ||
       strstr(renderer, "llvmpipe") || strstr(renderer, "lavapipe")) {
      fputs("m4-supertux: expected M4 Zink hardware context\n", stderr);
      exit(1);
   }
   return ctx;
}

/* Wall-clock application swap cadence, including rendering and blocking in
 * SDL/EGL. This observes real swaps; it is not a scanout presentation counter. */
void SDL_GL_SwapWindow(void *window)
{
   static void (*swap)(void *);
   static double start;
   static double swap_seconds, cpu_start;
   static unsigned frames;
   struct timespec now, before, cpu;
   if (!swap) {
      swap = dlsym(RTLD_NEXT, "SDL_GL_SwapWindow");
      if (!swap) {
         fputs("m4-supertux: SDL swap entry point unavailable\n", stderr);
         exit(1);
      }
   }
   int before_error = clock_gettime(CLOCK_MONOTONIC, &before);
   swap(window);
   if (before_error || clock_gettime(CLOCK_MONOTONIC, &now) ||
       clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu))
      return;
   double end = now.tv_sec + now.tv_nsec * 1e-9;
   double cpu_end = cpu.tv_sec + cpu.tv_nsec * 1e-9;
   if (!start) {
      start = end;
      cpu_start = cpu_end;
      return;
   }
   swap_seconds += end - (before.tv_sec + before.tv_nsec * 1e-9);
   if (++frames == 120) {
      fprintf(stderr, "m4-supertux: 120 swaps in %.3f s = %.2f FPS (wall clock); "
              "mean frame %.2f ms, in SDL swap %.2f ms, main-thread CPU %.2f ms\n",
              end - start, frames / (end - start), (end - start) * 1000 / frames,
              swap_seconds * 1000 / frames, (cpu_end - cpu_start) * 1000 / frames);
      start = end;
      cpu_start = cpu_end;
      swap_seconds = 0;
      frames = 0;
   }
}
