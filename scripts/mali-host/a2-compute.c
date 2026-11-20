/* SPDX-License-Identifier: MIT */
/* Build beside Mesa 26.2.6's bifrost_compiler. This uses Mesa's v7 packers
 * and NIR backend without adding Mesa or a compiler to the tablet kernel. */
#include <inttypes.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "compiler/nir/nir_builder.h"
#include "panfrost/compiler/pan_compiler.h"
#include "panfrost/lib/pan_encoder.h"
#include "panfrost/lib/pan_shader.h"
#include "util/u_dynarray.h"

static const uint64_t gpu_id = UINT64_C(0x74021000);
static const uint64_t base = UINT64_C(0x03000000);
enum {
   Rsd = 0x100,
   LocalStorage = 0x140,
   Shader = 0x1000,
   PayloadSize = 0x2000,
   Source = 0x4000,
   Destination = 0xc000,
   LocalX = 32,
   GroupsX = 128,
   PresentLocalX = 16,
   PresentWidth = 1200,
   PresentHeight = 1920,
   PresentSourceStride = 4864,
   PresentDestinationStride = 4800,
};
static const uint32_t mask = UINT32_C(0x51c39a6e);

static int emit(const char *path, const void *data, size_t size)
{
   FILE *file = fopen(path, "wb");
   if (!file)
      return 1;
   const bool written = fwrite(data, 1, size, file) == size;
   return fclose(file) != 0 || !written;
}

int main(int argc, char **argv)
{
   const bool present = argc == 4 && strcmp(argv[1], "--present") == 0;
   if (argc != 3 && !present) {
      fprintf(stderr, "usage: %s [--present] PACKETS.bin SHADER.bin\n", argv[0]);
      return 2;
   }
   if (pan_arch(gpu_id) != 7)
      return 1;
   struct pan_compile_inputs inputs = {.gpu_id = gpu_id};
   const nir_shader_compiler_options *options =
      pan_get_nir_shader_compiler_options(7, MESA_SHADER_COMPUTE, false);
   const uint64_t job = present ? UINT64_C(0x04000000) : base;
   const unsigned local_x = present ? PresentLocalX : LocalX;
   const unsigned groups_x = present ? 5 : GroupsX;
   const unsigned groups_y = present ? PresentHeight : 1;
   glsl_type_singleton_init_or_ref();
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, options,
                                                  present ? "bbk-a2-present" : "bbk-a2-storage-xor");
   b.shader->info.workgroup_size[0] = local_x;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;

   nir_def *group = nir_channel(&b, nir_load_workgroup_id(&b), 0);
   nir_def *local = nir_channel(&b, nir_load_local_invocation_id(&b), 0);
   nir_def *index = nir_iadd(&b, nir_imul_imm(&b, group, local_x), local);
   if (present) {
      /* Copy sixteen packed pixels per invocation without reading row padding. */
      nir_push_if(&b, nir_ult_imm(&b, index, PresentWidth / 16));
      nir_def *row = nir_channel(&b, nir_load_workgroup_id(&b), 1);
      nir_def *column = nir_imul_imm(&b, index, 16 * 4);
      nir_def *src_offset = nir_iadd(&b, nir_imul_imm(&b, row, PresentSourceStride), column);
      nir_def *dst_offset = nir_iadd(&b, nir_imul_imm(&b, row, PresentDestinationStride), column);
      for (unsigned byte = 0; byte < 16 * 4; byte += 16) {
         nir_def *src = nir_iadd(&b, nir_imm_int64(&b, 0x04200000 + byte), nir_u2u64(&b, src_offset));
         nir_def *dst = nir_iadd(&b, nir_imm_int64(&b, 0x04c00000 + byte), nir_u2u64(&b, dst_offset));
         nir_def *value = nir_load_global(&b, 4, 32, src, .align_mul = 16);
         nir_store_global(&b, value, dst, .align_mul = 16);
      }
      nir_pop_if(&b, NULL);
   } else {
      nir_def *offset = nir_u2u64(&b, nir_imul_imm(&b, index, 4));
      nir_def *src = nir_iadd(&b, nir_imm_int64(&b, base + Source), offset);
      nir_def *dst = nir_iadd(&b, nir_imm_int64(&b, base + Destination), offset);
      nir_def *value = nir_load_global(&b, 1, 32, src, .align_mul = 4);
      nir_store_global(&b, nir_ixor(&b, value, nir_imm_int(&b, mask)), dst, .align_mul = 4);
   }
   b.shader->info.writes_memory = true;

   struct pan_shader_info info = {0};
   struct util_dynarray binary = UTIL_DYNARRAY_INIT;
   pan_preprocess_nir(b.shader, gpu_id);
   pan_postprocess_nir(b.shader, &inputs, &info);
   pan_shader_compile(b.shader, &inputs, &binary, &info);
   if (!binary.size || binary.size > PayloadSize - Shader ||
       info.tls_size || info.wls_size || info.fau.count || info.attribute_count ||
       info.ubo_count || info.texture_count || info.sampler_count ||
       !info.work_reg_count || !info.writes_global) {
      fprintf(stderr, "unsupported resource usage: instructions=%u regs=%u TLS=%u WLS=%u FAU=%u\n",
              binary.size, info.work_reg_count, info.tls_size, info.wls_size, info.fau.count);
      return 1;
   }

   _Static_assert(MALI_COMPUTE_JOB_LENGTH <= Rsd, "compute job overlaps RSD");
   _Static_assert(Rsd + MALI_RENDERER_STATE_LENGTH <= LocalStorage, "RSD overlaps TLS");
   _Static_assert(LocalStorage + MALI_LOCAL_STORAGE_LENGTH <= Shader, "TLS overlaps shader");
   alignas(64) uint8_t packet[PayloadSize] = {0};
   pan_section_pack(packet, COMPUTE_JOB, HEADER, cfg) {
      cfg.type = MALI_JOB_TYPE_COMPUTE;
      cfg.index = 1;
   }
   pan_pack_work_groups_compute(pan_section_ptr(packet, COMPUTE_JOB, INVOCATION),
                                groups_x, groups_y, 1, local_x, 1, 1, false, false);
   pan_section_pack(packet, COMPUTE_JOB, PARAMETERS, cfg) {
      cfg.job_task_split = util_logbase2_ceil(local_x + 1) +
                           2 * util_logbase2_ceil(2);
   }
   pan_section_pack(packet, COMPUTE_JOB, DRAW, cfg) {
      cfg.state = job + Rsd;
      cfg.thread_storage = job + LocalStorage;
   }
   pan_pack((struct mali_renderer_state_packed *)(packet + Rsd), RENDERER_STATE, cfg) {
      pan_shader_prepare_rsd(&info, job + Shader, &cfg);
   }
   pan_pack((struct mali_local_storage_packed *)(packet + LocalStorage), LOCAL_STORAGE, cfg) {}
   memcpy(packet + Shader, binary.data, binary.size);

   fprintf(stderr, "G52 r1 code=%u regs=%u preload=0x%" PRIx64
                   " grid=%ux%u TLS=%u WLS=%u FAU=%u\n",
           binary.size, info.work_reg_count, info.preload, groups_x, local_x,
           info.tls_size, info.wls_size, info.fau.count);
   const int output_arg = present ? 2 : 1;
   const int status = emit(argv[output_arg], packet, Shader + binary.size) ||
                      emit(argv[output_arg + 1], binary.data, binary.size);
   util_dynarray_fini(&binary);
   ralloc_free(b.shader);
   glsl_type_singleton_decref();
   return status;
}
