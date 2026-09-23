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

// ==================================================================================================================
// - MARK: TOOLS ----------------------------------------------------------------------------------------------------
// ==================================================================================================================
static void print_hex(const char *title, const uint8_t *buf, size_t len) {
    printf("%s: ", title);
    for (size_t i = 0; i < len; i++) printf("%02x", buf[i]);
    printf("\n");
}

// ==================================================================================================================
// - MARK: Entropy --------------------------------------------------------------------------------------------------
// ==================================================================================================================

uint64_t get_cpu_jitter_entropy_64(void) {
    uint64_t entropy = 0;
    uint64_t now = 0, prev = 0;

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
    for (uint8_t i = 0; i < 64; i++) {
        // Induce execution time jitter via a data-dependent, unpredictable loop.
        // 'volatile' ensures GCC's optimizer does not strip this out. */
        volatile uint32_t noise_accumulator = 0;
        
        // Loop between 1 and 64 times based on the lowest bits of the previous timestamp.
        // This causes deliberate branch prediction variations and pipeline jitter.
        uint32_t iterations = (prev & 0x3F) + 1;
        for (uint32_t j = 0; j < iterations; j++) {noise_accumulator ^= j;}

        // 3. Read timer again */
        #if defined(__x86_64__) || defined(__i386__)
            __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
            now = ((uint64_t)hi << 32) | lo;
        #elif defined(__aarch64__)
            __asm__ __volatile__ ("mrs %0, cntvct_el0" : "=r" (now));
        #endif

        // 4. Calculate execution time delta */
        uint64_t delta = now - prev;

        // 5. Stir the delta into the accumulator.
        // We rotate the current entropy left by 1 to spread bits,
        // then XOR the delta (where the LSBs contain the most unpredictable noise).
        entropy = (entropy << 1) | (entropy >> 63);
        entropy ^= (delta + noise_accumulator); // Include noise to ensure compiler keeps it
        prev = now;
    }
    
    // The Avalanche step (MurmurHash3 64-bit finalizer).
    // Forces a uniform distribution by violently mixing the bits.
    //Latency: ~3-5 CPU cycles.
    entropy ^= entropy >> 33;
    entropy *= 0xff51afd7ed558ccdULL;
    entropy ^= entropy >> 33;
    entropy *= 0xc4ceb9fe1a85ec53ULL;
    entropy ^= entropy >> 33;
        
        
    return entropy;
}

void get_cpu_jitter_entropy_256(uint8_t * __restrict__ dst) {
    uint64_t state[4];
    uint64_t prev, now, delta;

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
    for (uint8_t i = 0; i < 4; i++) {
        uint64_t entropy64 = 0;

        #pragma GCC unroll 64
        for (uint8_t j = 0; j < 64; j++) {
            
            // Branch from 1 to 64 times
            uint32_t iterations = (prev & 0x3F) + 1;

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
    uint64_t a = state[0];
    uint64_t b = state[1];
    uint64_t c = state[2];
    uint64_t d = state[3];

    // Out-of-order execution engines will blast through these 12 operations
    // across 4 independent dependency chains concurrently.
    a ^= a >> 33; a *= 0xff51afd7ed558ccdULL; a ^= a >> 33; a *= 0xc4ceb9fe1a85ec53ULL; a ^= a >> 33;
    b ^= b >> 33; b *= 0xff51afd7ed558ccdULL; b ^= b >> 33; b *= 0xc4ceb9fe1a85ec53ULL; b ^= b >> 33;
    c ^= c >> 33; c *= 0xff51afd7ed558ccdULL; c ^= c >> 33; c *= 0xc4ceb9fe1a85ec53ULL; c ^= c >> 33;
    d ^= d >> 33; d *= 0xff51afd7ed558ccdULL; d ^= d >> 33; d *= 0xc4ceb9fe1a85ec53ULL; d ^= d >> 33;

    // 7. Write once at the end
    uint64_t *out = (uint64_t*)dst;
    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = d;
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

// MONTGOMERY LADDER

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
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    for (int i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) | ((uint32_t)data[j + 2] << 8) | ((uint32_t)data[j + 3]);
    for (int i = 16; i < 64; ++i)
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (int i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e, f, g) + K256[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha256_init(sha256_ctx *ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen] = data[i];
        ctx->datalen++;
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

void sha256_final(sha256_ctx *ctx, uint8_t hash[32]) {
    uint32_t i = ctx->datalen;

    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0x00;
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }

    ctx->bitlen += ctx->datalen * 8;
    ctx->data[56] = (ctx->bitlen >> 56) & 0xFF;
    ctx->data[57] = (ctx->bitlen >> 48) & 0xFF;
    ctx->data[58] = (ctx->bitlen >> 40) & 0xFF;
    ctx->data[59] = (ctx->bitlen >> 32) & 0xFF;
    ctx->data[60] = (ctx->bitlen >> 24) & 0xFF;
    ctx->data[61] = (ctx->bitlen >> 16) & 0xFF;
    ctx->data[62] = (ctx->bitlen >> 8) & 0xFF;
    ctx->data[63] = ctx->bitlen & 0xFF;
    sha256_transform(ctx, ctx->data);

    for (i = 0; i < 4; ++i) {
        hash[i]      = (ctx->state[0] >> (24 - i * 8)) & 0xFF;
        hash[i + 4]  = (ctx->state[1] >> (24 - i * 8)) & 0xFF;
        hash[i + 8]  = (ctx->state[2] >> (24 - i * 8)) & 0xFF;
        hash[i + 12] = (ctx->state[3] >> (24 - i * 8)) & 0xFF;
        hash[i + 16] = (ctx->state[4] >> (24 - i * 8)) & 0xFF;
        hash[i + 20] = (ctx->state[5] >> (24 - i * 8)) & 0xFF;
        hash[i + 24] = (ctx->state[6] >> (24 - i * 8)) & 0xFF;
        hash[i + 28] = (ctx->state[7] >> (24 - i * 8)) & 0xFF;
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

void tls13_derive_handshake_keys(const uint8_t ecdh_shared_secret[32],
                                 const uint8_t transcript_hash[32],
                                 uint8_t client_hs_traffic_secret[32],
                                 uint8_t server_hs_traffic_secret[32],
                                 uint8_t client_key[16], uint8_t client_iv[12]) {
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
    uint8_t handshake_secret[32];
    hkdf_extract(derived_secret, 32, ecdh_shared_secret, 32, handshake_secret);

    // 4. Client & Server Handshake Traffic Secrets
    derive_secret(handshake_secret, 32, "c hs traffic", transcript_hash, 32, client_hs_traffic_secret);
    derive_secret(handshake_secret, 32, "s hs traffic", transcript_hash, 32, server_hs_traffic_secret);

    // 5. Expand Traffic Key & IV for Record Encryption (e.g. AES-128-GCM)
    hkdf_expand_label(client_hs_traffic_secret, 32, "key", NULL, 0, 16, client_key);
    hkdf_expand_label(client_hs_traffic_secret, 32, "iv",  NULL, 0, 12, client_iv);
}


// ==================================================================================================================
// - MARK: TCP EXCHANGES --------------------------------------------------------------------------------------------
// ==================================================================================================================
static void write_u16(uint8_t *buf, size_t *pos, uint16_t val) {
    buf[(*pos)++] = (val >> 8) & 0xFF;
    buf[(*pos)++] = val & 0xFF;
}

static void write_u24(uint8_t *buf, size_t *pos, uint32_t val) {
    buf[(*pos)++] = (val >> 16) & 0xFF;
    buf[(*pos)++] = (val >> 8) & 0xFF;
    buf[(*pos)++] = val & 0xFF;
}

static void write_bytes(uint8_t *buf, size_t *pos, const uint8_t *src, size_t len) {
    memcpy(&buf[*pos], src, len);
    *pos += len;
}

// Builds a compliant TLS 1.3 ClientHello record with SNI (google.com) and X25519 KeyShare
static size_t build_client_hello_google(uint8_t *buf, const char *hostname, const uint8_t client_pub_key[32]) {
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
    for (int i = 0; i < 32; i++) buf[pos++] = (uint8_t)(i + 0x10);

    // Legacy Session ID (32 Bytes)
    buf[pos++] = 32;
    for (int i = 0; i < 32; i++) buf[pos++] = 0x55;

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

// Parses Google's ServerHello response frame and isolates the X25519 KeyShare
static int parse_server_hello_key_share(const uint8_t *buf, size_t buf_len,
                                        uint8_t server_pub_key[32],
                                        size_t *sh_handshake_len) {
    if (buf_len < 5) return -1;
    if (buf[0] != 0x16) return -2; // Not Handshake record

    size_t pos = 5;
    if (buf[pos] != 0x02) return -3; // Not ServerHello

    // Parse Handshake Payload Length
    uint32_t hs_len = ((uint32_t)buf[pos + 1] << 16) | ((uint32_t)buf[pos + 2] << 8) | buf[pos + 3];
    *sh_handshake_len = hs_len + 4; // Include 4-byte handshake header

    pos += 4; // Skip Handshake header
    pos += 2; // Skip legacy_version
    pos += 32; // Skip random

    uint8_t sess_id_len = buf[pos++];
    pos += sess_id_len;

    pos += 2; // Skip cipher_suite
    pos += 1; // Skip compression_method

    if (pos + 2 > buf_len) return -4;
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

// POSIX TCP Client Connection Helper
static int connect_tcp(const char *ip, uint16_t port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return -1;

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &serv_addr.sin_addr) <= 0) {
        close(sockfd);
        return -1;
    }

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sockfd);
        return -1;
    }
    return sockfd;
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

int main(int argc, const char * argv[]) {
    printf("====================================================================\n");
    printf("       Standalone TLS 1.3 Zero-Dependency Client Architecture       \n");
    printf("                             C LANGUAGE                             \n");
    printf("                        JOEL PACHERIE - 2026                        \n");
    printf("====================================================================\n\n");
    
    // STEP 0: Request config:
    const char *target_host = "google.com"; // will be dynamic later on..
    const char *target_port = "443"; // https
    
    
    // STEP 1: Generate Client X25519 Ephemeral Keypair
    uint8_t client_priv[32] = { // hardcoded for debug.. must be replaced by entropy 256
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
    };
    uint8_t base_point[32] = {9};
    uint8_t client_pub[32];
    x25519(client_pub, client_priv, base_point);
    print_hex("Client X25519 Public Key", client_pub, 32);
    
    // 2. Construct ClientHello Frame with Google SNI
    uint8_t ch_buf[512];
    size_t ch_len = build_client_hello_google(ch_buf, target_host, client_pub);
    printf("ClientHello Wire Size        : %zu Bytes\n", ch_len);

    // 3. Initialize Handshake Transcript Hash
    sha256_ctx transcript_ctx;
    sha256_init(&transcript_ctx);
    // Hash the ClientHello Handshake payload (exclude 5-byte Record Header)
    sha256_update(&transcript_ctx, ch_buf + 5, ch_len - 5);

    // 4. DNS Query & TCP Connect
    printf("Connecting to %s:%s via POSIX TCP...\n", target_host, target_port);
    int fd = connect_to_host(target_host, target_port);
    if (fd < 0) {
        fprintf(stderr, "ERROR: Socket connection failed to %s:%s\n", target_host, target_port);
        return EXIT_FAILURE;
    }
    printf("TCP Socket Connected        : FD %d\n", fd);

    // 5. Send ClientHello over Wire
    ssize_t sent = send(fd, ch_buf, ch_len, 0);
    printf("Sent ClientHello Frame      : %zd Bytes\n", sent);

    // 6. Receive Google's Live TLS Response
    uint8_t rx_buf[4096];
    ssize_t recvd = recv(fd, rx_buf, sizeof(rx_buf), 0);
    close(fd);

    if (recvd <= 0) {
        fprintf(stderr, "ERROR: Failed to receive response from Google.\n");
        return EXIT_FAILURE;
    }
    printf("Received Server TLS Stream   : %zd Bytes\n", recvd);

    // 7. Parse Google's ServerHello & Extract KeyShare
    uint8_t google_pub[32];
    size_t sh_hs_len = 0;
    int res = parse_server_hello_key_share(rx_buf, (size_t)recvd, google_pub, &sh_hs_len);

    if (res != 0) {
        fprintf(stderr, "ERROR: Failed to parse ServerHello KeyShare (Code: %d)\n", res);
        return EXIT_FAILURE;
    }

    print_hex("Google X25519 Public Key", google_pub, 32);

    // Feed exact ServerHello Handshake Payload into Transcript Hash
    sha256_update(&transcript_ctx, rx_buf + 5, sh_hs_len);

    // 8. Execute X25519 Diffie-Hellman Key Agreement against Google's Key
    uint8_t shared_secret_Z[32];
    x25519(shared_secret_Z, client_priv, google_pub);
    print_hex("Derived Shared Secret Z", shared_secret_Z, 32);

    // 9. Finalize Transcript Hash & Derive Live HKDF Traffic Keys
    uint8_t transcript_hash[32];
    sha256_final(&transcript_ctx, transcript_hash);
    print_hex("Handshake Transcript Hash H", transcript_hash, 32);

    uint8_t c_hs_traffic[32], s_hs_traffic[32];
    uint8_t client_key[16], client_iv[12];
    tls13_derive_handshake_keys(shared_secret_Z, transcript_hash,
                                c_hs_traffic, s_hs_traffic,
                                client_key, client_iv);

    printf("\n=== Live HKDF Key Schedule Outputs (Google TLS 1.3) ===\n");
    print_hex("Client HS Traffic Secret", c_hs_traffic, 32);
    print_hex("Server HS Traffic Secret", s_hs_traffic, 32);
    print_hex("Client Record AES Key   ", client_key, 16);
    print_hex("Client Record IV        ", client_iv, 12);
    
    printf("OK -> Exit\n");
    return EXIT_SUCCESS;
}
