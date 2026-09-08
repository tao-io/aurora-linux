/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
