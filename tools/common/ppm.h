#pragma once
#include <cstdint>
#include <cstdio>

// Minimal dependency-free P6 writer (drops alpha) for headless verification.
namespace moo::ppm {

inline bool writeRgba(const char* path, int w, int h, const uint8_t* rgba) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) fwrite(rgba + size_t(i) * 4, 1, 3, f);
    fclose(f);
    return true;
}

}  // namespace moo::ppm
