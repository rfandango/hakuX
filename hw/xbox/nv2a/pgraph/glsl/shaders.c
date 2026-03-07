/*
 * Geforce NV2A PGRAPH GLSL Shader Generator
 *
 * Copyright (c) 2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "hw/xbox/nv2a/pgraph/pgraph.h"
#include "qemu/fast-hash.h"
#include "shaders.h"

uint64_t pgraph_glsl_hash_shader_state(const ShaderState *state)
{
    size_t common_size = offsetof(VshState, fixed_function);
    uint64_t h = fast_hash((const uint8_t *)&state->vsh, common_size);

    if (state->vsh.is_fixed_function) {
        h ^= fast_hash((const uint8_t *)&state->vsh.fixed_function,
                        sizeof(FixedFunctionVshState));
    } else {
        size_t prog_size = offsetof(ProgrammableVshState, program_data) +
            state->vsh.programmable.program_length *
                VSH_TOKEN_SIZE * sizeof(uint32_t);
        h ^= fast_hash((const uint8_t *)&state->vsh.programmable, prog_size);
    }

    h ^= fast_hash((const uint8_t *)&state->geom, sizeof(GeomState));
    h ^= fast_hash((const uint8_t *)&state->psh, sizeof(PshState));
    return h;
}

int pgraph_glsl_compare_shader_state(const ShaderState *a,
                                     const ShaderState *b)
{
    size_t common_size = offsetof(VshState, fixed_function);
    int r = memcmp(&a->vsh, &b->vsh, common_size);
    if (r) return r;

    if (a->vsh.is_fixed_function != b->vsh.is_fixed_function) return 1;

    if (a->vsh.is_fixed_function) {
        r = memcmp(&a->vsh.fixed_function, &b->vsh.fixed_function,
                    sizeof(FixedFunctionVshState));
    } else {
        if (a->vsh.programmable.program_length !=
            b->vsh.programmable.program_length) return 1;
        size_t prog_size = offsetof(ProgrammableVshState, program_data) +
            a->vsh.programmable.program_length *
                VSH_TOKEN_SIZE * sizeof(uint32_t);
        r = memcmp(&a->vsh.programmable, &b->vsh.programmable, prog_size);
    }
    if (r) return r;

    r = memcmp(&a->geom, &b->geom, sizeof(GeomState));
    if (r) return r;

    return memcmp(&a->psh, &b->psh, sizeof(PshState));
}

ShaderState pgraph_glsl_get_shader_state(PGRAPHState *pg)
{
    pg->program_data_dirty = false; /* fixme */

    ShaderState state;

    // We will hash it, so make sure any padding is zeroed
    memset(&state, 0, sizeof(ShaderState));

    pgraph_glsl_set_vsh_state(pg, &state.vsh);
    pgraph_glsl_set_geom_state(pg, &state.geom);
    pgraph_glsl_set_psh_state(pg, &state.psh);

    return state;
}

bool pgraph_glsl_check_shader_state_dirty(PGRAPHState *pg,
                                          const ShaderState *state)
{
    if (pg->program_data_dirty) {
        return true;
    }

    unsigned int regs[] = {
        NV_PGRAPH_COMBINECTL,      NV_PGRAPH_COMBINESPECFOG0,
        NV_PGRAPH_COMBINESPECFOG1, NV_PGRAPH_CONTROL_0,
        NV_PGRAPH_CONTROL_3,       NV_PGRAPH_CSV0_C,
        NV_PGRAPH_CSV0_D,          NV_PGRAPH_CSV1_A,
        NV_PGRAPH_CSV1_B,          NV_PGRAPH_POINTSIZE,
        NV_PGRAPH_SETUPRASTER,     NV_PGRAPH_SHADERCLIPMODE,
        NV_PGRAPH_SHADERCTL,       NV_PGRAPH_SHADERPROG,
        NV_PGRAPH_SHADOWCTL,       NV_PGRAPH_ZCOMPRESSOCCLUDE,
    };
    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        if (pgraph_is_reg_dirty(pg, regs[i])) {
            return true;
        }
    }

    int num_stages = pgraph_reg_r(pg, NV_PGRAPH_COMBINECTL) & 0xFF;
    for (int i = 0; i < num_stages; i++) {
        if (pgraph_is_reg_dirty(pg, NV_PGRAPH_COMBINEALPHAI0 + i * 4) ||
            pgraph_is_reg_dirty(pg, NV_PGRAPH_COMBINEALPHAO0 + i * 4) ||
            pgraph_is_reg_dirty(pg, NV_PGRAPH_COMBINECOLORI0 + i * 4) ||
            pgraph_is_reg_dirty(pg, NV_PGRAPH_COMBINECOLORO0 + i * 4)) {
            return true;
        }
    }

    if (pg->uniform_attrs != state->vsh.uniform_attrs ||
        pg->swizzle_attrs != state->vsh.swizzle_attrs ||
        pg->compressed_attrs != state->vsh.compressed_attrs ||
        pg->primitive_mode != state->geom.primitive_mode ||
        pg->surface_scale_factor != state->vsh.surface_scale_factor ||
        pg->surface_shape.zeta_format != state->psh.surface_zeta_format) {
        return true;
    }

    for (int i = 0; i < 4; i++) {
        if (pgraph_is_reg_dirty(pg, NV_PGRAPH_TEXCTL0_0 + i * 4) ||
            pgraph_is_reg_dirty(pg, NV_PGRAPH_TEXFILTER0 + i * 4) ||
            pgraph_is_reg_dirty(pg, NV_PGRAPH_TEXFMT0 + i * 4)) {
            return true;
        }

        if (pg->texture_matrix_enable[i] !=
            state->vsh.fixed_function.texture_matrix_enable[i]) {
            return true;
        }
    }

    return false;
}
