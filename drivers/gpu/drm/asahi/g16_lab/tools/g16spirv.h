/* SPDX-License-Identifier: MIT */
/* SPIR-V frontend with explicit lowering to the bounded lab's vertex fetch,
 * UVS layout and tile store ABI. Shader arithmetic is compiled from the input.
 * This is offline compilation, not a Vulkan device or pipeline implementation.
 */
#include "compiler/spirv/nir_spirv.h"

static int lab_type_size(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

static bool lower_lab_io(nir_builder *b, nir_intrinsic_instr *intr, void *unused)
{
   (void)unused;
   b->cursor = nir_before_instr(&intr->instr);
   if (intr->intrinsic == nir_intrinsic_load_input && b->shader->info.stage == MESA_SHADER_VERTEX) {
      nir_io_semantics io = nir_intrinsic_io_semantics(intr);
      unsigned component = nir_intrinsic_component(intr);
      if (io.location != VERT_ATTRIB_GENERIC0 || component + intr->def.num_components > 4 ||
          !nir_src_is_const(intr->src[0]) || nir_src_as_uint(intr->src[0])) {
         fprintf(stderr, "unsupported vertex input\n");
         exit(1);
      }
      nir_def *attr = nir_load_agx(b, 4, 32, nir_load_preamble(b, 1, 64, .base = 0),
          nir_load_vertex_id(b), .base = 2, .format = PIPE_FORMAT_R32_UINT);
      unsigned channels[4];
      for (unsigned i = 0; i < intr->def.num_components; ++i)
         channels[i] = component + i;
      nir_def_replace(&intr->def, nir_swizzle(b, attr, channels, intr->def.num_components));
      return true;
   }
   if (intr->intrinsic != nir_intrinsic_store_output)
      return false;
   nir_io_semantics io = nir_intrinsic_io_semantics(intr);
   unsigned component = nir_intrinsic_component(intr);
   if (!nir_src_is_const(intr->src[1]) || nir_src_as_uint(intr->src[1])) {
      fprintf(stderr, "indirect output outside lab contract\n");
      exit(1);
   }
   if (b->shader->info.stage == MESA_SHADER_VERTEX) {
      unsigned base;
      if (io.location == VARYING_SLOT_POS && component + intr->num_components <= 4)
         base = 0;
      else if (io.location == VARYING_SLOT_VAR0 && component + intr->num_components <= 3)
         base = 4;
      else {
         fprintf(stderr, "unsupported vertex output location=%u components=%u offset=%u\n", io.location, intr->num_components, component);
         nir_print_shader(b->shader, stderr);
         exit(1);
      }
      for (unsigned i = 0; i < intr->num_components; ++i) {
         if (nir_intrinsic_write_mask(intr) & (1u << i))
            nir_store_uvs_agx(b, nir_channel(b, intr->src[0].ssa, i),
                             nir_imm_intN_t(b, base + component + i, 16));
      }
   } else {
      if (io.location != FRAG_RESULT_DATA0 || component || intr->num_components != 4 ||
          nir_intrinsic_write_mask(intr) != 0xf) {
         fprintf(stderr, "unsupported fragment output\n");
         exit(1);
      }
      nir_store_local_pixel_agx(b, intr->src[0].ssa, nir_imm_intN_t(b, 0xff, 16),
                               nir_undef(b, 2, 16), .base = 0, .write_mask = 0xf,
                               .format = PIPE_FORMAT_R8_UNORM);
   }
   nir_instr_remove(&intr->instr);
   return true;
}

static nir_shader *from_spirv(const char *path, mesa_shader_stage stage)
{
   FILE *file = fopen(path, "rb");
   if (!file || fseek(file, 0, SEEK_END)) {
      perror(path);
      exit(1);
   }
   long length = ftell(file);
   if (length < 20 || length > 1024 * 1024 || length % 4 || fseek(file, 0, SEEK_SET)) {
      fprintf(stderr, "invalid SPIR-V input length\n");
      exit(1);
   }
   uint32_t *words = malloc(length);
   if (!words || fread(words, 1, length, file) != (size_t)length || fclose(file)) {
      fprintf(stderr, "cannot read SPIR-V input\n");
      exit(1);
   }
   const struct spirv_to_nir_options options = {
      .environment = NIR_SPIRV_VULKAN,
      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = nir_address_format_32bit_index_offset,
      .push_const_addr_format = nir_address_format_32bit_offset,
      .skip_os_break_in_debug_build = true,
   };
   nir_shader *nir = spirv_to_nir(words, length / 4, NULL, 0, stage, "main",
                                 &options, &agx_nir_options);
   free(words);
   if (!nir) {
      fprintf(stderr, "SPIR-V translation failed\n");
      exit(1);
   }
   NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_inline_functions);
   nir_remove_non_entrypoints(nir);
   NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_all);
   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_split_per_member_structs);
   NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_shader_in | nir_var_shader_out | nir_var_system_value, NULL);
   NIR_PASS(_, nir, nir_lower_io_vars_to_temporaries, nir_shader_get_entrypoint(nir), nir_var_shader_out);
   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_lower_var_copies);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   NIR_PASS(_, nir, nir_opt_dce);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out, lab_type_size,
            nir_lower_io_use_interpolated_input_intrinsics);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_cse);
   NIR_PASS(_, nir, nir_opt_dce);
   NIR_PASS(_, nir, nir_shader_intrinsics_pass, lower_lab_io, nir_metadata_control_flow, NULL);
   agx_preprocess_nir(nir);
   if (stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS(_, nir, agx_nir_lower_discard_zs_emit);
      NIR_PASS(_, nir, agx_nir_lower_sample_mask);
      NIR_PASS(_, nir, agx_nir_lower_interpolation);
      NIR_PASS(_, nir, nir_shader_intrinsics_pass, lower_sysval, nir_metadata_control_flow, NULL);
   }
   return nir;
}
