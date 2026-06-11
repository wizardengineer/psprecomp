#include "psp_ge_vertex.h"
#include "psp_ge.h"
#include "psp_ge_constants.h"
#include "recomp.h"

#include <cstdio>
#include <cstring>
#include <cmath>

// ---- Matrix helpers (PSP column-major layout) ----
//
// The GE uploads matrices COLUMN-major: world/view are
// 4 columns x 3 rows (translation at flat indices 9,10,11);
// proj is 4 columns x 4 rows. These match PPSSPP's
// Vec3ByMatrix43 / Vec3ByMatrix44.

/// Transform (x,y,z,1) by a 4x3 column-major matrix.
static void vec3_by_matrix43(
    const float m[12], const float v[3], float out[3]
) {
    out[0] = v[0] * m[0] + v[1] * m[3]
           + v[2] * m[6] + m[9];
    out[1] = v[0] * m[1] + v[1] * m[4]
           + v[2] * m[7] + m[10];
    out[2] = v[0] * m[2] + v[1] * m[5]
           + v[2] * m[8] + m[11];
}

/// Transform (x,y,z,1) by a 4x4 column-major matrix.
static void vec3_by_matrix44(
    const float m[16], const float v[3], float out[4]
) {
    out[0] = v[0] * m[0] + v[1] * m[4]
           + v[2] * m[8] + m[12];
    out[1] = v[0] * m[1] + v[1] * m[5]
           + v[2] * m[9] + m[13];
    out[2] = v[0] * m[2] + v[1] * m[6]
           + v[2] * m[10] + m[14];
    out[3] = v[0] * m[3] + v[1] * m[7]
           + v[2] * m[11] + m[15];
}

// ---- Vertex stride calculation ----

int ge_vertex_stride(uint32_t vtype) {
    int stride = 0;

    // Texture coordinates
    int tc = ge_vtype_tc(vtype);
    switch (tc) {
    case GE_VTYPE_TC_8BIT:  stride += 2; break;
    case GE_VTYPE_TC_16BIT: stride += 4; break;
    case GE_VTYPE_TC_FLOAT: stride += 8; break;
    default: break;
    }

    // Color
    int col = ge_vtype_col(vtype);
    switch (col) {
    case GE_VTYPE_COL_565:
    case GE_VTYPE_COL_5551:
    case GE_VTYPE_COL_4444:
        stride += 2;
        break;
    case GE_VTYPE_COL_8888:
        stride += 4;
        break;
    default: break;
    }

    // Normal
    int nrm = ge_vtype_nrm(vtype);
    switch (nrm) {
    case GE_VTYPE_NRM_8BIT:  stride += 3; break;
    case GE_VTYPE_NRM_16BIT:
        stride = (stride + 1) & ~1;  // align to 2
        stride += 6;
        break;
    case GE_VTYPE_NRM_FLOAT: stride += 12; break;
    default: break;
    }

    // Position (always present)
    int pos = ge_vtype_pos(vtype);
    switch (pos) {
    case GE_VTYPE_POS_8BIT:  stride += 3; break;
    case GE_VTYPE_POS_16BIT:
        stride = (stride + 1) & ~1;  // align to 2
        stride += 6;
        break;
    case GE_VTYPE_POS_FLOAT:
        stride = (stride + 3) & ~3;  // align to 4
        stride += 12;
        break;
    default: break;
    }

    // Weight (skip actual data for Phase 5 but account
    // for stride if present)
    int wt = ge_vtype_weight(vtype);
    int wcount = ((vtype >> GE_VTYPE_WEIGHTCOUNT_SHIFT)
                  & GE_VTYPE_WEIGHTCOUNT_MASK) + 1;
    if (wt != 0) {
        switch (wt) {
        case 1: stride += wcount; break;       // u8
        case 2: stride += wcount * 2; break;   // u16
        case 3: stride += wcount * 4; break;   // float
        default: break;
        }
    }

    // Pad to alignment based on largest component type
    if (pos == GE_VTYPE_POS_FLOAT
        || nrm == GE_VTYPE_NRM_FLOAT
        || tc == GE_VTYPE_TC_FLOAT) {
        stride = (stride + 3) & ~3;
    } else if (pos == GE_VTYPE_POS_16BIT
               || nrm == GE_VTYPE_NRM_16BIT
               || tc == GE_VTYPE_TC_16BIT) {
        stride = (stride + 1) & ~1;
    }

    return stride;
}

// ---- Inline read helpers ----

static inline uint8_t read_u8(
    uint8_t* rdram, uint32_t addr
) {
    return psp_mem_read<uint8_t>(rdram, addr);
}

static inline int8_t read_s8(
    uint8_t* rdram, uint32_t addr
) {
    return static_cast<int8_t>(
        psp_mem_read<uint8_t>(rdram, addr));
}

static inline uint16_t read_u16(
    uint8_t* rdram, uint32_t addr
) {
    return psp_mem_read<uint16_t>(rdram, addr);
}

static inline int16_t read_s16(
    uint8_t* rdram, uint32_t addr
) {
    return static_cast<int16_t>(
        psp_mem_read<uint16_t>(rdram, addr));
}

static inline uint32_t read_u32(
    uint8_t* rdram, uint32_t addr
) {
    return psp_mem_read<uint32_t>(rdram, addr);
}

static inline float read_float(
    uint8_t* rdram, uint32_t addr
) {
    uint32_t bits = psp_mem_read<uint32_t>(rdram, addr);
    float f;
    std::memcpy(&f, &bits, sizeof(float));
    return f;
}

// ---- Vertex decode ----

void ge_decode_vertices(
    uint8_t* rdram,
    const GeState& state,
    int prim_type,
    int count,
    std::vector<DecodedVertex>& out
) {
    uint32_t vtype = state.vertex_type;
    int stride = ge_vertex_stride(vtype);

    if (stride <= 0 || count <= 0) return;

    int tc_type  = ge_vtype_tc(vtype);
    int col_type = ge_vtype_col(vtype);
    int nrm_type = ge_vtype_nrm(vtype);
    int pos_type = ge_vtype_pos(vtype);
    int idx_type = ge_vtype_idx(vtype);

    // Read index buffer if indexed
    std::vector<uint32_t> indices;
    if (idx_type != GE_VTYPE_IDX_NONE) {
        indices.resize(count);
        uint32_t iaddr = state.index_addr;
        for (int i = 0; i < count; i++) {
            switch (idx_type) {
            case GE_VTYPE_IDX_8BIT:
                indices[i] = read_u8(rdram, iaddr);
                iaddr += 1;
                break;
            case GE_VTYPE_IDX_16BIT:
                indices[i] = read_u16(rdram, iaddr);
                iaddr += 2;
                break;
            case GE_VTYPE_IDX_32BIT:
                indices[i] = read_u32(rdram, iaddr);
                iaddr += 4;
                break;
            default:
                indices[i] = i;
                break;
            }
        }
    }

    out.resize(count);
    for (int i = 0; i < count; i++) {
        DecodedVertex& v = out[i];
        v.has_uv = (tc_type != GE_VTYPE_TC_NONE);
        v.has_color = (col_type != GE_VTYPE_COL_NONE);
        v.has_normal = (nrm_type != GE_VTYPE_NRM_NONE);

        // Determine vertex index
        uint32_t vidx = (idx_type != GE_VTYPE_IDX_NONE)
                         ? indices[i]
                         : static_cast<uint32_t>(i);

        uint32_t base = state.vertex_addr
                        + vidx * static_cast<uint32_t>(stride);
        uint32_t offset = 0;

        // Texture coordinates
        if (tc_type == GE_VTYPE_TC_8BIT) {
            v.uv[0] = read_u8(rdram, base + offset) / 128.0f;
            v.uv[1] = read_u8(rdram, base + offset + 1)
                       / 128.0f;
            offset += 2;
        } else if (tc_type == GE_VTYPE_TC_16BIT) {
            v.uv[0] = read_s16(rdram, base + offset)
                       / 32768.0f;
            v.uv[1] = read_s16(rdram, base + offset + 2)
                       / 32768.0f;
            offset += 4;
        } else if (tc_type == GE_VTYPE_TC_FLOAT) {
            v.uv[0] = read_float(rdram, base + offset);
            v.uv[1] = read_float(rdram, base + offset + 4);
            offset += 8;
        } else {
            v.uv[0] = 0.0f;
            v.uv[1] = 0.0f;
        }

        // Color
        if (col_type == GE_VTYPE_COL_565) {
            uint16_t val = read_u16(rdram, base + offset);
            v.color[0] = static_cast<uint8_t>(
                ((val & 0x1F) * 255) / 31);
            v.color[1] = static_cast<uint8_t>(
                (((val >> 5) & 0x3F) * 255) / 63);
            v.color[2] = static_cast<uint8_t>(
                (((val >> 11) & 0x1F) * 255) / 31);
            v.color[3] = 255;
            offset += 2;
        } else if (col_type == GE_VTYPE_COL_5551) {
            uint16_t val = read_u16(rdram, base + offset);
            v.color[0] = static_cast<uint8_t>(
                ((val & 0x1F) * 255) / 31);
            v.color[1] = static_cast<uint8_t>(
                (((val >> 5) & 0x1F) * 255) / 31);
            v.color[2] = static_cast<uint8_t>(
                (((val >> 10) & 0x1F) * 255) / 31);
            v.color[3] = (val & 0x8000) ? 255 : 0;
            offset += 2;
        } else if (col_type == GE_VTYPE_COL_4444) {
            uint16_t val = read_u16(rdram, base + offset);
            v.color[0] = static_cast<uint8_t>(
                ((val & 0xF) * 255) / 15);
            v.color[1] = static_cast<uint8_t>(
                (((val >> 4) & 0xF) * 255) / 15);
            v.color[2] = static_cast<uint8_t>(
                (((val >> 8) & 0xF) * 255) / 15);
            v.color[3] = static_cast<uint8_t>(
                (((val >> 12) & 0xF) * 255) / 15);
            offset += 2;
        } else if (col_type == GE_VTYPE_COL_8888) {
            v.color[0] = read_u8(rdram, base + offset);
            v.color[1] = read_u8(rdram, base + offset + 1);
            v.color[2] = read_u8(rdram, base + offset + 2);
            v.color[3] = read_u8(rdram, base + offset + 3);
            offset += 4;
        } else {
            // Default: white
            v.color[0] = 255;
            v.color[1] = 255;
            v.color[2] = 255;
            v.color[3] = 255;
        }

        // Normal
        if (nrm_type == GE_VTYPE_NRM_8BIT) {
            v.normal[0] = read_s8(rdram, base + offset)
                          / 127.0f;
            v.normal[1] = read_s8(rdram, base + offset + 1)
                          / 127.0f;
            v.normal[2] = read_s8(rdram, base + offset + 2)
                          / 127.0f;
            offset += 3;
        } else if (nrm_type == GE_VTYPE_NRM_16BIT) {
            // Align to 2
            offset = (offset + 1) & ~1u;
            v.normal[0] = read_s16(rdram, base + offset)
                          / 32767.0f;
            v.normal[1] = read_s16(rdram, base + offset + 2)
                          / 32767.0f;
            v.normal[2] = read_s16(rdram, base + offset + 4)
                          / 32767.0f;
            offset += 6;
        } else if (nrm_type == GE_VTYPE_NRM_FLOAT) {
            v.normal[0] = read_float(rdram, base + offset);
            v.normal[1] = read_float(rdram,
                                     base + offset + 4);
            v.normal[2] = read_float(rdram,
                                     base + offset + 8);
            offset += 12;
        } else {
            v.normal[0] = 0.0f;
            v.normal[1] = 0.0f;
            v.normal[2] = 1.0f;
        }

        // Position
        if (pos_type == GE_VTYPE_POS_8BIT) {
            v.pos[0] = static_cast<float>(
                read_s8(rdram, base + offset));
            v.pos[1] = static_cast<float>(
                read_s8(rdram, base + offset + 1));
            v.pos[2] = static_cast<float>(
                read_s8(rdram, base + offset + 2));
        } else if (pos_type == GE_VTYPE_POS_16BIT) {
            // Align to 2
            offset = (offset + 1) & ~1u;
            v.pos[0] = static_cast<float>(
                read_s16(rdram, base + offset));
            v.pos[1] = static_cast<float>(
                read_s16(rdram, base + offset + 2));
            v.pos[2] = static_cast<float>(
                read_s16(rdram, base + offset + 4));
        } else if (pos_type == GE_VTYPE_POS_FLOAT) {
            // Align to 4
            offset = (offset + 3) & ~3u;
            v.pos[0] = read_float(rdram, base + offset);
            v.pos[1] = read_float(rdram,
                                  base + offset + 4);
            v.pos[2] = read_float(rdram,
                                  base + offset + 8);
        } else {
            v.pos[0] = 0.0f;
            v.pos[1] = 0.0f;
            v.pos[2] = 0.0f;
        }
    }

    (void)prim_type;  // Used by caller for primitive assembly
}

// ---- Transform ----

// Degenerate-matrix detection. These compensate for
// guest-side broken matrix uploads (open issue, FPU/VFPU
// dataflow family): view arrives all-zero and proj arrives
// NaN/Inf or with a collapsed diagonal. Remove the fallbacks
// once the guest uploads sane matrices.

/// True if every element of the view matrix is zero.
static bool ge_view_matrix_all_zero(const GeState& state) {
    for (int i = 0; i < 12; i++) {
        if (state.view_matrix[i] != 0.0f) return false;
    }
    static bool warned = false;
    if (!warned) {
        warned = true;
        std::fprintf(stderr,
            "[GE] view matrix all-zero -- NDC-direct "
            "fallback engaged\n");
    }
    return true;
}

/// True if the proj matrix is non-finite or has a
/// collapsed X/Y diagonal (column-major indices 0 and 5).
static bool ge_proj_matrix_degenerate(const GeState& state) {
    bool bad = std::fabs(state.proj_matrix[0]) < 1e-6f
            || std::fabs(state.proj_matrix[5]) < 1e-6f;
    for (int i = 0; i < 16 && !bad; i++) {
        if (!std::isfinite(state.proj_matrix[i])) bad = true;
    }
    if (bad) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                "[GE] proj matrix degenerate -- NDC-direct "
                "fallback engaged\n");
        }
    }
    return bad;
}

void ge_transform_vertices(
    std::vector<DecodedVertex>& verts,
    const GeState& state
) {
    bool through = ge_vtype_through(state.vertex_type);

    if (through) {
        // Through-mode: map screen coords to NDC [-1,1]
        // PSP screen is 480x272
        for (auto& v : verts) {
            v.pos[0] = v.pos[0] / 240.0f - 1.0f;
            v.pos[1] = 1.0f - v.pos[1] / 136.0f;
            v.pos[2] = v.pos[2] / 65535.0f;

            // Apply tex scale/offset
            if (v.has_uv) {
                v.uv[0] = v.uv[0] * state.tex_scale_u
                          + state.tex_offset_u;
                v.uv[1] = v.uv[1] * state.tex_scale_v
                          + state.tex_offset_v;
            }
        }
    } else {
        // Transform mode: model -> world -> view -> proj
        // using PSP column-major matrix semantics.
        //
        // FALLBACKS: the guest currently uploads broken
        // matrices (view all-zero; proj NaN/Inf or malformed
        // diagonal) -- open issue, FPU/VFPU dataflow family.
        // Remove these workarounds when that is fixed.
        //
        // Since the issue #27 VFPU register-file fix, the proj
        // upload is a valid finite ortho, but the view upload is
        // still all-zero (the guest-side gum view buffer at
        // ~0x090965B0 is never written -- next divergence layer).
        // An all-zero view with a valid proj can never render
        // correctly through the real path (identity-view is a
        // wrong guess against the 176x100 ortho extents), so an
        // all-zero view ALSO routes to the NDC-direct mapping.
        // The full real path engages automatically once the guest
        // uploads a non-zero view matrix.
        bool view_zero = ge_view_matrix_all_zero(state);
        bool proj_bad = ge_proj_matrix_degenerate(state);
        bool ndc_direct = view_zero || proj_bad;

        for (auto& v : verts) {
            float wpos[3], vpos[3];
            vec3_by_matrix43(state.world_matrix,
                             v.pos, wpos);
            if (!ndc_direct) {
                vec3_by_matrix43(state.view_matrix,
                                 wpos, vpos);
            }

            if (ndc_direct) {
                // Bypass proj: emit NDC directly from
                // world-space. This is the ortho mapping
                // Patapon intends (viewport scale 240/-136,
                // center 2048, offset 1808/1912 makes
                // NDC-direct equivalent).
                v.pos[0] = wpos[0] / 240.0f - 1.0f;
                v.pos[1] = wpos[1] / 136.0f + 1.0f;
                v.pos[2] = 0.0f;
            } else {
                float clip[4];
                vec3_by_matrix44(state.proj_matrix,
                                 vpos, clip);

                // Perspective divide
                if (std::fabs(clip[3]) > 1e-6f) {
                    v.pos[0] = clip[0] / clip[3];
                    v.pos[1] = clip[1] / clip[3];
                    v.pos[2] = clip[2] / clip[3];
                } else {
                    v.pos[0] = clip[0];
                    v.pos[1] = clip[1];
                    v.pos[2] = clip[2];
                }
            }

            // Apply tex scale/offset
            if (v.has_uv) {
                v.uv[0] = v.uv[0] * state.tex_scale_u
                          + state.tex_offset_u;
                v.uv[1] = v.uv[1] * state.tex_scale_v
                          + state.tex_offset_v;
            }
        }
    }
}
