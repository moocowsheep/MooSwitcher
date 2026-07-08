#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

// Shared test-pattern layout for moo-testgen / moo-latmeter.
//
// UYVY frame layout (all regions sized in even pixels):
//   row 0 (y in [0,16)):   64 data blocks + 8 parity blocks = frame counter
//   row 1 (y in [16,32)):  64 data blocks + 8 parity blocks = send wallclock
//                          (CLOCK_REALTIME ns, stamped just before send)
//   flash (y in [32,48), x in [0,256)): white on sync frames, else black
//   bars/moving bar below y=64.
// Blocks are 16x16 px, MSB first, Y=235 for 1, Y=16 for 0, chroma neutral.
// Strips need 72*16 = 1152 px of width: every target >= 1280 wide works.
namespace moo::pattern {

constexpr int kBlock = 16;
constexpr int kDataBlocks = 64;
constexpr int kParityBlocks = 8;
constexpr int kStripWidth = (kDataBlocks + kParityBlocks) * kBlock;  // 1152
constexpr int kCounterRow = 0;
constexpr int kTimeRow = 1;
constexpr int kFlashY = 32;
constexpr int kFlashH = 16;
constexpr int kFlashW = 256;
constexpr int kPatternTop = 64;      // everything below is bars + moving bar
constexpr int kFlashPeriodTicks = 60;
constexpr int kToneHz = 1000;
constexpr int kToneBurstSamples = 4800;  // 100 ms @ 48 kHz
constexpr int kSampleRate = 48000;

inline void fillRectUYVY(uint8_t* buf, int strideBytes, int x0, int y0, int w,
                         int h, uint8_t Y, uint8_t U, uint8_t V) {
    // x0 and w must be even (callers use 16px-aligned regions).
    for (int y = y0; y < y0 + h; ++y) {
        uint8_t* p = buf + size_t(y) * strideBytes + size_t(x0 / 2) * 4;
        for (int x = 0; x < w / 2; ++x) {
            p[0] = U;
            p[1] = Y;
            p[2] = V;
            p[3] = Y;
            p += 4;
        }
    }
}

inline uint8_t lumaAt(const uint8_t* buf, int strideBytes, int x, int y) {
    return buf[size_t(y) * strideBytes + size_t(x / 2) * 4 + 1 + size_t(x & 1) * 2];
}

inline uint8_t parity8(uint64_t v) {
    uint8_t p = 0;
    for (int i = 0; i < 8; ++i) p ^= uint8_t(v >> (i * 8));
    return p;
}

inline void stampStrip(uint8_t* buf, int strideBytes, int row, uint64_t value) {
    const int y0 = row * kBlock;
    for (int i = 0; i < kDataBlocks; ++i) {
        const bool bit = (value >> (kDataBlocks - 1 - i)) & 1;
        fillRectUYVY(buf, strideBytes, i * kBlock, y0, kBlock, kBlock,
                     bit ? 235 : 16, 128, 128);
    }
    const uint8_t p = parity8(value);
    for (int i = 0; i < kParityBlocks; ++i) {
        const bool bit = (p >> (kParityBlocks - 1 - i)) & 1;
        fillRectUYVY(buf, strideBytes, (kDataBlocks + i) * kBlock, y0, kBlock,
                     kBlock, bit ? 235 : 16, 128, 128);
    }
}

inline bool readStrip(const uint8_t* buf, int strideBytes, int row, uint64_t& out) {
    const int yc = row * kBlock + kBlock / 2;
    uint64_t v = 0;
    for (int i = 0; i < kDataBlocks; ++i)
        v = (v << 1) | (lumaAt(buf, strideBytes, i * kBlock + kBlock / 2, yc) > 127);
    uint8_t p = 0;
    for (int i = 0; i < kParityBlocks; ++i)
        p = uint8_t((p << 1) |
                    (lumaAt(buf, strideBytes, (kDataBlocks + i) * kBlock + kBlock / 2, yc) > 127));
    out = v;
    return p == parity8(v);
}

inline void stampFlash(uint8_t* buf, int strideBytes, bool on) {
    fillRectUYVY(buf, strideBytes, 0, kFlashY, kFlashW, kFlashH, on ? 235 : 16,
                 128, 128);
}

inline bool readFlash(const uint8_t* buf, int strideBytes) {
    int sum = 0, count = 0;
    for (int x = 8; x < kFlashW; x += 32) {
        sum += lumaAt(buf, strideBytes, x, kFlashY + kFlashH / 2);
        ++count;
    }
    return sum / count > 127;
}

// Tally display row (y in [48,64)): red block when on-program, green block
// when on-preview -- lets a receiver (or a human) verify tally end-to-end.
constexpr int kTallyY = 48;
constexpr int kTallyH = 16;
constexpr int kTallyW = 128;

inline void stampTally(uint8_t* buf, int strideBytes, bool pgm, bool pvw) {
    if (pgm)  // 100% red, BT.709
        fillRectUYVY(buf, strideBytes, 0, kTallyY, kTallyW, kTallyH, 63, 102, 240);
    else
        fillRectUYVY(buf, strideBytes, 0, kTallyY, kTallyW, kTallyH, 16, 128, 128);
    if (pvw)  // 100% green, BT.709
        fillRectUYVY(buf, strideBytes, kTallyW, kTallyY, kTallyW, kTallyH, 173, 42, 26);
    else
        fillRectUYVY(buf, strideBytes, kTallyW, kTallyY, kTallyW, kTallyH, 16, 128,
                     128);
}

// BT.709 75% color bars (8 columns) below kPatternTop.
inline void fillBars(uint8_t* buf, int strideBytes, int W, int H) {
    struct Yuv { uint8_t y, u, v; };
    static constexpr Yuv bars[8] = {
        {180, 128, 128}, {168, 44, 136}, {145, 147, 44}, {133, 63, 52},
        {63, 193, 204},  {51, 109, 212}, {28, 212, 120}, {16, 128, 128},
    };
    const int colW = (W / 8) & ~1;
    for (int i = 0; i < 8; ++i) {
        const int x0 = i * colW;
        const int w = (i == 7) ? W - x0 : colW;
        fillRectUYVY(buf, strideBytes, x0, kPatternTop, w, H - kPatternTop,
                     bars[i].y, bars[i].u, bars[i].v);
    }
    // Header background between strips and pattern.
    fillRectUYVY(buf, strideBytes, 0, kFlashY + kFlashH, W,
                 kPatternTop - kFlashY - kFlashH, 16, 128, 128);
    if (W > kStripWidth) {
        fillRectUYVY(buf, strideBytes, kStripWidth, 0, W - kStripWidth,
                     kFlashY + kFlashH, 16, 128, 128);
        // (flash region spans only kFlashW; clear the rest of its row too)
    }
    if (W > kFlashW)
        fillRectUYVY(buf, strideBytes, kFlashW, kFlashY, W - kFlashW, kFlashH,
                     16, 128, 128);
}

// Video-range noise over the pattern region: defeats DCT + entropy coding,
// giving worst-case (representative-upper-bound) codec load -- bars compress
// ~1000x under SpeedHQ and make codec benches meaningless. Seed per slot so
// a precomputed ring plays as temporal chaos.
inline void fillNoise(uint8_t* buf, int strideBytes, int W, int H,
                      uint32_t seed) {
    uint32_t s = seed * 2654435761u + 12345u;
    auto rnd = [&] {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    };
    for (int y = kPatternTop; y < H; ++y) {
        uint8_t* p = buf + size_t(y) * strideBytes;
        for (int x = 0; x < W * 2; x += 4) {
            const uint32_t r = rnd();
            p[x + 0] = uint8_t(16 + (r & 0xFF) % 209);
            p[x + 1] = uint8_t(16 + ((r >> 8) & 0xFF) % 220);
            p[x + 2] = uint8_t(16 + ((r >> 16) & 0xFF) % 209);
            p[x + 3] = uint8_t(16 + ((r >> 24) & 0xFF) % 220);
        }
    }
}

// Mid-entropy content: per-slot random flat 8x8 blocks -- spatially busy but
// DCT-compressible, temporally chaotic across the precomputed ring. A stand-in
// for detailed real-world content between bars (~1000x compressible) and
// full noise (incompressible).
inline void fillMid(uint8_t* buf, int strideBytes, int W, int H,
                    uint32_t seed) {
    uint32_t s = seed * 2246822519u + 54321u;
    auto rnd = [&] {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    };
    for (int y = kPatternTop; y < H; y += 8) {
        const int bh = std::min(8, H - y);
        for (int x = 0; x < W; x += 8) {
            const uint32_t r = rnd();
            const uint8_t Y = uint8_t(16 + (r & 0xFF) % 220);
            const uint8_t U = uint8_t(16 + ((r >> 8) & 0xFF) % 209);
            const uint8_t V = uint8_t(16 + ((r >> 16) & 0xFF) % 209);
            fillRectUYVY(buf, strideBytes, x, y, std::min(8, W - x), bh, Y, U,
                         V);
        }
    }
}

// Moving bar: white vertical bar over the bars region; position by slot so a
// precomputed ring of K frames loops seamlessly.
inline void bakeMovingBar(uint8_t* buf, int strideBytes, int W, int H, int slot,
                          int slotCount) {
    constexpr int kBarW = 32;
    const int travel = W - kBarW;
    const int x0 = (int(int64_t(slot) * travel / slotCount)) & ~1;
    fillRectUYVY(buf, strideBytes, x0, kPatternTop, kBarW, H - kPatternTop, 235,
                 128, 128);
}

// Exact sample index of tick n at 48 kHz for an fpsN/fpsD clock.
inline int64_t sampleForTick(int64_t n, int64_t fpsN, int64_t fpsD) {
    return n * kSampleRate * fpsD / fpsN;  // exact for broadcast rates
}

// 1 kHz tone burst occupying kToneBurstSamples after every flash tick.
inline float toneSample(int64_t s, int64_t flashPeriodSamples) {
    const int64_t ph = s % flashPeriodSamples;
    if (ph >= kToneBurstSamples) return 0.f;
    return 0.5f * std::sin(2.0 * M_PI * kToneHz * (double(ph) / kSampleRate));
}

}  // namespace moo::pattern
