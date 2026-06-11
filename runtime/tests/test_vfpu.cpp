#include "psp_vfpu.h"
#include "recomp.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int failures = 0;
static int tests_run = 0;

#define ASSERT_EXACT(actual, expected, msg) \
    do { \
        tests_run++; \
        if ((actual) != (expected)) { \
            std::fprintf(stderr, \
                "FAIL: %s: got %f, expected %f\n", \
                msg, \
                static_cast<double>(actual), \
                static_cast<double>(expected)); \
            failures++; \
        } \
    } while (0)

#define ASSERT_APPROX(actual, expected, eps, msg) \
    do { \
        tests_run++; \
        if (std::fabsf((actual) - (expected)) > (eps)) { \
            std::fprintf(stderr, \
                "FAIL: %s: got %f, expected %f (eps=%e)\n", \
                msg, \
                static_cast<double>(actual), \
                static_cast<double>(expected), \
                static_cast<double>(eps)); \
            failures++; \
        } \
    } while (0)

#define ASSERT_INT_EQ(actual, expected, msg) \
    do { \
        tests_run++; \
        if ((actual) != (expected)) { \
            std::fprintf(stderr, \
                "FAIL: %s: got 0x%X, expected 0x%X\n", \
                msg, \
                static_cast<unsigned>(actual), \
                static_cast<unsigned>(expected)); \
            failures++; \
        } \
    } while (0)

/// Initialize a clean recomp_context with identity prefixes.
static void init_ctx(recomp_context& ctx) {
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.vfpu_ctrl[VFPU_CTRL_SPREFIX] = 0xE4u;
    ctx.vfpu_ctrl[VFPU_CTRL_TPREFIX] = 0xE4u;
    ctx.vfpu_ctrl[VFPU_CTRL_DPREFIX] = 0x00u;
}

// Helpers available for future tests if needed:
// set_vfpu_single(ctx, flat_idx, val) - write single float
// single_idx(reg) - compute flat index from 7-bit encoding

// ===================================================================
// Test 1: vfpu_sin cardinal values (bit-exact)
// ===================================================================
static void test_sin_cardinals() {
    std::printf("  test_sin_cardinals...\n");
    recomp_context ctx;

    // sin(0.0) = 0.0
    init_ctx(ctx);
    ctx.vfpu[0] = 0.0f;  // S000 (flat 0: mtx0, col0, row0)
    vfpu_vsin(&ctx, nullptr, 0x01, 0x00, 1);  // S010 = sin(S000)
    // vd=0x01: mtx0, col1, row0 -> flat 0*16 + 1*4 + 0 = 4
    ASSERT_EXACT(ctx.vfpu[4], 0.0f, "sin(0.0)");

    // sin(1.0) = 1.0
    init_ctx(ctx);
    ctx.vfpu[0] = 1.0f;
    vfpu_vsin(&ctx, nullptr, 0x01, 0x00, 1);
    ASSERT_EXACT(ctx.vfpu[4], 1.0f, "sin(1.0)");

    // sin(2.0) = 0.0
    init_ctx(ctx);
    ctx.vfpu[0] = 2.0f;
    vfpu_vsin(&ctx, nullptr, 0x01, 0x00, 1);
    ASSERT_EXACT(ctx.vfpu[4], 0.0f, "sin(2.0)");

    // sin(3.0) = -1.0
    init_ctx(ctx);
    ctx.vfpu[0] = 3.0f;
    vfpu_vsin(&ctx, nullptr, 0x01, 0x00, 1);
    ASSERT_EXACT(ctx.vfpu[4], -1.0f, "sin(3.0)");
}

// ===================================================================
// Test 2: vfpu_cos cardinal values (bit-exact)
// ===================================================================
static void test_cos_cardinals() {
    std::printf("  test_cos_cardinals...\n");
    recomp_context ctx;

    // cos(0.0) = 1.0
    init_ctx(ctx);
    ctx.vfpu[0] = 0.0f;
    vfpu_vcos(&ctx, nullptr, 0x01, 0x00, 1);
    ASSERT_EXACT(ctx.vfpu[4], 1.0f, "cos(0.0)");

    // cos(1.0) = 0.0
    init_ctx(ctx);
    ctx.vfpu[0] = 1.0f;
    vfpu_vcos(&ctx, nullptr, 0x01, 0x00, 1);
    ASSERT_EXACT(ctx.vfpu[4], 0.0f, "cos(1.0)");

    // cos(2.0) = -1.0
    init_ctx(ctx);
    ctx.vfpu[0] = 2.0f;
    vfpu_vcos(&ctx, nullptr, 0x01, 0x00, 1);
    ASSERT_EXACT(ctx.vfpu[4], -1.0f, "cos(2.0)");

    // cos(3.0) = 0.0
    init_ctx(ctx);
    ctx.vfpu[0] = 3.0f;
    vfpu_vcos(&ctx, nullptr, 0x01, 0x00, 1);
    ASSERT_EXACT(ctx.vfpu[4], 0.0f, "cos(3.0)");
}

// ===================================================================
// Test 3: eat_prefixes resets correctly
// ===================================================================
static void test_eat_prefixes() {
    std::printf("  test_eat_prefixes...\n");
    recomp_context ctx;
    init_ctx(ctx);

    ctx.vfpu_ctrl[VFPU_CTRL_SPREFIX] = 0xDEADBEEFu;
    ctx.vfpu_ctrl[VFPU_CTRL_TPREFIX] = 0xCAFEBABEu;
    ctx.vfpu_ctrl[VFPU_CTRL_DPREFIX] = 0x12345678u;

    vfpu_eat_prefixes(&ctx);

    ASSERT_INT_EQ(ctx.vfpu_ctrl[VFPU_CTRL_SPREFIX], 0xE4u,
                  "SPREFIX after eat");
    ASSERT_INT_EQ(ctx.vfpu_ctrl[VFPU_CTRL_TPREFIX], 0xE4u,
                  "TPREFIX after eat");
    ASSERT_INT_EQ(ctx.vfpu_ctrl[VFPU_CTRL_DPREFIX], 0x00u,
                  "DPREFIX after eat");
}

// ===================================================================
// Test 4: Prefix swizzle on a pair vector
// ===================================================================
static void test_prefix_swizzle() {
    std::printf("  test_prefix_swizzle...\n");
    recomp_context ctx;
    init_ctx(ctx);

    // Load a pair {3.0, 7.0} into C000 (rows 0,1 of column 0)
    // 7-bit encoding for pair C000: mtx=0, col=0, row=0,
    //   transpose=0 -> reg = 0x00
    // Layout vfpu[mtx*16 + col*4 + row]: pair is contiguous down
    // the column.
    ctx.vfpu[0] = 3.0f;  // mtx0, col0, row0 -> flat 0
    ctx.vfpu[1] = 7.0f;  // mtx0, col0, row1 -> flat 1

    // Set SPREFIX to swizzle Y,X (swap elements):
    // swizzle bits: element 0 reads from index 1 (=0b01),
    //               element 1 reads from index 0 (=0b00)
    // bits 1:0 = 01 (element 0 <- index 1)
    // bits 3:2 = 00 (element 1 <- index 0)
    // Result: 0x01 for swizzle, rest 0
    ctx.vfpu_ctrl[VFPU_CTRL_SPREFIX] = 0x01u;

    // vmov pair: vd=0x04 (C100), vs=0x00 (C000), size=2
    // vd = 0x04 -> mtx = (4>>2)&7 = 1, col = 4&3 = 0
    // For pair: row = (4>>5)&2 = 0, transpose = (4>>5)&1 = 0
    // Elements at mtx=1, col=0, rows 0,1 -> flat 16, 17
    vfpu_vmov(&ctx, nullptr, 0x04, 0x00, 2);

    ASSERT_EXACT(ctx.vfpu[16], 7.0f, "swizzle pair[0]");
    ASSERT_EXACT(ctx.vfpu[17], 3.0f, "swizzle pair[1]");
}

// ===================================================================
// Test 5: vmmul 2x2 identity produces original matrix
// ===================================================================
static void test_vmmul_identity() {
    std::printf("  test_vmmul_identity...\n");
    recomp_context ctx;
    init_ctx(ctx);

    // Layout: element (row r, col c) of mtx m -> flat m*16 + c*4 + r
    // Set up 2x2 identity in matrix 0 (M000)
    ctx.vfpu[0]  = 1.0f;  // mtx0 (0,0)
    ctx.vfpu[4]  = 0.0f;  // mtx0 (0,1)
    ctx.vfpu[1]  = 0.0f;  // mtx0 (1,0)
    ctx.vfpu[5]  = 1.0f;  // mtx0 (1,1)

    // Set up arbitrary 2x2 [[1,2],[3,4]] in matrix 1 (M100)
    ctx.vfpu[16] = 1.0f;  // mtx1 (0,0)
    ctx.vfpu[20] = 2.0f;  // mtx1 (0,1)
    ctx.vfpu[17] = 3.0f;  // mtx1 (1,0)
    ctx.vfpu[21] = 4.0f;  // mtx1 (1,1)

    // vmmul M200, M000, M100 -> result in matrix 2
    // M000 = 0x00, M100 = 0x04, M200 = 0x08
    // PPSSPP semantics: result = transpose(M000) * M100
    //                 = identity * M100 = M100
    vfpu_vmmul(&ctx, nullptr, 0x08, 0x00, 0x04, 2);

    ASSERT_APPROX(ctx.vfpu[32], 1.0f, 1e-5f,
                  "mmul identity [0][0]");
    ASSERT_APPROX(ctx.vfpu[36], 2.0f, 1e-5f,
                  "mmul identity [0][1]");
    ASSERT_APPROX(ctx.vfpu[33], 3.0f, 1e-5f,
                  "mmul identity [1][0]");
    ASSERT_APPROX(ctx.vfpu[37], 4.0f, 1e-5f,
                  "mmul identity [1][1]");
}

// ===================================================================
// Test 6: Non-cardinal trig accuracy (epsilon)
// ===================================================================
static void test_trig_noncardinal() {
    std::printf("  test_trig_noncardinal...\n");
    recomp_context ctx;

    // sin(0.5) should be sin(pi/4) = sqrt(2)/2
    init_ctx(ctx);
    ctx.vfpu[0] = 0.5f;
    vfpu_vsin(&ctx, nullptr, 0x01, 0x00, 1);
    float expected_sin = std::sinf(0.5f
        * static_cast<float>(M_PI_2));
    ASSERT_APPROX(ctx.vfpu[4], expected_sin, 1e-5f,
                  "sin(0.5)");

    // cos(0.5) should be cos(pi/4) = sqrt(2)/2
    init_ctx(ctx);
    ctx.vfpu[0] = 0.5f;
    vfpu_vcos(&ctx, nullptr, 0x01, 0x00, 1);
    float expected_cos = std::cosf(0.5f
        * static_cast<float>(M_PI_2));
    ASSERT_APPROX(ctx.vfpu[4], expected_cos, 1e-5f,
                  "cos(0.5)");
}

// ===================================================================
// Test 7: vadd quad adds element-wise
// ===================================================================
static void test_vadd_quad() {
    std::printf("  test_vadd_quad...\n");
    recomp_context ctx;
    init_ctx(ctx);

    // C000 (quad, mtx=0, col=0, row=0, no transpose)
    ctx.vfpu[0] = 1.0f;
    ctx.vfpu[1] = 2.0f;
    ctx.vfpu[2] = 3.0f;
    ctx.vfpu[3] = 4.0f;

    // C100 (quad, mtx=1, col=0)
    ctx.vfpu[16] = 10.0f;
    ctx.vfpu[17] = 20.0f;
    ctx.vfpu[18] = 30.0f;
    ctx.vfpu[19] = 40.0f;

    // vadd C200, C000, C100 (size=4)
    // C000 = 0x00, C100 = 0x04, C200 = 0x08
    vfpu_vadd(&ctx, nullptr, 0x08, 0x00, 0x04, 4);

    ASSERT_EXACT(ctx.vfpu[32], 11.0f, "vadd quad[0]");
    ASSERT_EXACT(ctx.vfpu[33], 22.0f, "vadd quad[1]");
    ASSERT_EXACT(ctx.vfpu[34], 33.0f, "vadd quad[2]");
    ASSERT_EXACT(ctx.vfpu[35], 44.0f, "vadd quad[3]");
}

// ===================================================================
// Test 8: vdot dot product
// ===================================================================
static void test_vdot() {
    std::printf("  test_vdot...\n");
    recomp_context ctx;
    init_ctx(ctx);

    // Vector {1, 2, 3} in C000 (triple)
    ctx.vfpu[0] = 1.0f;
    ctx.vfpu[1] = 2.0f;
    ctx.vfpu[2] = 3.0f;

    // Vector {4, 5, 6} in C100 (triple)
    ctx.vfpu[16] = 4.0f;
    ctx.vfpu[17] = 5.0f;
    ctx.vfpu[18] = 6.0f;

    // vdot S200, C000, C100 (size=3)
    vfpu_vdot(&ctx, nullptr, 0x08, 0x00, 0x04, 3);

    // dot = 1*4 + 2*5 + 3*6 = 4 + 10 + 18 = 32
    ASSERT_EXACT(ctx.vfpu[32], 32.0f, "vdot triple");
}

// ===================================================================
// Test 9: vcmp EQ mode
// ===================================================================
static void test_vcmp_eq() {
    std::printf("  test_vcmp_eq...\n");
    recomp_context ctx;
    init_ctx(ctx);

    // Pair {1, 2} vs {1, 3}
    ctx.vfpu[0] = 1.0f;
    ctx.vfpu[1] = 2.0f;
    ctx.vfpu[16] = 1.0f;
    ctx.vfpu[17] = 3.0f;

    // vcmp EQ, P000, P100 (cond=1, size=2)
    vfpu_vcmp(&ctx, nullptr, 0x00, 0x04, 1, 2);

    // Element 0: 1==1 -> true (bit 0 set)
    // Element 1: 2==3 -> false (bit 1 not set)
    // OR (bit 4) = true, AND (bit 5) = false
    uint32_t cc = ctx.vfpu_ctrl[VFPU_CTRL_CC];
    ASSERT_INT_EQ(cc & 0x3F, 0x11u,
                  "vcmp EQ pair: bits 0,4 set");
}

// ===================================================================
// Test 10: lv.q into C300..C330 fills 16 DISJOINT slots
// Regression for issue #27: the old layout aliased all four columns
// of a matrix onto the same 4 physical slots (rotated).
// ===================================================================
static void test_lvq_quad_disjoint() {
    std::printf("  test_lvq_quad_disjoint...\n");
    recomp_context ctx;
    init_ctx(ctx);

    // Small fake rdram: PSP address 0x08000000 masks to offset 0.
    uint8_t* rdram = static_cast<uint8_t*>(std::calloc(1, 4096));
    const uint32_t base = 0x08000000u;
    for (int i = 0; i < 16; i++) {
        float f = static_cast<float>(i + 1);  // 1..16, all distinct
        std::memcpy(rdram + i * 4, &f, 4);
    }

    ctx.r[8] = static_cast<int32_t>(base);  // t0
    vfpu_lv_q(&ctx, rdram, 0x0C, 8, 0);   // C300 <- 1,2,3,4
    vfpu_lv_q(&ctx, rdram, 0x0D, 8, 16);  // C310 <- 5,6,7,8
    vfpu_lv_q(&ctx, rdram, 0x0E, 8, 32);  // C320 <- 9,10,11,12
    vfpu_lv_q(&ctx, rdram, 0x0F, 8, 48);  // C330 <- 13,14,15,16

    // All 16 physical slots of mtx3 (flat 48..63) must hold the 16
    // distinct values: column c, row r -> flat 48 + c*4 + r.
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            char msg[64];
            std::snprintf(msg, sizeof(msg),
                          "lv.q disjoint mtx3 col%d row%d", c, r);
            ASSERT_EXACT(ctx.vfpu[48 + c * 4 + r],
                         static_cast<float>(c * 4 + r + 1), msg);
        }
    }

    // Read back per element through the accessors (single reg
    // encoding: mtx=3, col=c, row=r -> reg = (r<<5)|(3<<2)|c).
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            float v = 0.0f;
            int reg = (r << 5) | (3 << 2) | c;
            vfpu_read_vector(&v, 1, reg, ctx.vfpu);
            char msg[64];
            std::snprintf(msg, sizeof(msg),
                          "lv.q readback S3%d%d", c, r);
            ASSERT_EXACT(v, static_cast<float>(c * 4 + r + 1), msg);
        }
    }

    std::free(rdram);
}

// ===================================================================
// Test 11: transpose round-trip -- write M-form, read E-form
// vmmov M100 <- E000 must produce the transpose of mtx0 in mtx1.
// ===================================================================
static void test_transpose_roundtrip() {
    std::printf("  test_transpose_roundtrip...\n");
    recomp_context ctx;
    init_ctx(ctx);

    // Fill mtx0 with value == flat offset (0..15).
    for (int i = 0; i < 16; i++) {
        ctx.vfpu[i] = static_cast<float>(i);
    }

    // vmmov vd=M100 (0x04), vs=E000 (0x20, transpose form), size=4.
    vfpu_vmmov(&ctx, nullptr, 0x04, 0x20, 4);

    // mtx1 flat[j*4+i] must equal mtx0 flat[i*4+j].
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 4; i++) {
            char msg[64];
            std::snprintf(msg, sizeof(msg),
                          "transpose roundtrip [%d][%d]", j, i);
            ASSERT_EXACT(ctx.vfpu[16 + j * 4 + i],
                         static_cast<float>(i * 4 + j), msg);
        }
    }
}

// ===================================================================
// Test 12: sceGumOrtho regression (issue #27)
// Replays the exact VFPU op sequence of FUN_08857d10 (sceGumOrtho
// helper) with the title-screen argument family and an identity
// stack top, asserting the textbook column-major ortho output.
// Args: l=0, r=480, b=272, t=0, n=-100, f=76.471 (f-n = 176.471).
// ===================================================================
static void test_sce_gum_ortho() {
    std::printf("  test_sce_gum_ortho...\n");
    recomp_context ctx;
    init_ctx(ctx);

    uint8_t* rdram = static_cast<uint8_t*>(std::calloc(1, 4096));
    const uint32_t SP  = 0x08000100u;  // 6 stack floats
    const uint32_t TOP = 0x08000200u;  // gum stack top (identity)
    const uint32_t OUT = 0x08000300u;  // output matrix

    // Stack float family: -(r+l), r-l, -(t+b), t-b, -(f+n), f-n
    const float A[6] = {-480.0f, 480.0f, 272.0f, -272.0f,
                        -(-100.0f + 76.471f), 176.471f};
    for (int i = 0; i < 6; i++) {
        std::memcpy(rdram + ((SP + i * 4) & 0x07FFFFFFu),
                    &A[i], 4);
    }
    const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                             0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(rdram + (TOP & 0x07FFFFFFu), ident, 64);

    ctx.r[4] = static_cast<int32_t>(TOP);   // a0
    ctx.r[29] = static_cast<int32_t>(SP);   // sp

    // Exact op sequence from FUN_08857d10 (cribbed from the
    // /tmp/vfpu_harness differential harness).
    vfpu_lv_q(&ctx, rdram, 0x0C, 4, 0);
    vfpu_lv_q(&ctx, rdram, 0x0D, 4, 16);
    vfpu_lv_q(&ctx, rdram, 0x0E, 4, 32);
    vfpu_lv_q(&ctx, rdram, 0x0F, 4, 48);
    vfpu_vmidt(&ctx, rdram, 0x24, 4);
    vfpu_lv_s(&ctx, rdram, 0x00, 29, 0);
    vfpu_lv_s(&ctx, rdram, 0x20, 29, 4);
    vfpu_lv_s(&ctx, rdram, 0x40, 29, 8);
    vfpu_lv_s(&ctx, rdram, 0x60, 29, 12);
    vfpu_lv_s(&ctx, rdram, 0x01, 29, 16);
    vfpu_lv_s(&ctx, rdram, 0x21, 29, 20);
    vfpu_vfim(&ctx, 0x41, 0x4000u);  // 2.0f
    vfpu_vneg(&ctx, rdram, 0x00, 0x00, 4);
    vfpu_vneg(&ctx, rdram, 0x01, 0x01, 2);
    vfpu_vrcp(&ctx, rdram, 0x20, 0x20, 1);
    vfpu_vrcp(&ctx, rdram, 0x60, 0x60, 1);
    vfpu_vrcp(&ctx, rdram, 0x21, 0x21, 1);
    vfpu_set_prefix(&ctx, 0, 0x000100E4u);
    vfpu_vmul(&ctx, rdram, 0x04, 0x41, 0x20, 1);
    vfpu_set_prefix(&ctx, 0, 0x000100E4u);
    vfpu_vmul(&ctx, rdram, 0x25, 0x41, 0x60, 1);
    vfpu_vmul(&ctx, rdram, 0x46, 0x41, 0x21, 1);
    vfpu_vmul(&ctx, rdram, 0x07, 0x00, 0x20, 1);
    vfpu_vmul(&ctx, rdram, 0x27, 0x40, 0x60, 1);
    vfpu_vmul(&ctx, rdram, 0x47, 0x01, 0x21, 1);
    vfpu_vmmul(&ctx, rdram, 0x28, 0x04, 0x2C, 4);
    ctx.r[4] = static_cast<int32_t>(OUT);
    vfpu_sv_q(&ctx, rdram, 0x08, 4, 0);
    vfpu_sv_q(&ctx, rdram, 0x09, 4, 16);
    vfpu_sv_q(&ctx, rdram, 0x0A, 4, 32);
    vfpu_sv_q(&ctx, rdram, 0x0B, 4, 48);

    float out[16];
    std::memcpy(out, rdram + (OUT & 0x07FFFFFFu), 64);

    // Textbook column-major ortho:
    //   m[0]  =  2/(r-l)   = 2/480     =  0.00416667
    //   m[5]  =  2/(t-b)   = 2/(-272)  = -0.00735294
    //   m[10] = -2/(f-n)   = -2/176.47 = -0.0113334
    //   m[12] = -(r+l)/(r-l) = -1
    //   m[13] = -(t+b)/(t-b) = -1
    //   m[14] = -(f+n)/(f-n) =  0.133337
    //   m[15] =  1
    ASSERT_APPROX(out[0],  0.00416667f, 1e-7f, "ortho m[0]");
    ASSERT_APPROX(out[5], -0.00735294f, 1e-7f, "ortho m[5]");
    ASSERT_APPROX(out[10], -0.0113334f, 1e-6f, "ortho m[10]");
    ASSERT_APPROX(out[12], -1.0f, 1e-5f, "ortho m[12]");
    ASSERT_APPROX(out[13], -1.0f, 1e-5f, "ortho m[13]");
    ASSERT_APPROX(out[14], 0.133337f, 1e-5f, "ortho m[14]");
    ASSERT_EXACT(out[15], 1.0f, "ortho m[15]");

    // All remaining slots must be exactly zero.
    const int zero_slots[] = {1, 2, 3, 4, 6, 7, 8, 9, 11};
    for (int idx : zero_slots) {
        char msg[32];
        std::snprintf(msg, sizeof(msg), "ortho m[%d]==0", idx);
        ASSERT_EXACT(out[idx], 0.0f, msg);
    }

    std::free(rdram);
}

// ===================================================================
// main
// ===================================================================

int main() {
    std::printf("Running VFPU runtime tests...\n\n");

    test_sin_cardinals();
    test_cos_cardinals();
    test_eat_prefixes();
    test_prefix_swizzle();
    test_vmmul_identity();
    test_trig_noncardinal();
    test_vadd_quad();
    test_vdot();
    test_vcmp_eq();
    test_lvq_quad_disjoint();
    test_transpose_roundtrip();
    test_sce_gum_ortho();

    std::printf("\n%d tests run, %d failures\n",
                tests_run, failures);

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    } else {
        std::printf("SOME TESTS FAILED\n");
        return 1;
    }
}
