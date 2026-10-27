// Exercises the accept path of on-device retraining against a test head file (never the real one):
// trains, accepts if left/right stay separate, saves, reloads and compares, prints the calibration.
#define TONGUE_HEAD_FILE "/data/local/tmp/tongue-head-test.bin"
#define TONGUE_TRAIN_ACCEPT_ANY
#include "tongue-train-check.cpp"
