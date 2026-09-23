//
//  main.c
//  TSL_FromScratch
//
//  Created by Joel PACHERIE on 9/23/26.
//

// BUILDING IN PROGRESS...

/*
 * =====================================================
 * ============================================================================
 * ! DISCLAIMER & NOTICE OF USE !
 *
 * WARNING: THIS IMPLEMENTATION IS FOR EDUCATIONAL AND DEMONSTRATION PURPOSES ONLY.
 * IT IS DESIGNED TO ILLUSTRATE TLS PROTOCOL MECHANICS AT A LOWEST LEVEL.
 *
 * - DO NOT USE THIS IMPLEMENTATION IN PRODUCTION ENVIRONMENTS.
 * - DO NOT USE THIS SOFTWARE FOR MALICIOUS, OBSCURE, OR ILLEGAL PURPOSES.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES, OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * ============================================================================
 * ====================================================
 */


// C STANDARD
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
// POSIX

// ==================================================================================================================
// - MARK: Diffie Hellman X25519 ---------------------
// ==================================================================================================================

// Ensure GCC/Clang 128-bit support
typedef __uint128_t u128;
typedef uint64_t gf[5];
#define MASK51 0x7FFFFFFFFFFFFULL

// 1. CORE ARITHMETIC
static void load(gf o, const uint8_t *n) {
    uint64_t dummy[4] = {0};
    memcpy(dummy, n, 32);
    o[0] = (dummy[0] >> 0)  & MASK51;
    o[1] = (dummy[0] >> 51) | ((dummy[1] << 13) & MASK51);
    o[2] = (dummy[1] >> 38) | ((dummy[2] << 26) & MASK51);
    o[3] = (dummy[2] >> 25) | ((dummy[3] << 39) & MASK51);
    o[4] = (dummy[3] >> 12) & MASK51;
}

static void store(uint8_t *o, gf a) {
    uint64_t c;
    // Standard carry propagation
    for(int j=0; j<2; j++) {
        c = a[0] >> 51; a[0] &= MASK51; a[1] += c;
        c = a[1] >> 51; a[1] &= MASK51; a[2] += c;
        c = a[2] >> 51; a[2] &= MASK51; a[3] += c;
        c = a[3] >> 51; a[3] &= MASK51; a[4] += c;
        c = a[4] >> 51; a[4] &= MASK51; a[0] += c * 19;
    }

    // Bulletproof constant-time final reduction modulo (2^255 - 19)
    uint64_t dummy[5];
    dummy[0] = a[0] + 19;
    dummy[1] = a[1] + (dummy[0] >> 51); dummy[0] &= MASK51;
    dummy[2] = a[2] + (dummy[1] >> 51); dummy[1] &= MASK51;
    dummy[3] = a[3] + (dummy[2] >> 51); dummy[2] &= MASK51;
    dummy[4] = a[4] + (dummy[3] >> 51); dummy[3] &= MASK51;

    // If adding 19 caused a carry into bit 255, the original value was >= P
    uint64_t mask = -(dummy[4] >> 51);
    dummy[4] &= MASK51; // Clear the 255th bit to simulate subtracting P

    a[0] = (a[0] & ~mask) | (dummy[0] & mask);
    a[1] = (a[1] & ~mask) | (dummy[1] & mask);
    a[2] = (a[2] & ~mask) | (dummy[2] & mask);
    a[3] = (a[3] & ~mask) | (dummy[3] & mask);
    a[4] = (a[4] & ~mask) | (dummy[4] & mask);

    uint64_t out[4];
    out[0] = a[0] | (a[1] << 51);
    out[1] = (a[1] >> 13) | (a[2] << 38);
    out[2] = (a[2] >> 26) | (a[3] << 25);
    out[3] = (a[3] >> 39) | (a[4] << 12);
    memcpy(o, out, 32);
}

static void add(gf o, const gf a, const gf b) {
    for (int i = 0; i < 5; i++) o[i] = a[i] + b[i];
}

static void sub(gf o, const gf a, const gf b) {
    // Add 2*P to avoid underflow
    o[0] = (a[0] + 0xFFFFFFFFFFFDAULL) - b[0];
    o[1] = (a[1] + 0xFFFFFFFFFFFFEULL) - b[1];
    o[2] = (a[2] + 0xFFFFFFFFFFFFEULL) - b[2];
    o[3] = (a[3] + 0xFFFFFFFFFFFFEULL) - b[3];
    o[4] = (a[4] + 0xFFFFFFFFFFFFEULL) - b[4];
}

static void mul(gf o, const gf a, const gf b) {
    u128 t[5];
    uint64_t b1_19 = b[1] * 19, b2_19 = b[2] * 19, b3_19 = b[3] * 19, b4_19 = b[4] * 19;

    t[0] = (u128)a[0]*b[0] + (u128)a[1]*b4_19 + (u128)a[2]*b3_19 + (u128)a[3]*b2_19 + (u128)a[4]*b1_19;
    t[1] = (u128)a[0]*b[1] + (u128)a[1]*b[0]  + (u128)a[2]*b4_19 + (u128)a[3]*b3_19 + (u128)a[4]*b2_19;
    t[2] = (u128)a[0]*b[2] + (u128)a[1]*b[1]  + (u128)a[2]*b[0]  + (u128)a[3]*b4_19 + (u128)a[4]*b3_19;
    t[3] = (u128)a[0]*b[3] + (u128)a[1]*b[2]  + (u128)a[2]*b[1]  + (u128)a[3]*b[0]  + (u128)a[4]*b4_19;
    t[4] = (u128)a[0]*b[4] + (u128)a[1]*b[3]  + (u128)a[2]*b[2]  + (u128)a[3]*b[1]  + (u128)a[4]*b[0];

    uint64_t c;
    o[0] = (uint64_t)t[0] & MASK51; c = (uint64_t)(t[0] >> 51);
    t[1] += c; o[1] = (uint64_t)t[1] & MASK51; c = (uint64_t)(t[1] >> 51);
    t[2] += c; o[2] = (uint64_t)t[2] & MASK51; c = (uint64_t)(t[2] >> 51);
    t[3] += c; o[3] = (uint64_t)t[3] & MASK51; c = (uint64_t)(t[3] >> 51);
    t[4] += c; o[4] = (uint64_t)t[4] & MASK51; c = (uint64_t)(t[4] >> 51);
    o[0] += c * 19; c = o[0] >> 51; o[0] &= MASK51; o[1] += c;
}

static void sqr(gf o, const gf a) {
    u128 t[5];
    uint64_t a0_2 = a[0]*2, a1_2 = a[1]*2;// a2_2 = a[2]*2;
    uint64_t a4_19 = a[4]*19, a3_19 = a[3]*19, a1_19_2 = a[1]*38;

    t[0] = (u128)a[0]*a[0] + (u128)a1_19_2*a[4] + (u128)a[2]*a3_19*2;
    t[1] = (u128)a0_2*a[1] + (u128)a[2]*a4_19*2 + (u128)a[3]*a3_19;
    t[2] = (u128)a0_2*a[2] + (u128)a[1]*a[1]    + (u128)a[4]*a3_19*2;
    t[3] = (u128)a0_2*a[3] + (u128)a1_2*a[2]    + (u128)a[4]*a4_19;
    t[4] = (u128)a0_2*a[4] + (u128)a1_2*a[3]    + (u128)a[2]*a[2];

    uint64_t c;
    o[0] = (uint64_t)t[0] & MASK51; c = (uint64_t)(t[0] >> 51);
    t[1] += c; o[1] = (uint64_t)t[1] & MASK51; c = (uint64_t)(t[1] >> 51);
    t[2] += c; o[2] = (uint64_t)t[2] & MASK51; c = (uint64_t)(t[2] >> 51);
    t[3] += c; o[3] = (uint64_t)t[3] & MASK51; c = (uint64_t)(t[3] >> 51);
    t[4] += c; o[4] = (uint64_t)t[4] & MASK51; c = (uint64_t)(t[4] >> 51);
    o[0] += c * 19; c = o[0] >> 51; o[0] &= MASK51; o[1] += c;
}

static void sel(gf p, gf q, int b) {
    uint64_t mask = -(uint64_t)b;
    for (int i = 0; i < 5; i++) {
        uint64_t t = mask & (p[i] ^ q[i]);
        p[i] ^= t; q[i] ^= t;
    }
}

// 2. INVERSION ADDITION CHAIN
static void inv(gf out, const gf z) {
    gf t0, t1, z2, z9, z11, z2_10_0, z2_50_0, z2_100_0;
    sqr(z2, z); sqr(t0, z2); sqr(t1, t0);
    mul(z9, t1, z); mul(z11, z9, z2);
    sqr(t0, z11); mul(t0, t0, z9);
    
    sqr(t1, t0); for (int i = 1; i < 5; ++i) sqr(t1, t1);
    mul(z2_10_0, t1, t0);
    sqr(t1, z2_10_0); for (int i = 1; i < 10; ++i) sqr(t1, t1);
    mul(t1, t1, z2_10_0);
    sqr(t0, t1); for (int i = 1; i < 20; ++i) sqr(t0, t0);
    mul(t0, t0, t1);
    sqr(t0, t0); for (int i = 1; i < 10; ++i) sqr(t0, t0);
    mul(z2_50_0, t0, z2_10_0);
    sqr(t0, z2_50_0); for (int i = 1; i < 50; ++i) sqr(t0, t0);
    mul(z2_100_0, t0, z2_50_0);
    sqr(t0, z2_100_0); for (int i = 1; i < 100; ++i) sqr(t0, t0);
    mul(t0, t0, z2_100_0);
    sqr(t0, t0); for (int i = 1; i < 50; ++i) sqr(t0, t0);
    mul(t0, t0, z2_50_0);
    sqr(t0, t0); for (int i = 1; i < 5; ++i) sqr(t0, t0);
    mul(out, t0, z11);
}

// 3. MONTGOMERY LADDER
void x25519(uint8_t *q, const uint8_t *scalar, const uint8_t *point) {
    uint8_t s[32];
    gf x1, x2 = {1}, z2 = {0}, x3, z3 = {1};
    const gf a24 = {121665}; // (A-2)/4

    // RFC 7748 Clamping
    memcpy(s, scalar, 32);
    s[0] &= 248;
    s[31] = (s[31] & 127) | 64;

    load(x1, point);
    for (int i = 0; i < 5; i++) x3[i] = x1[i];

    int prev_bit = 0;
    for (int i = 254; i >= 0; i--) {
        int bit = (s[i >> 3] >> (i & 7)) & 1;
        int swap = bit ^ prev_bit;
        prev_bit = bit;

        sel(x2, x3, swap);
        sel(z2, z3, swap);

        // Scoped variables map directly to equations, preventing C register collision
        gf A, B, C, D, AA, BB, DA, CB, E, t1, t2;

        add(A, x2, z2);   // A = X2 + Z2
        sub(B, x2, z2);   // B = X2 - Z2
        add(C, x3, z3);   // C = X3 + Z3
        sub(D, x3, z3);   // D = X3 - Z3

        sqr(AA, A);       // AA = A^2
        sqr(BB, B);       // BB = B^2

        mul(DA, D, A);    // DA = D * A
        mul(CB, C, B);    // CB = C * B

        add(t1, DA, CB);  // DA + CB
        sqr(x3, t1);      // X3 = (DA + CB)^2

        sub(t2, DA, CB);  // DA - CB
        sqr(t2, t2);      // (DA - CB)^2
        mul(z3, t2, x1);  // Z3 = x * (DA - CB)^2

        sub(E, AA, BB);   // E = AA - BB

        mul(x2, AA, BB);  // X2 = AA * BB

        mul(t1, E, a24);  // a24 * E
        add(t1, t1, AA);  // AA + a24 * E
        mul(z2, E, t1);   // Z2 = E * (AA + a24 * E)
    }
    
    sel(x2, x3, prev_bit);
    sel(z2, z3, prev_bit);

    // Final conversion to affine coordinates
    inv(z2, z2);
    mul(x2, x2, z2);
    store(q, x2);
}

// ==================================================================================================================
// - MARK: Hellman X25519 ---------------------
// ==================================================================================================================


//
int main(int argc, const char * argv[]) {
    return EXIT_SUCCESS;
}
