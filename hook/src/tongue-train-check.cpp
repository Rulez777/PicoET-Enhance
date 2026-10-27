// Standalone checks for on-device retraining (never writes the personal head):
//   tongue-train-check grad  frames200.u8 index mask56.u8 out.f32   -> loss + all head gradients
//   tongue-train-check train frames200.u8 labels.u8                  -> full run on recorded frames
#ifndef TONGUE_HEAD_FILE
#define TONGUE_TRAIN_NO_SAVE
#endif
#include <cstdio>
#include <ctime>
#include <vector>
#include "tongue_train.h"

static std::vector<unsigned char> slurp(const char* path) {
    std::vector<unsigned char> v; FILE* f = fopen(path, "rb"); if (!f) return v;
    unsigned char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) v.insert(v.end(), buf, buf + n);
    fclose(f); return v;
}

int main(int argc, char** argv) {
    if (argc >= 6 && !strcmp(argv[1], "grad")) {
        auto frames = slurp(argv[2]); auto mask = slurp(argv[4]);
        int idx = atoi(argv[3]);
        if (mask.size() != 56 * 56 || frames.size() < size_t(idx + 1) * 40000) return 2;
        tongue_seg::Context ctx; tongue_train::Grad g; tongue_train::zero_like(g);
        tongue_seg::Head h = tongue_seg::standard_head();
        tongue_seg::features(ctx, frames.data() + size_t(idx) * 40000);
        float loss = tongue_train::loss_and_grad(h, ctx, mask.data(), g);
        FILE* o = fopen(argv[5], "wb");
        fwrite(&loss, 4, 1, o);
        for (auto* part : {&g.w1, &g.b1, &g.w2, &g.b2, &g.w3, &g.b3}) fwrite(part->data(), 4, part->size(), o);
        fclose(o);
        printf("loss=%.6f\n", loss);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "train")) {
        auto frames = slurp(argv[2]); auto labels = slurp(argv[3]);
        size_t n = labels.size();
        if (frames.size() < n * 40000) return 2;
        tongue_seg::Context ctx; static float logits[56 * 56];
        for (size_t i = 0; i < n && tongue_train::samples.size() < tongue_train::kMaxSamples; i++) {
            if (!labels[i]) continue;
            tongue_seg::features(ctx, frames.data() + i * 40000);
            tongue_seg::head_forward(tongue_seg::active_head, ctx, logits);
            tongue_train::recording = labels[i];
            tongue_train::record(frames.data() + i * 40000, logits);
        }
        tongue_train::recording = tongue_train::NONE;
        printf("samples=%zu closed=%d open=%d center=%d left=%d right=%d\n", tongue_train::samples.size(),
               tongue_train::count(1), tongue_train::count(2), tongue_train::count(3), tongue_train::count(4), tongue_train::count(5));
        timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
        tongue_train::state = tongue_train::RUNNING;
        tongue_train::run(nullptr);
        clock_gettime(CLOCK_MONOTONIC, &b);
        printf("state=%u old_score=%.3f new_score=%.3f seconds=%.1f\n", tongue_train::state.load(),
               tongue_train::old_score.load(), tongue_train::new_score.load(), (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9);
        if (tongue_train::pending_calibration_valid) {
            auto& c = tongue_train::pending_calibration;
            printf("calibration %.3f %.3f %.3f %d\n", c.left, c.center, c.right, c.min_area);
        }
#ifndef TONGUE_TRAIN_NO_SAVE
        if (tongue_train::pending_head) {   // saved file must reload to exactly the trained head
            tongue_seg::Head loaded;
            bool ok = tongue_train::load_head(loaded) && tongue_train::flatten(loaded) == tongue_train::flatten(*tongue_train::pending_head);
            printf("saved head %s reload %s\n", tongue_train::kHeadFile, ok ? "IDENTICAL" : "MISMATCH");
        }
#endif
        return 0;
    }
    fprintf(stderr, "usage: grad frames idx mask out | train frames labels\n");
    return 1;
}
