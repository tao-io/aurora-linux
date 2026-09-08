/* SPDX-License-Identifier: MIT */
/* GLES 3.0 compositing gate for the Hyprland -> Zink -> Vulkan workstream.
 * Tests sampled surfaces, premultiplied alpha, scissor damage, reuse and
 * exported EGL native fences. This is not the Hyprland compositor itself.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s (EGL %#x, GL %#x)\n", __LINE__, #x, eglGetError(), glGetError()); exit(1); } } while (0)
#define SIDE 32

static GLuint compile(GLenum stage, const char *source)
{
   GLuint shader = glCreateShader(stage);
   glShaderSource(shader, 1, &source, NULL);
   glCompileShader(shader);
   GLint ok;
   glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[4096];
      glGetShaderInfoLog(shader, sizeof(log), NULL, log);
      fprintf(stderr, "%s\n", log);
   }
   CHECK(ok);
   return shader;
}

static GLuint texture(const void *pixels, GLint filter)
{
   GLuint id;
   glGenTextures(1, &id);
   glBindTexture(GL_TEXTURE_2D, id);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   return id;
}

static float linear_sample(const uint8_t pixels[4][4], unsigned x, unsigned y, unsigned c)
{
   float u = fminf(1, fmaxf(0, ((x + 0.5f) / SIDE) * 2 - 0.5f));
   float v = fminf(1, fmaxf(0, ((y + 0.5f) / SIDE) * 2 - 0.5f));
   return ((pixels[0][c] * (1-u) + pixels[1][c] * u) * (1-v) +
           (pixels[2][c] * (1-u) + pixels[3][c] * u) * v);
}

static void json_string(const char *s)
{
   putchar('"');
   for (; *s; ++s) {
      if (*s == '"' || *s == '\\') putchar('\\');
      if ((unsigned char)*s < 32) printf("\\u%04x", (unsigned char)*s);
      else putchar(*s);
   }
   putchar('"');
}

static GLuint shared_texture(EGLDisplay display, EGLContext context, GLuint source)
{
   PFNEGLCREATEIMAGEKHRPROC create = (void *)eglGetProcAddress("eglCreateImageKHR");
   PFNEGLDESTROYIMAGEKHRPROC destroy = (void *)eglGetProcAddress("eglDestroyImageKHR");
   PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC query = (void *)eglGetProcAddress("eglExportDMABUFImageQueryMESA");
   PFNEGLEXPORTDMABUFIMAGEMESAPROC export = (void *)eglGetProcAddress("eglExportDMABUFImageMESA");
   PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
   CHECK(create && destroy && query && export && bind);
   glFinish(); /* Complete the source upload before the external handoff. */
   EGLImageKHR image = create(display, context, EGL_GL_TEXTURE_2D_KHR,
                              (EGLClientBuffer)(uintptr_t)source, NULL);
   CHECK(image != EGL_NO_IMAGE_KHR);
   EGLint fourcc, planes, stride, offset;
   EGLuint64KHR modifier;
   CHECK(query(display, image, &fourcc, &planes, &modifier) && planes == 1);
   int fd = -1;
   CHECK(export(display, image, &fd, &stride, &offset) && fd >= 0);
   const EGLint attrs[] = {
      EGL_WIDTH, 2, EGL_HEIGHT, 2, EGL_LINUX_DRM_FOURCC_EXT, fourcc,
      EGL_DMA_BUF_PLANE0_FD_EXT, fd, EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT, offset,
      EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(uint32_t)modifier,
      EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(uint32_t)(modifier >> 32), EGL_NONE,
   };
   EGLImageKHR imported = create(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
   CHECK(imported != EGL_NO_IMAGE_KHR && close(fd) == 0);
   GLuint alias;
   glGenTextures(1, &alias); glBindTexture(GL_TEXTURE_2D, alias);
   bind(GL_TEXTURE_2D, imported);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   CHECK(glGetError() == GL_NO_ERROR);
   CHECK(destroy(display, imported) && destroy(display, image));
   return alias; /* GL retains the imported storage after EGLImage/fd close. */
}

int main(int argc, char **argv)
{
   bool development = argc == 2 && !strcmp(argv[1], "--development-gpu");
   bool local = argc == 2 && !strcmp(argv[1], "--local-textures");
   CHECK(argc == 1 || development || local);
   EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   CHECK(display != EGL_NO_DISPLAY && eglInitialize(display, NULL, NULL));
   const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
   CHECK(extensions && strstr(extensions, "EGL_KHR_no_config_context") &&
         strstr(extensions, "EGL_KHR_surfaceless_context") &&
         strstr(extensions, "EGL_ANDROID_native_fence_sync") &&
         strstr(extensions, "EGL_MESA_image_dma_buf_export") &&
         strstr(extensions, "EGL_EXT_image_dma_buf_import_modifiers"));
   CHECK(eglBindAPI(EGL_OPENGL_ES_API));
   EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
   EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
   CHECK(context != EGL_NO_CONTEXT && eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context));
   const char *renderer = (const char *)glGetString(GL_RENDERER);
   const char *version = (const char *)glGetString(GL_VERSION);
   fprintf(stderr, "renderer: %s\nversion: %s\n", renderer, version);
   CHECK(renderer && strstr(renderer, "zink") && !strstr(renderer, "llvmpipe") &&
         !strstr(renderer, "lavapipe") && !strstr(renderer, "softpipe"));
   CHECK(development || strstr(renderer, "M4") || strstr(renderer, "T8132"));
   PFNEGLCREATESYNCKHRPROC create_sync = (void *)eglGetProcAddress("eglCreateSyncKHR");
   PFNEGLDUPNATIVEFENCEFDANDROIDPROC export_sync = (void *)eglGetProcAddress("eglDupNativeFenceFDANDROID");
   PFNEGLDESTROYSYNCKHRPROC destroy_sync = (void *)eglGetProcAddress("eglDestroySyncKHR");
   PFNEGLWAITSYNCKHRPROC wait_sync = (void *)eglGetProcAddress("eglWaitSyncKHR");
   CHECK(create_sync && export_sync && destroy_sync && wait_sync);

   const char *vs = "#version 300 es\nlayout(location=0) in vec2 pos;\n"
      "layout(location=1) in vec2 uv; out vec2 coord;\n"
      "void main(){gl_Position=vec4(pos,0,1);coord=uv;}\n";
   const char *fs = "#version 300 es\nprecision highp float;\n"
      "in vec2 coord; uniform sampler2D tex; uniform float opacity;\n"
      "layout(location=0) out vec4 color;\n"
      "void main(){color=texture(tex,coord)*opacity;}\n";
   GLuint vertex = compile(GL_VERTEX_SHADER, vs), fragment = compile(GL_FRAGMENT_SHADER, fs);
   GLuint program = glCreateProgram();
   glAttachShader(program, vertex); glAttachShader(program, fragment); glLinkProgram(program);
   GLint linked; glGetProgramiv(program, GL_LINK_STATUS, &linked); CHECK(linked);
   glUseProgram(program);
   GLint opacity = glGetUniformLocation(program, "opacity");
   CHECK(opacity >= 0);
   glUniform1i(glGetUniformLocation(program, "tex"), 0);
   const float quad[] = {-1,-1,0,0, 1,-1,1,0, -1,1,0,1,
                         -1,1,0,1, 1,-1,1,0, 1,1,1,1};
   GLuint vao, vbo; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
   glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), NULL);
   glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void *)(2*sizeof(float)));
   glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
   uint8_t base[4][4] = {{255,0,0,255}, {0,255,0,255}, {0,0,255,255}, {255,255,255,255}};
   const uint8_t over[4][4] = {{64,0,0,128}, {0,96,0,128}, {0,0,32,64}, {32,32,32,64}};
   GLuint base_tex = texture(base, GL_NEAREST), over_tex = texture(over, GL_LINEAR), target, fbo;
   GLuint imported_over = local ? over_tex : shared_texture(display, context, over_tex);
   glGenTextures(1, &target); glBindTexture(GL_TEXTURE_2D, target);
   glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, SIDE, SIDE);
   glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
   CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
   glViewport(0, 0, SIDE, SIDE);
   glDisable(GL_DITHER);
   uint8_t readback[SIDE*SIDE][4];
   unsigned max_error = 0;
   for (unsigned frame = 0; frame < 8; ++frame) {
      base[0][0] = frame & 1 ? 0 : 255;
      base[0][2] = frame & 1 ? 255 : 0;
      glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND);
      glClearColor(1, 0, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
      glBindTexture(GL_TEXTURE_2D, base_tex);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, base);
      glUniform1f(opacity, 1); glDrawArrays(GL_TRIANGLES, 0, 6);
      glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      glEnable(GL_SCISSOR_TEST); glScissor(8, 8, 16, 16);
      float alpha = frame & 1 ? 0.5f : 1.0f;
      glBindTexture(GL_TEXTURE_2D, imported_over); glUniform1f(opacity, alpha);
      glDrawArrays(GL_TRIANGLES, 0, 6);
      CHECK(glGetError() == GL_NO_ERROR);
      EGLSyncKHR sync = create_sync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
      CHECK(sync != EGL_NO_SYNC_KHR);
      glFlush();
      int fd = export_sync(display, sync); CHECK(fd >= 0);
      int imported_fd = dup(fd); CHECK(imported_fd >= 0);
      const EGLint sync_attrs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, imported_fd, EGL_NONE};
      EGLSyncKHR incoming = create_sync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, sync_attrs);
      CHECK(incoming != EGL_NO_SYNC_KHR && wait_sync(display, incoming, 0));
      struct pollfd p = {.fd = fd, .events = POLLIN};
      CHECK(poll(&p, 1, 3000) == 1 && p.revents & POLLIN);
      CHECK(close(fd) == 0 && destroy_sync(display, incoming) && destroy_sync(display, sync));
      glReadPixels(0, 0, SIDE, SIDE, GL_RGBA, GL_UNSIGNED_BYTE, readback);
      CHECK(glGetError() == GL_NO_ERROR);
      for (unsigned y = 0; y < SIDE; ++y) {
         for (unsigned x = 0; x < SIDE; ++x) {
            unsigned texel = (y >= 16) * 2 + (x >= 16);
            for (unsigned c = 0; c < 4; ++c) {
               float expected = base[texel][c];
               if (x >= 8 && x < 24 && y >= 8 && y < 24) {
                  float a = linear_sample(over, x, y, 3) * alpha / 255;
                  expected = linear_sample(over, x, y, c) * alpha + expected * (1-a);
               }
               unsigned error = abs((int)readback[y*SIDE+x][c] - (int)lroundf(expected));
               if (error > max_error) max_error = error;
               if (error > 1) {
                  fprintf(stderr, "frame=%u x=%u y=%u channel=%u actual=%u expected=%f\n",
                          frame, x, y, c, readback[y*SIDE+x][c], expected);
                  exit(1);
               }
            }
         }
      }
   }
   printf("{\"passed\":true,\"scope\":\"%s\",\"renderer\":", development ? "development GPU" : "M4");
   json_string(renderer);
   printf(",\"frames\":8,\"pixels_verified\":8192,\"max_channel_error\":%u,"
          "\"nearest_and_linear_sampling\":true,\"premultiplied_alpha\":true,"
          "\"scissor_damage\":true,\"texture_reuse\":true,\"native_fences\":8,"
          "\"egl_dma_buf_roundtrip\":%s,\"native_fence_import_wait\":true}\n", max_error, local ? "false" : "true");
   glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &target);
   glDeleteTextures(1, &over_tex); glDeleteTextures(1, &base_tex);
   if (!local) glDeleteTextures(1, &imported_over);
   glDeleteBuffers(1, &vbo); glDeleteVertexArrays(1, &vao);
   glDeleteProgram(program); glDeleteShader(vertex); glDeleteShader(fragment);
   CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
   CHECK(eglDestroyContext(display, context) && eglTerminate(display));
   return 0;
}
