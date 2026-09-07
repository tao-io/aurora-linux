/* SPDX-License-Identifier: MIT */
/* Offline Mesa NIR -> AGX3b compiler probe for the bounded J713 RGB draw.
 * No Vulkan device admission is changed. Hardware validation is separate.
 */
#include "agx_compile.h"
#include "agx_nir.h"
#include "nir_builder.h"
#include "util/format/u_formats.h"
#include <stdio.h>
#include <stdlib.h>

static nir_shader *vertex(void)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_VERTEX, &agx_nir_options, "J713 RGB vertex");
   nir_def *attr = nir_load_agx(
      &b, 4, 32, nir_load_preamble(&b, 1, 64, .base = 0),
      nir_load_vertex_id(&b), .access = 0, .base = 2,
      .format = PIPE_FORMAT_R32_UINT, .sign_extend = false);
   nir_def *r = nir_channel(&b, attr, 2);
   nir_def *g = nir_channel(&b, attr, 3);
   nir_def *values[] = {
      nir_channel(&b, attr, 0), nir_channel(&b, attr, 1),
      nir_imm_float(&b, 0), nir_imm_float(&b, 1), r, g,
      nir_fsub(&b, nir_fsub(&b, nir_imm_float(&b, 1), r), g),
   };
   for (unsigned i = 0; i < ARRAY_SIZE(values); ++i)
      nir_store_uvs_agx(&b, values[i], nir_imm_intN_t(&b, i, 16));
   b.shader->info.outputs_written = VARYING_BIT_POS | BITFIELD64_BIT(VARYING_SLOT_VAR0);
   agx_preprocess_nir(b.shader);
   return b.shader;
}

static bool lower_sysval(nir_builder *b, nir_intrinsic_instr *intr, void *unused)
{
   (void)unused;
   uint16_t value;
   switch (intr->intrinsic) {
   case nir_intrinsic_load_api_sample_mask_agx:
   case nir_intrinsic_load_active_samples_agx:
      value = 0xff;
      break;
   case nir_intrinsic_load_shader_part_tests_zs_agx:
      value = 0;
      break;
   default:
      return false;
   }
   b->cursor = nir_before_instr(&intr->instr);
   nir_def_replace(&intr->def, nir_imm_intN_t(b, value, 16));
   return true;
}

static nir_shader *fragment(void)
{
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &agx_nir_options, "J713 RGB fragment");
   nir_def *bary = nir_load_barycentric_pixel(&b, 32, .interp_mode = INTERP_MODE_NOPERSPECTIVE);
   nir_def *rgb = nir_load_interpolated_input(
      &b, 3, 32, bary, nir_imm_int(&b, 0), .base = 0, .component = 0,
      .io_semantics.location = VARYING_SLOT_VAR0, .io_semantics.num_slots = 1);
   nir_def *rgba = nir_vec4(&b, nir_channel(&b, rgb, 0), nir_channel(&b, rgb, 1),
                            nir_channel(&b, rgb, 2), nir_imm_float(&b, 1));
   /* Lowered per-lane tile format, as used by Mesa's graphics compiler test. */
   nir_store_local_pixel_agx(&b, rgba, nir_imm_intN_t(&b, 0xff, 16),
                            nir_undef(&b, 2, 16), .base = 0, .write_mask = 0xf,
                            .format = PIPE_FORMAT_R8_UNORM);
   b.shader->info.inputs_read = BITFIELD64_BIT(VARYING_SLOT_VAR0);
   b.shader->info.outputs_written = BITFIELD64_BIT(FRAG_RESULT_DATA0);
   agx_preprocess_nir(b.shader);
   NIR_PASS(_, b.shader, agx_nir_lower_discard_zs_emit);
   NIR_PASS(_, b.shader, agx_nir_lower_sample_mask);
   NIR_PASS(_, b.shader, agx_nir_lower_interpolation);
   NIR_PASS(_, b.shader, nir_shader_intrinsics_pass, lower_sysval,
            nir_metadata_control_flow, NULL);
   return b.shader;
}

#include "g16spirv.h"

int main(int argc, char **argv)
{
   if (argc != 1 && argc != 3) {
      fprintf(stderr, "usage: %s [vertex.spv fragment.spv]\n", argv[0]);
      return 1;
   }
   glsl_type_singleton_init_or_ref();
   struct agx_shader_part parts[2] = {0};
   const char *names[] = {"vertex", "fragment"};
   for (unsigned i = 0; i < 2; ++i) {
      nir_shader *nir = argc == 3 ? from_spirv(argv[i + 1], i ? MESA_SHADER_FRAGMENT : MESA_SHADER_VERTEX) :
                                   i ? fragment() : vertex();
      struct agx_shader_key key = {
         .dev.encoding = AGX_ISA_ENCODING_AGX3B,
         .no_preamble = true,
      };
      bool ok = agx_compile_shader_nir(nir, &key, &parts[i]);
      fprintf(stderr, "%s compiled=%d bytes=%u gprs=%u staging=%u known=%d offset=%u preamble=%d constants=%u scratch=%u push=%u\n",
              names[i], ok, parts[i].info.binary_size, parts[i].info.nr_gprs,
              parts[i].info.agx3_staging_halfs, parts[i].info.agx3_staging_known,
              parts[i].info.main_offset, parts[i].info.has_preamble,
              parts[i].info.rodata.size_16, parts[i].info.scratch_size, parts[i].info.push_count);
      ralloc_free(nir);
      if (!ok || !parts[i].binary || parts[i].info.main_offset ||
          parts[i].info.has_preamble || parts[i].info.rodata.size_16 ||
          parts[i].info.scratch_size || parts[i].info.push_count ||
          !parts[i].info.agx3_staging_known ||
          parts[i].info.nr_gprs > (i ? 8 : 18) ||
          parts[i].info.agx3_staging_halfs > (i ? 0 : 14) ||
          parts[i].info.binary_size > 0x800) {
         fprintf(stderr, "%s exceeds the lab's shader contract\n", names[i]);
         return 1;
      }
   }
   const struct agx_varyings_fs *fs = &parts[1].info.varyings.fs;
   if (fs->nr_cf != 3 || fs->nr_bindings != 1 || fs->reads_z ||
       fs->bindings[0].cf_base || fs->bindings[0].count != 3 ||
       fs->bindings[0].slot != VARYING_SLOT_VAR0 || fs->bindings[0].offset ||
       !fs->bindings[0].smooth || fs->bindings[0].perspective) {
      fprintf(stderr, "fragment coefficient contract differs from the RGB lab\n");
      return 1;
   }
   puts("/* SPDX-License-Identifier: MIT */");
   puts(argc == 3 ? "/* Generated by tools/g16mesa.c from SPIR-V through Mesa NIR. */" :
                    "/* Generated by tools/g16mesa.c through Mesa NIR. */");
   for (unsigned i = 0; i < 2; ++i) {
      const struct agx_shader_info *info = &parts[i].info;
      fprintf(stderr, "%s: bytes=%u gpr_halfs=%u staging_halfs=%u push=%u\n",
              names[i], info->binary_size, info->nr_gprs, info->agx3_staging_halfs, info->push_count);
      printf("static const u8 g16_mesa_%s[] = {\n", names[i]);
      const uint8_t *bytes = parts[i].binary;
      for (unsigned j = 0; j < info->binary_size; ++j)
         printf("%s0x%02x,%s", j % 12 ? " " : "\t", bytes[j], j % 12 == 11 ? "\n" : "");
      puts("\n};");
      free(parts[i].binary);
   }
   glsl_type_singleton_decref();
   return 0;
}
