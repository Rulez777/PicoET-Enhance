// Standalone on-device check: tongue-seg-check frames200.u8 out-logits.f32
// Runs tongue_seg::infer on every frame, writes logits, prints timing and geometry.
#include <cstdio>
#include <ctime>
#include <vector>
#include "tongue_seg.h"
static double now_ms() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    FILE* f = fopen(argv[1], "rb"); if (!f) return 3;
    std::vector<unsigned char> all; unsigned char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) all.insert(all.end(), buf, buf + n);
    fclose(f);
    int frames = int(all.size() / 40000);
    FILE* o = fopen(argv[2], "wb"); if (!o) return 4;
    float logits[56 * 56]; double total = 0, worst = 0;
    for (int i = 0; i < frames; i++) {
        double t0 = now_ms();
        tongue_seg::infer(all.data() + size_t(i) * 40000, logits);
        double dt = now_ms() - t0; total += dt; if (dt > worst) worst = dt;
        auto g = tongue_seg::geometry(all.data() + size_t(i) * 40000, logits);
        fwrite(logits, sizeof(float), 56 * 56, o);
        printf("%d present=%d area=%d angle=%.3f position=%.3f ms=%.1f\n", i, g.present, g.area, g.angle, g.position, dt);
    }
    fclose(o);
    printf("frames=%d mean_ms=%.1f worst_ms=%.1f\n", frames, total / frames, worst);
    return 0;
}
