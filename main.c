//
//  main.c
//  TSL_FromScratch
//
//  Created by Joel PACHERIE on 9/23/26.
//

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
// POSIX NETWORKING
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

static void print_hex(const char *title, const uint8_t *buf, size_t len) {
    printf("%s: ", title);for (size_t i = 0; i < len; i++) {printf("%02x", buf[i]);}printf("\n");
}

// ==================================================================================================================
// - MARK: Entropy --------------------------------------------------------------------------------------------------
// ==================================================================================================================

uint64_t get_cpu_jitter_entropy_64(void) {
    uint64_t delta,entropy = 0U, now = 0U, prev = 0U; uint32_t j,iterations; uint8_t i; // STACK
    // 1. Initial timer read (Warm-up)
    #if defined(__x86_64__) || defined(__i386__)
        uint32_t lo, hi;
        __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
        prev = ((uint64_t)hi << 32) | lo;
    #elif defined(__aarch64__)
        __asm__ __volatile__ ("mrs %0, cntvct_el0" : "=r" (prev));
    #else
        #error "Architecture not supported for inline ASM timer read !"
    #endif
    // 2. Accumulate 64 bits of entropy
    for (i = 0U; i < 64U; i++) {
        // Induce execution time jitter via a data-dependent, unpredictable loop.
        // 'volatile' ensures GCC's optimizer does not strip this out. */
        volatile uint32_t noise_accumulator = 0U;
        // Loop between 1 and 64 times based on the lowest bits of the previous timestamp.
        // This causes deliberate branch prediction variations and pipeline jitter.
        iterations = (prev & 0x3F) + 1;
        for (j = 0U; j < iterations; j++) {noise_accumulator ^= j;}
        // 3. Read timer again */
        #if defined(__x86_64__) || defined(__i386__)
            __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
            now = ((uint64_t)hi << 32) | lo;
        #elif defined(__aarch64__)
            __asm__ __volatile__ ("mrs %0, cntvct_el0" : "=r" (now));
        #endif
        // 4. Calculate execution time delta */
        delta = now - prev;
        // 5. Stir the delta into the accumulator.
        // We rotate the current entropy left by 1 to spread bits,
        // then XOR the delta (where the LSBs contain the most unpredictable noise).
        entropy = (entropy << 1) | (entropy >> 63);
        entropy ^= (delta + noise_accumulator); // Include noise to ensure compiler keeps it
        prev = now;
    }
    
    // The Avalanche step (MurmurHash3 64-bit finalizer).
    // Forces a uniform distribution by violently mixing the bits.
    // Latency: ~3-5 CPU cycles.
    entropy ^= entropy >> 33;
    entropy *= 0xff51afd7ed558ccdULL;
    entropy ^= entropy >> 33;
    entropy *= 0xc4ceb9fe1a85ec53ULL;
    entropy ^= entropy >> 33;
    return entropy;
}

void get_cpu_jitter_entropy_256(uint8_t * __restrict__ dst) {
    uint64_t state[4], prev, now, delta, entropy64; uint32_t iterations; uint8_t i,j; // STACK

    // 1. Initial timer read (Warm-up)
    #if defined(__x86_64__) || defined(__i386__)
        prev = __builtin_ia32_rdtsc();
    #elif defined(__aarch64__)
        __asm__ __volatile__ ("mrs %0, cntvct_el0" : "=r" (prev));
    #else
        #error "Architecture not supported for inline ASM timer read !"
    #endif

    // 2. Force loop unrolling to eliminate outer/inner branch evaluation overhead.
    // We want the only branch mispredictions to come from our jitter loop.
    #pragma GCC unroll 4
    for (i= 0U; i < 4U; i++) {
        entropy64 = 0U;
        #pragma GCC unroll 64
        for (j = 0; j < 64; j++) {
            iterations = (prev & 0x3F) + 1; // Branch from 1 to 64 times
            // 3. Pure register-bound jitter delay (0 L1 cache latency)
            #if defined(__x86_64__) || defined(__i386__)
                __asm__ __volatile__ (
                    "1: dec %0 \n\t"
                    "jnz 1b \n\t"
                    : "+r" (iterations)
                    : // No inputs
                    : "cc" // Clobbers CPU condition flags
                );
                now = __builtin_ia32_rdtsc();
            #elif defined(__aarch64__)
                __asm__ __volatile__ (
                    "1: subs %w0, %w0, #1 \n\t"
                    "bne 1b \n\t"
                    : "+r" (iterations)
                    :
                    : "cc"
                );
                __asm__ __volatile__ ("mrs %0, cntvct_el0" : "=r" (now));
            #endif
            // 4. Time delta holds the entropy
            delta = now - prev;
            // 5. Stir the delta
            entropy64 = (entropy64 << 1) | (entropy64 >> 63);
            entropy64 ^= delta;
            prev = now;
        }
        state[i] = entropy64;
    }
    // 6. The Avalanche step (Superscalar execution)
    uint64_t a = state[0], b = state[1], c = state[2],d = state[3];
    // Out-of-order execution engines will blast through these 12 operations
    // across 4 independent dependency chains concurrently.
    a ^= a >> 33; a *= 0xff51afd7ed558ccdULL; a ^= a >> 33; a *= 0xc4ceb9fe1a85ec53ULL; a ^= a >> 33;
    b ^= b >> 33; b *= 0xff51afd7ed558ccdULL; b ^= b >> 33; b *= 0xc4ceb9fe1a85ec53ULL; b ^= b >> 33;
    c ^= c >> 33; c *= 0xff51afd7ed558ccdULL; c ^= c >> 33; c *= 0xc4ceb9fe1a85ec53ULL; c ^= c >> 33;
    d ^= d >> 33; d *= 0xff51afd7ed558ccdULL; d ^= d >> 33; d *= 0xc4ceb9fe1a85ec53ULL; d ^= d >> 33;
    // 7. Write once at the end
    uint64_t *out = (uint64_t*)dst;
    out[0] = a; out[1] = b; out[2] = c; out[3] = d;
}

// ==================================================================================================================
// - MARK: Diffie Hellman X25519 ------------------------------------------------------------------------------------
// ==================================================================================================================

// Ensure GCC/Clang 128-bit support
typedef __uint128_t u128;
typedef uint64_t gf[5];
#define MASK51 0x7FFFFFFFFFFFFULL

// --- 1. CORE ARITHMETIC
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
    for(int j=0; j<2; j++) { // Standard carry propagation
        c = a[0] >> 51; a[0] &= MASK51; a[1] += c;
        c = a[1] >> 51; a[1] &= MASK51; a[2] += c;
        c = a[2] >> 51; a[2] &= MASK51; a[3] += c;
        c = a[3] >> 51; a[3] &= MASK51; a[4] += c;
        c = a[4] >> 51; a[4] &= MASK51; a[0] += c * 19;
    }
    uint64_t dummy[5]; // Bulletproof constant-time final reduction modulo (2^255 - 19)
    dummy[0] = a[0] + 19;
    dummy[1] = a[1] + (dummy[0] >> 51); dummy[0] &= MASK51;
    dummy[2] = a[2] + (dummy[1] >> 51); dummy[1] &= MASK51;
    dummy[3] = a[3] + (dummy[2] >> 51); dummy[2] &= MASK51;
    dummy[4] = a[4] + (dummy[3] >> 51); dummy[3] &= MASK51;

    uint64_t mask = -(dummy[4] >> 51); // If adding 19 caused a carry into bit 255, the original value was >= P
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

static void sub(gf o, const gf a, const gf b) { // Add 2*P to avoid underflow
    o[0] = (a[0] + 0xFFFFFFFFFFFDAULL) - b[0]; o[1] = (a[1] + 0xFFFFFFFFFFFFEULL) - b[1];
    o[2] = (a[2] + 0xFFFFFFFFFFFFEULL) - b[2]; o[3] = (a[3] + 0xFFFFFFFFFFFFEULL) - b[3];
    o[4] = (a[4] + 0xFFFFFFFFFFFFEULL) - b[4];
}

static void mul(gf o, const gf a, const gf b) {
    u128 t[5]; uint64_t c,b1_19 = b[1] * 19, b2_19 = b[2] * 19, b3_19 = b[3] * 19, b4_19 = b[4] * 19;
    t[0] = (u128)a[0]*b[0] + (u128)a[1]*b4_19 + (u128)a[2]*b3_19 + (u128)a[3]*b2_19 + (u128)a[4]*b1_19;
    t[1] = (u128)a[0]*b[1] + (u128)a[1]*b[0]  + (u128)a[2]*b4_19 + (u128)a[3]*b3_19 + (u128)a[4]*b2_19;
    t[2] = (u128)a[0]*b[2] + (u128)a[1]*b[1]  + (u128)a[2]*b[0]  + (u128)a[3]*b4_19 + (u128)a[4]*b3_19;
    t[3] = (u128)a[0]*b[3] + (u128)a[1]*b[2]  + (u128)a[2]*b[1]  + (u128)a[3]*b[0]  + (u128)a[4]*b4_19;
    t[4] = (u128)a[0]*b[4] + (u128)a[1]*b[3]  + (u128)a[2]*b[2]  + (u128)a[3]*b[1]  + (u128)a[4]*b[0];
    o[0] = (uint64_t)t[0] & MASK51; c = (uint64_t)(t[0] >> 51);
    t[1] += c; o[1] = (uint64_t)t[1] & MASK51; c = (uint64_t)(t[1] >> 51);
    t[2] += c; o[2] = (uint64_t)t[2] & MASK51; c = (uint64_t)(t[2] >> 51);
    t[3] += c; o[3] = (uint64_t)t[3] & MASK51; c = (uint64_t)(t[3] >> 51);
    t[4] += c; o[4] = (uint64_t)t[4] & MASK51; c = (uint64_t)(t[4] >> 51);
    o[0] += c * 19; c = o[0] >> 51; o[0] &= MASK51; o[1] += c;
}

static void sqr(gf o, const gf a) {
    u128 t[5]; uint64_t c=0U,a0_2 = a[0]*2, a1_2 = a[1]*2, a4_19 = a[4]*19, a3_19 = a[3]*19, a1_19_2 = a[1]*38;// a2_2 = a[2]*2;
    t[0] = (u128)a[0]*a[0] + (u128)a1_19_2*a[4] + (u128)a[2]*a3_19*2;
    t[1] = (u128)a0_2*a[1] + (u128)a[2]*a4_19*2 + (u128)a[3]*a3_19;
    t[2] = (u128)a0_2*a[2] + (u128)a[1]*a[1]    + (u128)a[4]*a3_19*2;
    t[3] = (u128)a0_2*a[3] + (u128)a1_2*a[2]    + (u128)a[4]*a4_19;
    t[4] = (u128)a0_2*a[4] + (u128)a1_2*a[3]    + (u128)a[2]*a[2];
    o[0] = (uint64_t)t[0] & MASK51; c = (uint64_t)(t[0] >> 51);
    t[1] += c; o[1] = (uint64_t)t[1] & MASK51; c = (uint64_t)(t[1] >> 51);
    t[2] += c; o[2] = (uint64_t)t[2] & MASK51; c = (uint64_t)(t[2] >> 51);
    t[3] += c; o[3] = (uint64_t)t[3] & MASK51; c = (uint64_t)(t[3] >> 51);
    t[4] += c; o[4] = (uint64_t)t[4] & MASK51; c = (uint64_t)(t[4] >> 51);
    o[0] += c * 19; c = o[0] >> 51; o[0] &= MASK51; o[1] += c;
}

static void sel(gf p, gf q, int b) {
    uint64_t t,mask = -(uint64_t)b; for (int i = 0; i < 5; i++) {t= mask & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t;}
}

// 2. INVERSION ADDITION CHAIN
static void inv(gf out, const gf z) {
    gf t0, t1, z2, z9, z11, z2_10_0, z2_50_0, z2_100_0;
    sqr(z2, z); sqr(t0, z2); sqr(t1, t0);
    mul(z9, t1, z); mul(z11, z9, z2);
    sqr(t0, z11); mul(t0, t0, z9);
    //
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

// MONTGOMERY LADDER

void x25519(uint8_t *q, const uint8_t *scalar, const uint8_t *point) {
    uint8_t s[32]; gf x1, x2 = {1}, z2 = {0}, x3, z3 = {1}; const gf a24 = {121665}; // (A-2)/4
    memcpy(s, scalar, 32); s[0] &= 248;s[31] = (s[31] & 127) | 64; // RFC 7748 Clamping
    load(x1, point); for (int i = 0; i < 5; i++){ x3[i] = x1[i];}

    int bit,swap,prev_bit = 0;
    for (int i = 254; i >= 0; i--) {
        bit = (s[i >> 3] >> (i & 7)) & 1;
        swap = bit ^ prev_bit;
        prev_bit = bit;
        sel(x2, x3, swap);
        sel(z2, z3, swap);
        // Scoped variables map directly to equations, preventing C register collision
        gf A, B, C, D, AA, BB, DA, CB, E, t1, t2;
        add(A, x2, z2);   // A = X2 + Z2
        sub(B, x2, z2);   // B = X2 - Z2
        add(C, x3, z3);   // C = X3 + Z3
        sub(D, x3, z3);   // D = X3 - Z3
        //
        sqr(AA, A);       // AA = A^2
        sqr(BB, B);       // BB = B^2
        //
        mul(DA, D, A);    // DA = D * A
        mul(CB, C, B);    // CB = C * B
        //
        add(t1, DA, CB);  // DA + CB
        sqr(x3, t1);      // X3 = (DA + CB)^2
        //
        sub(t2, DA, CB);  // DA - CB
        sqr(t2, t2);      // (DA - CB)^2
        mul(z3, t2, x1);  // Z3 = x * (DA - CB)^2
        //
        sub(E, AA, BB);   // E = AA - BB
        //
        mul(x2, AA, BB);  // X2 = AA * BB
        //
        mul(t1, E, a24);  // a24 * E
        add(t1, t1, AA);  // AA + a24 * E
        mul(z2, E, t1);   // Z2 = E * (AA + a24 * E)
    }
    sel(x2, x3, prev_bit);
    sel(z2, z3, prev_bit);
    inv(z2, z2); mul(x2, x2, z2); store(q, x2); // Final conversion to affine coordinates
}



// ==================================================================================================================
// - MARK: SHA-256 Digest Engine ------------------------------------------------------------------------------------
// ==================================================================================================================

typedef struct {
    uint8_t data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} sha256_ctx;

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_transform(sha256_ctx *ctx, const uint8_t data[64]) {
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64]; uint8_t i,j;
    for (i = 0, j = 0; i < 16; ++i, j += 4) {
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) | ((uint32_t)data[j + 2] << 8) | ((uint32_t)data[j + 3]);
    }
    for (i = 16; i < 64; ++i){m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];}

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];
    for (i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e, f, g) + K256[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha256_init(sha256_ctx *ctx) {
    ctx->datalen = 0; ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen] = data[i]; ctx->datalen++;
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512; ctx->datalen = 0;
        }
    }
}

void sha256_final(sha256_ctx *ctx, uint8_t hash[32]) {
    uint32_t i = ctx->datalen;
    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) {ctx->data[i++] = 0x00;}
    }
    else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0x00;
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }
    ctx->bitlen += ctx->datalen * 8;
    ctx->data[56] = (ctx->bitlen >> 56) & 0xFF; ctx->data[57] = (ctx->bitlen >> 48) & 0xFF;
    ctx->data[58] = (ctx->bitlen >> 40) & 0xFF; ctx->data[59] = (ctx->bitlen >> 32) & 0xFF;
    ctx->data[60] = (ctx->bitlen >> 24) & 0xFF; ctx->data[61] = (ctx->bitlen >> 16) & 0xFF;
    ctx->data[62] = (ctx->bitlen >> 8) & 0xFF;  ctx->data[63] = ctx->bitlen & 0xFF;
    sha256_transform(ctx, ctx->data);

    for (i = 0; i < 4; ++i) {
        hash[i]      = (ctx->state[0] >> (24 - i * 8)) & 0xFF; hash[i + 4]  = (ctx->state[1] >> (24 - i * 8)) & 0xFF;
        hash[i + 8]  = (ctx->state[2] >> (24 - i * 8)) & 0xFF; hash[i + 12] = (ctx->state[3] >> (24 - i * 8)) & 0xFF;
        hash[i + 16] = (ctx->state[4] >> (24 - i * 8)) & 0xFF; hash[i + 20] = (ctx->state[5] >> (24 - i * 8)) & 0xFF;
        hash[i + 24] = (ctx->state[6] >> (24 - i * 8)) & 0xFF; hash[i + 28] = (ctx->state[7] >> (24 - i * 8)) & 0xFF;
    }
}

static void sha256(const uint8_t *data, size_t len, uint8_t hash[32]) {
    sha256_ctx ctx;
    sha256_init(&ctx);
    if (data && len > 0) sha256_update(&ctx, data, len);
    sha256_final(&ctx, hash);
}

// ==================================================================================================================
// - MARK: HMAC-SHA256 & HKDF Engine -------------------------------------------------------------------------------
// ==================================================================================================================

static void hmac_sha256(const uint8_t *key, size_t key_len,
                        const uint8_t *data, size_t data_len,
                        uint8_t mac[32]) {
    sha256_ctx ctx;
    uint8_t k_ipad[64], k_opad[64], tk[32];

    if (key_len > 64) {
        sha256(key, key_len, tk);
        key = tk;
        key_len = 32;
    }

    memset(k_ipad, 0x36, 64);
    memset(k_opad, 0x5c, 64);

    for (size_t i = 0; i < key_len; i++) {
        k_ipad[i] ^= key[i];
        k_opad[i] ^= key[i];
    }

    sha256_init(&ctx);
    sha256_update(&ctx, k_ipad, 64);
    if (data && data_len > 0) sha256_update(&ctx, data, data_len);
    sha256_final(&ctx, mac);

    sha256_init(&ctx);
    sha256_update(&ctx, k_opad, 64);
    sha256_update(&ctx, mac, 32);
    sha256_final(&ctx, mac);
}

static void hkdf_extract(const uint8_t *salt, size_t salt_len,
                         const uint8_t *ikm, size_t ikm_len,
                         uint8_t prk[32]) {
    uint8_t zero_salt[32] = {0};
    if (salt == NULL || salt_len == 0) {
        salt = zero_salt;
        salt_len = 32;
    }
    hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
}

static void hkdf_expand(const uint8_t *prk, size_t prk_len,
                        const uint8_t *info, size_t info_len,
                        uint8_t *okm, size_t okm_len) {
    uint8_t T[32];
    size_t t_len = 0;
    uint8_t counter = 1;
    size_t pos = 0;

    while (pos < okm_len) {
        sha256_ctx ctx;
        uint8_t k_ipad[64], k_opad[64], tk[32];
        const uint8_t *key = prk;
        size_t key_len = prk_len;

        if (key_len > 64) {
            sha256(key, key_len, tk);
            key = tk;
            key_len = 32;
        }

        memset(k_ipad, 0x36, 64);
        memset(k_opad, 0x5c, 64);
        for (size_t i = 0; i < key_len; i++) {
            k_ipad[i] ^= key[i];
            k_opad[i] ^= key[i];
        }

        sha256_init(&ctx);
        sha256_update(&ctx, k_ipad, 64);
        if (t_len > 0) sha256_update(&ctx, T, t_len);
        if (info && info_len > 0) sha256_update(&ctx, info, info_len);
        sha256_update(&ctx, &counter, 1);
        sha256_final(&ctx, T);

        sha256_init(&ctx);
        sha256_update(&ctx, k_opad, 64);
        sha256_update(&ctx, T, 32);
        sha256_final(&ctx, T);

        t_len = 32;
        size_t copy_len = (okm_len - pos < 32) ? (okm_len - pos) : 32;
        memcpy(okm + pos, T, copy_len);
        pos += copy_len;
        counter++;
    }
}

static void hkdf_expand_label(const uint8_t *secret, size_t secret_len,
                              const char *label,
                              const uint8_t *context, size_t context_len,
                              uint16_t out_len, uint8_t *out) {
    uint8_t hkdf_label[512];
    size_t pos = 0;

    // uint16 length (big-endian)
    hkdf_label[pos++] = (out_len >> 8) & 0xFF;
    hkdf_label[pos++] = out_len & 0xFF;

    // opaque label<7..255> = "tls13 " + label
    const char *prefix = "tls13 ";
    size_t prefix_len = 6;
    size_t label_len = strlen(label);
    uint8_t full_label_len = (uint8_t)(prefix_len + label_len);

    hkdf_label[pos++] = full_label_len;
    memcpy(&hkdf_label[pos], prefix, prefix_len);
    pos += prefix_len;
    memcpy(&hkdf_label[pos], label, label_len);
    pos += label_len;

    // opaque context<0..255>
    hkdf_label[pos++] = (uint8_t)context_len;
    if (context_len > 0 && context != NULL) {
        memcpy(&hkdf_label[pos], context, context_len);
        pos += context_len;
    }

    hkdf_expand(secret, secret_len, hkdf_label, pos, out, out_len);
}

static void derive_secret(const uint8_t *secret, size_t secret_len,
                          const char *label,
                          const uint8_t *transcript_hash, size_t hash_len,
                          uint8_t *out) {
    hkdf_expand_label(secret, secret_len, label, transcript_hash, hash_len, (uint16_t)hash_len, out);
}

// ==================================================================================================================
// - MARK: TLS 1.3 Key Schedule Integration ------------------------------------------------------------------------
// ==================================================================================================================

// Transcript = ClientHello..ServerHello. Outputs the handshake secret (needed later for the master secret)
// plus both handshake traffic secrets.
void tls13_derive_handshake_keys(const uint8_t ecdh_shared_secret[32],
                                 const uint8_t transcript_hash[32],
                                 uint8_t handshake_secret[32],
                                 uint8_t client_hs_traffic_secret[32],
                                 uint8_t server_hs_traffic_secret[32]) {
    uint8_t zero32[32] = {0};
    uint8_t early_secret[32];

    // 1. Early Secret = HKDF-Extract(Salt=0, IKM=0)
    hkdf_extract(zero32, 32, zero32, 32, early_secret);

    // 2. Derived Secret = Derive-Secret(Early Secret, "derived", SHA256(""))
    uint8_t empty_hash[32];
    sha256(NULL, 0, empty_hash);

    uint8_t derived_secret[32];
    derive_secret(early_secret, 32, "derived", empty_hash, 32, derived_secret);

    // 3. Handshake Secret = HKDF-Extract(Salt=derived_secret, IKM=ecdh_shared_secret)
    hkdf_extract(derived_secret, 32, ecdh_shared_secret, 32, handshake_secret);

    // 4. Client & Server Handshake Traffic Secrets
    derive_secret(handshake_secret, 32, "c hs traffic", transcript_hash, 32, client_hs_traffic_secret);
    derive_secret(handshake_secret, 32, "s hs traffic", transcript_hash, 32, server_hs_traffic_secret);
}

// Transcript = ClientHello..server Finished.
// Master Secret = HKDF-Extract(Salt=Derive-Secret(HS, "derived", ""), IKM=0)
void tls13_derive_app_secrets(const uint8_t handshake_secret[32],
                              const uint8_t transcript_hash_sf[32],
                              uint8_t client_ap_traffic_secret[32],
                              uint8_t server_ap_traffic_secret[32]) {
    uint8_t zero32[32] = {0}, empty_hash[32], derived[32], master[32];
    sha256(NULL, 0, empty_hash);
    derive_secret(handshake_secret, 32, "derived", empty_hash, 32, derived);
    hkdf_extract(derived, 32, zero32, 32, master);
    derive_secret(master, 32, "c ap traffic", transcript_hash_sf, 32, client_ap_traffic_secret);
    derive_secret(master, 32, "s ap traffic", transcript_hash_sf, 32, server_ap_traffic_secret);
}

// Finished.verify_data = HMAC(HKDF-Expand-Label(base_secret, "finished", "", 32), Transcript-Hash)
static void tls13_finished_verify_data(const uint8_t base_secret[32], const uint8_t transcript_hash[32],
                                       uint8_t out[32]) {
    uint8_t finished_key[32];
    hkdf_expand_label(base_secret, 32, "finished", NULL, 0, 32, finished_key);
    hmac_sha256(finished_key, 32, transcript_hash, 32, out);
}

static int ct_eq(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

// ==================================================================================================================
// - MARK: AES-128-GCM (AEAD) ---------------------------------------------------------------------------------------
// ==================================================================================================================
// Table-driven AES + bitwise GHASH: readable, NOT constant-time.

static uint8_t SBOX256[256];
#define ROTL8(x, s) ((uint8_t)(((x) << (s)) | ((x) >> (8 - (s)))))

// Generates the S-box from GF(2^8) inversion + affine transform (no 256-byte table to typo).
static void aes_init_sbox(void) {
    uint8_t p = 1, q = 1;
    do {
        p = p ^ (uint8_t)(p << 1) ^ ((p & 0x80) ? 0x1B : 0); // p *= 3
        q ^= (uint8_t)(q << 1); q ^= (uint8_t)(q << 2); q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09; // q /= 3
        SBOX256[p] = q ^ ROTL8(q, 1) ^ ROTL8(q, 2) ^ ROTL8(q, 3) ^ ROTL8(q, 4) ^ 0x63;
    } while (p != 1);
    SBOX256[0] = 0x63;
}

typedef struct { uint8_t rk[176]; } aes128_ctx;

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1B)); }

static void aes128_key_expand(aes128_ctx *c, const uint8_t key[16]) {
    memcpy(c->rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, &c->rk[i - 4], 4);
        if (i % 16 == 0) {
            uint8_t t0 = t[0];
            t[0] = SBOX256[t[1]] ^ rcon; t[1] = SBOX256[t[2]]; t[2] = SBOX256[t[3]]; t[3] = SBOX256[t0];
            rcon = xtime(rcon);
        }
        for (int j = 0; j < 4; j++) c->rk[i + j] = c->rk[i - 16 + j] ^ t[j];
    }
}

// State is column-major: s[col*4 + row]
static void aes128_encrypt_block(const aes128_ctx *c, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ c->rk[i];

    for (int r = 1; r <= 10; r++) {
        // SubBytes + ShiftRows
        for (int col = 0; col < 4; col++)
            for (int row = 0; row < 4; row++)
                t[col * 4 + row] = SBOX256[s[((col + row) & 3) * 4 + row]];
        // MixColumns (skipped in the last round)
        if (r < 10) {
            for (int col = 0; col < 4; col++) {
                uint8_t *p = &t[col * 4];
                uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3], all = a0 ^ a1 ^ a2 ^ a3;
                p[0] = a0 ^ all ^ xtime(a0 ^ a1);
                p[1] = a1 ^ all ^ xtime(a1 ^ a2);
                p[2] = a2 ^ all ^ xtime(a2 ^ a3);
                p[3] = a3 ^ all ^ xtime(a3 ^ a0);
            }
        }
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ c->rk[r * 16 + i];
    }
    memcpy(out, s, 16);
}

// x = x * h in GF(2^128), GCM bit ordering
static void gf128_mul(uint8_t x[16], const uint8_t h[16]) {
    uint8_t z[16] = {0}, v[16];
    memcpy(v, h, 16);
    for (int i = 0; i < 128; i++) {
        if ((x[i >> 3] >> (7 - (i & 7))) & 1){for (int j = 0; j < 16; j++) z[j] ^= v[j];}
        int lsb = v[15] & 1;
        for (int j = 15; j > 0; j--) {v[j] = (uint8_t)((v[j] >> 1) | (v[j - 1] << 7));}
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xE1;
    }
    memcpy(x, z, 16);
}

static void ghash_update(uint8_t y[16], const uint8_t h[16], const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i += 16) {
        size_t n = (len - i < 16) ? (len - i) : 16;
        for (size_t j = 0; j < n; j++) y[j] ^= data[i + j];
        gf128_mul(y, h);
    }
}

// CTR keystream starting at counter block IV||0x00000002 (96-bit IV). in == out is fine.
static void gcm_ctr_xor(const aes128_ctx *aes, const uint8_t iv[12], const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t ctr[16], ks[16];
    uint32_t n = 2;
    memcpy(ctr, iv, 12);
    for (size_t i = 0; i < len; i += 16) {
        ctr[12] = (uint8_t)(n >> 24); ctr[13] = (uint8_t)(n >> 16); ctr[14] = (uint8_t)(n >> 8); ctr[15] = (uint8_t)n;
        aes128_encrypt_block(aes, ctr, ks);
        size_t m = (len - i < 16) ? (len - i) : 16;
        for (size_t j = 0; j < m; j++) out[i + j] = in[i + j] ^ ks[j];
        n++;
    }
}

// tag = GHASH(H, A, C) ^ E(K, IV||1)
static void gcm_tag(const aes128_ctx *aes, const uint8_t iv[12],
                    const uint8_t *aad, size_t aad_len,
                    const uint8_t *ct, size_t ct_len, uint8_t tag[16]) {
    uint8_t h[16] = {0}, y[16] = {0}, lens[16], j0[16], ek[16];
    aes128_encrypt_block(aes, h, h);
    ghash_update(y, h, aad, aad_len);
    ghash_update(y, h, ct, ct_len);
    uint64_t alen = (uint64_t)aad_len * 8, clen = (uint64_t)ct_len * 8;
    for (int i = 0; i < 8; i++) {
        lens[i]     = (uint8_t)(alen >> (56 - 8 * i));
        lens[8 + i] = (uint8_t)(clen >> (56 - 8 * i));
    }
    ghash_update(y, h, lens, 16);
    memcpy(j0, iv, 12); j0[12] = j0[13] = j0[14] = 0; j0[15] = 1;
    aes128_encrypt_block(aes, j0, ek);
    for (int i = 0; i < 16; i++) tag[i] = y[i] ^ ek[i];
}

// ==================================================================================================================
// - MARK: TLS 1.3 Record Layer -------------------------------------------------------------------------------------
// ==================================================================================================================

#define REC_MAX (5 + 16384 + 256)   // header + max ciphertext (plaintext 2^14 + 256 expansion)

typedef struct {
    aes128_ctx aes;
    uint8_t    iv[12];
    uint64_t   seq;
} tls_dir;

// traffic_key = Expand-Label(secret, "key", "", 16); traffic_iv = Expand-Label(secret, "iv", "", 12)
static void tls_dir_init(tls_dir *d, const uint8_t secret[32]) {
    uint8_t key[16];
    hkdf_expand_label(secret, 32, "key", NULL, 0, 16, key);
    hkdf_expand_label(secret, 32, "iv",  NULL, 0, 12, d->iv);
    aes128_key_expand(&d->aes, key);
    d->seq = 0;
}

// per-record nonce = iv XOR (64-bit big-endian seq, left-padded to 12 bytes)
static void tls_nonce(const tls_dir *d, uint8_t nonce[12]) {
    memcpy(nonce, d->iv, 12);
    for (int i = 0; i < 8; i++) nonce[11 - i] ^= (uint8_t)(d->seq >> (8 * i));
}

// TLSInnerPlaintext = content || type. Record = 17 03 03 len || AEAD(inner) || tag. Returns record length.
static size_t tls_seal(tls_dir *d, uint8_t inner_type, const uint8_t *pt, size_t pt_len, uint8_t *rec) {
    size_t inner_len = pt_len + 1, ct_len = inner_len + 16;
    uint8_t nonce[12];
    rec[0] = 0x17; rec[1] = 0x03; rec[2] = 0x03;
    rec[3] = (uint8_t)(ct_len >> 8); rec[4] = (uint8_t)ct_len;
    memcpy(rec + 5, pt, pt_len);
    rec[5 + pt_len] = inner_type;
    tls_nonce(d, nonce);
    gcm_ctr_xor(&d->aes, nonce, rec + 5, rec + 5, inner_len);
    gcm_tag(&d->aes, nonce, rec, 5, rec + 5, inner_len, rec + 5 + inner_len);   // AAD = record header
    d->seq++;
    return 5 + ct_len;
}

// Decrypts in place (plaintext at rec+5). Returns inner content type, or -1 on bad tag / malformed.
static int tls_open(tls_dir *d, uint8_t *rec, size_t *pt_len) {
    size_t ct_len = ((size_t)rec[3] << 8) | rec[4];
    if (ct_len < 17) return -1;
    size_t enc_len = ct_len - 16;
    uint8_t nonce[12], tag[16];
    tls_nonce(d, nonce);
    gcm_tag(&d->aes, nonce, rec, 5, rec + 5, enc_len, tag);
    if (!ct_eq(tag, rec + 5 + enc_len, 16)) return -1;
    gcm_ctr_xor(&d->aes, nonce, rec + 5, rec + 5, enc_len);
    d->seq++;
    size_t n = enc_len;                                  // strip zero padding, last non-zero byte = type
    while (n > 0 && rec[5 + n - 1] == 0) n--;
    if (n == 0) return -1;
    *pt_len = n - 1;
    return rec[5 + n - 1];
}

// ==================================================================================================================
// - MARK: TCP EXCHANGES --------------------------------------------------------------------------------------------
// ==================================================================================================================
static void write_u16(uint8_t *buf, size_t *pos, uint16_t val) {
    buf[(*pos)++] = (val >> 8) & 0xFF;
    buf[(*pos)++] = val & 0xFF;
}

static void write_bytes(uint8_t *buf, size_t *pos, const uint8_t *src, size_t len) {
    memcpy(&buf[*pos], src, len);
    *pos += len;
}

// Builds a compliant TLS 1.3 ClientHello record with SNI and X25519 KeyShare
static size_t build_client_hello_google(uint8_t *buf, const char *hostname, const uint8_t client_pub_key[32],
                                        const uint8_t random[32], const uint8_t session_id[32]) {
    size_t pos = 0;

    // 1. Record Header (TLS Plaintext)
    buf[pos++] = 0x16; // Handshake Record Type
    buf[pos++] = 0x03; buf[pos++] = 0x01; // Legacy Record Version (TLS 1.0)
    
    size_t record_len_pos = pos;
    pos += 2; // Placeholder for record payload length

    size_t handshake_start = pos;

    // 2. Handshake Header
    buf[pos++] = 0x01; // Handshake Type: ClientHello
    size_t hs_len_pos = pos;
    pos += 3; // Placeholder for 24-bit handshake length

    size_t hs_body_start = pos;

    // 3. ClientHello Body
    buf[pos++] = 0x03; buf[pos++] = 0x03; // Legacy Client Version: TLS 1.2

    // Client Random (32 Bytes)
    write_bytes(buf, &pos, random, 32);

    // Legacy Session ID (32 Bytes)
    buf[pos++] = 32;
    write_bytes(buf, &pos, session_id, 32);

    // Cipher Suites (1 suite: TLS_AES_128_GCM_SHA256 = 0x1301)
    write_u16(buf, &pos, 2);
    write_u16(buf, &pos, 0x1301);

    // Compression Methods (0x00 null)
    buf[pos++] = 1;
    buf[pos++] = 0;

    // 4. Extensions Section
    size_t ext_len_pos = pos;
    pos += 2;
    size_t ext_start = pos;

    // Extension A: server_name (SNI = 0x0000)
    size_t host_len = strlen(hostname);
    write_u16(buf, &pos, 0x0000);
    write_u16(buf, &pos, (uint16_t)(host_len + 5)); // Ext length
    write_u16(buf, &pos, (uint16_t)(host_len + 3)); // ServerName list length
    buf[pos++] = 0x00;                              // NameType: host_name
    write_u16(buf, &pos, (uint16_t)host_len);       // Hostname length
    write_bytes(buf, &pos, (const uint8_t *)hostname, host_len);

    // Extension B: supported_versions (0x002b) -> TLS 1.3 (0x0304)
    write_u16(buf, &pos, 0x002b);
    write_u16(buf, &pos, 3);
    buf[pos++] = 2;
    write_u16(buf, &pos, 0x0304);

    // Extension C: supported_groups (0x000a) -> X25519 (0x001d)
    write_u16(buf, &pos, 0x000a);
    write_u16(buf, &pos, 4); // Ext length
    write_u16(buf, &pos, 2); // Group list length
    write_u16(buf, &pos, 0x001d);

    // Extension D: signature_algorithms (0x000d)
    write_u16(buf, &pos, 0x000d);
    write_u16(buf, &pos, 8); // Ext length
    write_u16(buf, &pos, 6); // SigAlg list length
    write_u16(buf, &pos, 0x0403); // ecdsa_secp256r1_sha256
    write_u16(buf, &pos, 0x0804); // rsa_pss_rsae_sha256
    write_u16(buf, &pos, 0x0401); // rsa_pkcs1_sha256

    // Extension E: key_share (0x0033) -> X25519 Public Key
    write_u16(buf, &pos, 0x0033);
    write_u16(buf, &pos, 38);   // Ext length
    write_u16(buf, &pos, 36);   // Client key shares vector length
    write_u16(buf, &pos, 0x001d); // Named Group: x25519
    write_u16(buf, &pos, 32);   // Key exchange length
    write_bytes(buf, &pos, client_pub_key, 32);

    // Backfill Header Lengths
    uint16_t total_ext_len = (uint16_t)(pos - ext_start);
    buf[ext_len_pos]     = (total_ext_len >> 8) & 0xFF;
    buf[ext_len_pos + 1] = total_ext_len & 0xFF;

    uint32_t total_hs_len = (uint32_t)(pos - hs_body_start);
    buf[hs_len_pos]     = (total_hs_len >> 16) & 0xFF;
    buf[hs_len_pos + 1] = (total_hs_len >> 8) & 0xFF;
    buf[hs_len_pos + 2] = total_hs_len & 0xFF;

    uint16_t total_record_len = (uint16_t)(pos - handshake_start);
    buf[record_len_pos]     = (total_record_len >> 8) & 0xFF;
    buf[record_len_pos + 1] = total_record_len & 0xFF;

    return pos;
}

// Parses ServerHello record and isolates the X25519 KeyShare
static int parse_server_hello_key_share(const uint8_t *buf, size_t buf_len,uint8_t server_pub_key[32],size_t *sh_handshake_len) {
    if (buf_len < 5) {return -1;}
    if (buf[0] != 0x16){return -2;} // Not Handshake record
    size_t pos = 5; if (buf[pos] != 0x02) {return -3;} // Not ServerHello
    // Parse Handshake Payload Length
    uint32_t hs_len = ((uint32_t)buf[pos + 1] << 16) | ((uint32_t)buf[pos + 2] << 8) | buf[pos + 3];
    *sh_handshake_len = hs_len + 4; // Include 4-byte handshake header
    pos += 38; // Skip Handshake header (4 bytes) and legacy_version (2 bytes) and random (32 bytes)
    uint8_t sess_id_len = buf[pos++];
    pos += sess_id_len;
    pos += 3; // Skip cipher_suite (2 bytes) and compression_method (1 byte)
    if (pos + 2 > buf_len){ return -4;}
    uint16_t ext_total_len = (buf[pos] << 8) | buf[pos + 1];
    pos += 2;
    size_t ext_end = pos + ext_total_len;
    while (pos + 4 <= ext_end && pos + 4 <= buf_len) {
        uint16_t ext_type = (buf[pos] << 8) | buf[pos + 1];
        uint16_t ext_len  = (buf[pos + 2] << 8) | buf[pos + 3];
        pos += 4;
        if (ext_type == 0x0033) { // key_share extension
            if (ext_len < 36) return -5;
            uint16_t group = (buf[pos] << 8) | buf[pos + 1];
            uint16_t klen  = (buf[pos + 2] << 8) | buf[pos + 3];
            if (group == 0x001d && klen == 32) {
                memcpy(server_pub_key, &buf[pos + 4], 32);
                return 0; // Success
            }
        }
        pos += ext_len;
    }
    return -6;
}

// DNS Resolution and Socket Creation for Hostname:Port
static int connect_to_host(const char *hostname, const char *port) {
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP

    if (getaddrinfo(hostname, port, &hints, &res) != 0) return -1;

    int sockfd = -1;
    for (p = res; p != NULL; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd < 0) continue;
        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(sockfd);
        sockfd = -1;
    }
    freeaddrinfo(res);
    return sockfd;
}

// ==================================================================================================================
// - MARK: Record I/O -----------------------------------------------------------------------------------------------
// ==================================================================================================================

static int send_all(int fd, const uint8_t *buf, size_t n) {
    while (n > 0) {
        ssize_t s = send(fd, buf, n, 0);
        if (s <= 0) return -1;
        buf += s; n -= (size_t)s;
    }
    return 0;
}

static int recv_all(int fd, uint8_t *buf, size_t n) {
    while (n > 0) {
        ssize_t r = recv(fd, buf, n, 0);
        if (r <= 0) return -1;
        buf += r; n -= (size_t)r;
    }
    return 0;
}

// Reads exactly one TLS record (5-byte header + body) into rec. Returns body length, or -1.
static int read_record(int fd, uint8_t *rec, size_t cap) {
    if (recv_all(fd, rec, 5) < 0) return -1;
    size_t len = ((size_t)rec[3] << 8) | rec[4];
    if (len > cap - 5) return -1;
    if (recv_all(fd, rec + 5, len) < 0) return -1;
    return (int)len;
}

static const char *hs_name(uint8_t t) {
    switch (t) {
        case 8:  return "EncryptedExtensions";
        case 11: return "Certificate";
        case 13: return "CertificateRequest";
        case 15: return "CertificateVerify";
        case 20: return "Finished";
        default: return "Unknown";
    }
}

#define FAIL(...) do { fprintf(stderr, "ERROR: " __VA_ARGS__); fprintf(stderr, "\n"); if (fd >= 0) close(fd); return EXIT_FAILURE; } while (0)

int main() {
    printf("====================================================================\n");
    printf("       Standalone TLS 1.3 Zero-Dependency Client Architecture       \n");
    printf("                             C LANGUAGE                             \n");
    printf("                      JOEL PACHERIE - SEPT 2026                     \n");
    printf("====================================================================\n\n");
    
    // STEP 0: Request config:
    const char *target_host = "www.joelpacherie.com";
    const char *target_port = "443"; // https
    int fd = -1;

    aes_init_sbox();

    // STEP 1: Generate Client X25519 Ephemeral Keypair (+ ClientHello random / legacy session id, still here to padding)
    uint8_t client_priv[32], ch_random[32], ch_sess_id[32];
    get_cpu_jitter_entropy_256(client_priv);
    get_cpu_jitter_entropy_256(ch_random);
    get_cpu_jitter_entropy_256(ch_sess_id);
    uint8_t base_point[32] = {9};
    uint8_t client_pub[32];
    x25519(client_pub, client_priv, base_point);
    print_hex("Client X25519 Public Key", client_pub, 32);
    
    // 2. Construct ClientHello Frame
    uint8_t ch_buf[512];
    size_t ch_len = build_client_hello_google(ch_buf, target_host, client_pub, ch_random, ch_sess_id);
    printf("ClientHello Wire Size        : %zu Bytes\n", ch_len);

    // 3. Initialize Handshake Transcript Hash (running; snapshot by struct copy)
    sha256_ctx transcript_ctx;
    sha256_init(&transcript_ctx);
    sha256_update(&transcript_ctx, ch_buf + 5, ch_len - 5);   // handshake message only, no record header

    // 4. DNS Query & TCP Connect
    printf("Connecting to %s:%s via POSIX TCP...\n", target_host, target_port);
    fd = connect_to_host(target_host, target_port);
    if (fd < 0) FAIL("Socket connection failed to %s:%s", target_host, target_port);
    printf("TCP Socket Connected        : FD %d\n", fd);

    // 5. Send ClientHello
    if (send_all(fd, ch_buf, ch_len) < 0) FAIL("send ClientHello");
    printf("Sent ClientHello Frame      : %zu Bytes\n", ch_len);

    // 6. Receive ServerHello record
    static uint8_t rec[REC_MAX];
    int n = read_record(fd, rec, sizeof(rec));
    if (n < 0) FAIL("no ServerHello record");
    if (rec[0] == 0x15) FAIL("server alert: level %u desc %u", rec[5], rec[6]);
    printf("Received ServerHello Record : %d Bytes\n", n + 5);

    // 7. Parse ServerHello & extract KeyShare
    uint8_t server_pub[32];
    size_t sh_hs_len = 0;
    int res = parse_server_hello_key_share(rec, (size_t)n + 5, server_pub, &sh_hs_len);
    if (res != 0) FAIL("Failed to parse ServerHello KeyShare (Code: %d)", res);
    if (sh_hs_len > (size_t)n) FAIL("ServerHello length");
    print_hex("Server X25519 Public Key", server_pub, 32);
    sha256_update(&transcript_ctx, rec + 5, sh_hs_len);

    // 8. X25519 Diffie-Hellman
    uint8_t shared_secret_Z[32];
    x25519(shared_secret_Z, client_priv, server_pub);
    print_hex("Derived Shared Secret Z", shared_secret_Z, 32);

    // 9. Transcript(CH..SH) -> handshake traffic secrets -> record keys
    uint8_t transcript_hash[32];
    sha256_ctx snap = transcript_ctx;               // sha256_final destroys the ctx, so hash a copy
    sha256_final(&snap, transcript_hash);
    print_hex("Handshake Transcript Hash H", transcript_hash, 32);

    uint8_t hs_secret[32], c_hs_traffic[32], s_hs_traffic[32];
    tls13_derive_handshake_keys(shared_secret_Z, transcript_hash, hs_secret, c_hs_traffic, s_hs_traffic);
    print_hex("Client HS Traffic Secret", c_hs_traffic, 32);
    print_hex("Server HS Traffic Secret", s_hs_traffic, 32);

    tls_dir cli_hs, srv_hs;
    tls_dir_init(&cli_hs, c_hs_traffic);
    tls_dir_init(&srv_hs, s_hs_traffic);

    // 10. Encrypted server flight: EncryptedExtensions, Certificate, CertificateVerify, Finished.
    //     Handshake messages may be coalesced in one record or split across several -> reassembly buffer.
    static uint8_t hs_buf[65536];
    size_t hs_len = 0;
    int got_finished = 0;

    printf("\n=== Server Handshake Flight ===\n");
    while (!got_finished) {
        n = read_record(fd, rec, sizeof(rec));
        if (n < 0) FAIL("connection closed during handshake");
        if (rec[0] == 0x14) continue;                                   // middlebox-compat ChangeCipherSpec
        if (rec[0] == 0x15) FAIL("server alert: level %u desc %u", rec[5], rec[6]);
        if (rec[0] != 0x17) FAIL("unexpected record type 0x%02x", rec[0]);

        size_t pt_len = 0;
        int inner = tls_open(&srv_hs, rec, &pt_len);
        if (inner < 0) FAIL("record decrypt failed (bad tag)");
        if (inner == 0x15) FAIL("server alert: level %u desc %u", rec[5], rec[6]);
        if (inner != 0x16) FAIL("unexpected inner type 0x%02x during handshake", inner);
        if (hs_len + pt_len > sizeof(hs_buf)) FAIL("handshake flight too large");
        memcpy(hs_buf + hs_len, rec + 5, pt_len);
        hs_len += pt_len;

        while (hs_len >= 4 && !got_finished) {
            uint8_t  mtype = hs_buf[0];
            uint32_t mlen  = ((uint32_t)hs_buf[1] << 16) | ((uint32_t)hs_buf[2] << 8) | hs_buf[3];
            if (hs_len < 4 + (size_t)mlen) break;                       // wait for the rest

            printf("  <- %-20s (%u bytes)\n", hs_name(mtype), mlen);
            if (mtype == 13) FAIL("CertificateRequest (client auth) not supported");

            if (mtype == 20) {
                // Server Finished MAC covers CH..CertificateVerify == transcript so far
                uint8_t h_cv[32], expect[32];
                sha256_ctx s = transcript_ctx;
                sha256_final(&s, h_cv);
                tls13_finished_verify_data(s_hs_traffic, h_cv, expect);
                if (mlen != 32 || !ct_eq(expect, hs_buf + 4, 32)) FAIL("server Finished MAC mismatch");
                printf("     server Finished MAC verified\n");
                got_finished = 1;
            }
            // TODO: mtype 11 = parse + validate cert chain, mtype 15 = verify signature (see notes) !!!

            sha256_update(&transcript_ctx, hs_buf, 4 + (size_t)mlen);
            hs_len -= 4 + (size_t)mlen;
            memmove(hs_buf, hs_buf + 4 + mlen, hs_len);
        }
    }

    // 11. Transcript(CH..server Finished) -> application secrets
    uint8_t h_sf[32];
    snap = transcript_ctx;
    sha256_final(&snap, h_sf);
    print_hex("Transcript Hash (..SF)  ", h_sf, 32);

    uint8_t c_ap[32], s_ap[32];
    tls13_derive_app_secrets(hs_secret, h_sf, c_ap, s_ap);
    print_hex("Client AP Traffic Secret", c_ap, 32);
    print_hex("Server AP Traffic Secret", s_ap, 32);

    // 12. Client flight: [ChangeCipherSpec compat] + Finished under client handshake keys
    static const uint8_t ccs[6] = {0x14, 0x03, 0x03, 0x00, 0x01, 0x01};
    uint8_t cfin[4 + 32] = {20, 0, 0, 32};
    tls13_finished_verify_data(c_hs_traffic, h_sf, cfin + 4);
    size_t rn = tls_seal(&cli_hs, 0x16, cfin, sizeof(cfin), rec);
    if (send_all(fd, ccs, sizeof(ccs)) < 0 || send_all(fd, rec, rn) < 0) FAIL("send client Finished");
    printf("  -> Finished              (client)\n");

    // 13. Switch to application keys (sequence numbers restart at 0)
    tls_dir cli_ap, srv_ap;
    tls_dir_init(&cli_ap, c_ap);
    tls_dir_init(&srv_ap, s_ap);
    printf("\n=== TLS 1.3 handshake complete (TLS_AES_128_GCM_SHA256) ===\n\n");

    // 14. HTTP over TLS
    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "GET / HTTP/1.1\r\nHost: %s\r\nUser-Agent: tls13-from-scratch\r\nAccept: */*\r\nConnection: close\r\n\r\n",
        target_host);
    rn = tls_seal(&cli_ap, 0x17, (const uint8_t *)req, (size_t)req_len, rec);
    if (send_all(fd, rec, rn) < 0) FAIL("send HTTP request");
    printf(">>> %.*s", req_len, req);

    // 15. Read application data until close_notify / EOF
    printf("<<<\n");
    for (;;) {
        n = read_record(fd, rec, sizeof(rec));
        if (n < 0) break;
        if (rec[0] == 0x14) continue;
        if (rec[0] != 0x17) break;
        size_t pt_len = 0;
        int inner = tls_open(&srv_ap, rec, &pt_len);
        if (inner < 0) FAIL("application record decrypt failed");
        if (inner == 0x17) fwrite(rec + 5, 1, pt_len, stdout);
        else if (inner == 0x15) { printf("\n[alert level %u desc %u]\n", rec[5], rec[6]); break; }
        // inner == 0x16: NewSessionTicket / KeyUpdate -> ignored
    }
    printf("\n");
    close(fd);
    printf("OK -> Exit\n");
    return EXIT_SUCCESS;
}
