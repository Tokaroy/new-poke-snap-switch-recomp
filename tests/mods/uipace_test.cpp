// Native test of the interface pacing decision and the pad sampling (host/mods/uipace_core.h). No game needed.
#include <cstdio>
#include <initializer_list>
#include <cstdlib>
#include "../../host/mods/uipace_core.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// Steps taken in one second of frames at hz (period from hz), with an optional hitch (one frame of hitch_ms).
static int steps_in_second(double hz, double hitch_ms = 0) {
    uipace::Pace p;
    const uint64_t period = (uint64_t)(1e9 / hz);
    uint64_t t = 1000000000ull;
    int steps = 0, frames = 0;
    uint64_t start = t;
    for (uint64_t f = 0; t - start < 1000000000ull; f++, frames++) {
        steps += uipace::decide(p, f, t, period);
        t += period + (f == 100 && hitch_ms > 0 ? (uint64_t)(hitch_ms * 1e6) : 0);
    }
    (void)frames;
    return steps;
}

int main() {
    // 60 fps and below: every frame steps. Above: 60 steps a second of real time.
    CHECK(steps_in_second(60) == 60 || steps_in_second(60) == 61);
    CHECK(steps_in_second(30) == 30 || steps_in_second(30) == 31);
    for (double hz : {90.0, 120.0, 144.0}) {
        int s = steps_in_second(hz);
        std::printf("%.0f fps: %d steps in one second\n", hz, s);
        CHECK(s >= 59 && s <= 61);
    }
    // A hitch of 400 ms at 144 fps banks at most one step per frame and at most kMaxDtNs: 100 ms = 6 steps over the next frames, never a burst.
    {
        uipace::Pace p;
        const uint64_t period = (uint64_t)(1e9 / 144.0);
        uint64_t t = 1000000000ull;
        int consecutive = 0, max_consecutive = 0;
        for (uint64_t f = 0; f < 400; f++) {
            int s = uipace::decide(p, f, t, period);
            consecutive = s ? consecutive + 1 : 0;
            if (f > 20) max_consecutive = consecutive > max_consecutive ? consecutive : max_consecutive;
            t += period + (f == 100 ? 400000000ull : 0);
        }
        CHECK(max_consecutive <= 7);   // one step per frame; the banked time is capped at 100 ms
    }
    // The same frame id returns the decision already made.
    {
        uipace::Pace p;
        const uint64_t period = (uint64_t)(1e9 / 144.0);
        int a = uipace::decide(p, 5, 1000000000ull, period);
        int b = uipace::decide(p, 5, 1000000000ull + 50000000ull, period);
        CHECK(a == b);
    }
    // Pad sampling at 144 fps: steps every 2 or 3 frames. A press seen on a non-step frame reaches the next step exactly once.
    {
        uipace::PadState s;
        uint32_t mask[4] = {0, 0, 0, 0};
        uint8_t bytes[64] = {};
        // frame 0: step, nothing held
        uipace::sample_pad(s, mask, bytes, true, false);
        // frame 1: not a step, button 3 pressed
        mask[0] = 1u << 3, bytes[3] = 1;
        uipace::sample_pad(s, mask, bytes, false, false);
        CHECK(mask[0] == 0 && mask[1] == 0 && bytes[3] == 0);   // shows the last step: nothing
        // frame 2: step, still held: pressed edge once
        mask[0] = 1u << 3, bytes[3] = 1;
        uipace::sample_pad(s, mask, bytes, true, false);
        CHECK(mask[0] == (1u << 3) && mask[1] == (1u << 3) && bytes[3] == 1 && bytes[32 + 3] == 0);
        // frame 3: non-step, still held: no new edge
        mask[0] = 1u << 3, bytes[3] = 1;
        uipace::sample_pad(s, mask, bytes, false, false);
        CHECK(mask[0] == (1u << 3) && mask[1] == 0 && bytes[3] == 1 && bytes[32 + 3] == 1);
        // frame 4: step, still held: no edge
        mask[0] = 1u << 3, bytes[3] = 1;
        uipace::sample_pad(s, mask, bytes, true, false);
        CHECK(mask[1] == 0 && bytes[32 + 3] == 1);
        // frame 5: non-step, released and pressed-and-released button 5 in between
        mask[0] = 1u << 5, bytes[5] = 1;
        uipace::sample_pad(s, mask, bytes, false, false);
        // frame 6: step, nothing held: the tap of button 5 counts once, button 3 released
        mask[0] = 0, bytes[3] = 0, bytes[5] = 0;
        uipace::sample_pad(s, mask, bytes, true, false);
        CHECK((mask[1] & (1u << 5)) && (mask[2] & (1u << 3)));
        CHECK(bytes[5] == 1);
        // frame 7: step again, nothing: the tap does not repeat
        mask[0] = 0, bytes[5] = 0;
        uipace::sample_pad(s, mask, bytes, true, false);
        CHECK(mask[1] == 0);
    }
    // At 60 fps the pad is left as the game wrote it.
    {
        uipace::PadState s;
        uint32_t mask[4] = {1, 1, 0, 0};
        uint8_t bytes[64] = {};
        bytes[0] = 1;
        uipace::sample_pad(s, mask, bytes, true, true);
        CHECK(mask[0] == 1 && mask[1] == 1 && bytes[0] == 1);
    }
    // Pointer time (FPS-02): steady frames give their own length; 0 or a huge gap falls back to the pace period; a hitch counts in full;
    // the result stays within 1 to 100 ms. Pointer travel per second is then the same at 60 and 144 fps.
    {
        double avg = 0;
        float dt = 0;
        for (int i = 0; i < 64; i++) dt = uipace::pointer_dt(avg, 6944444, 6944444);
        CHECK(dt > 0.00693f && dt < 0.00696f);
        dt = uipace::pointer_dt(avg, 0, 6944444);
        CHECK(dt > 0.00693f && dt < 0.00696f);
        dt = uipace::pointer_dt(avg, 40000000, 6944444);   // a 40 ms hitch: its own length
        CHECK(dt > 0.0399f && dt < 0.0401f);
        double a2 = 0;
        CHECK(uipace::pointer_dt(a2, 500000, 0) >= 0.001f);   // floor 1 ms
        double travel144 = 0, travel60 = 0, a144 = 0, a60 = 0;
        for (int i = 0; i < 144; i++) travel144 += uipace::pointer_dt(a144, 6944444, 6944444) * 60.0;   // step = speed * dt * 60
        for (int i = 0; i < 60; i++) travel60 += uipace::pointer_dt(a60, 16666667, 16666667) * 60.0;
        std::printf("pointer travel in one second (units of one 60 fps step): %.2f at 144, %.2f at 60\n", travel144, travel60);
        CHECK(travel144 > 59.5 && travel144 < 60.5 && travel60 > 59.5 && travel60 < 60.5);
    }
    std::printf(g_fail ? "uipace: %d FAILED\n" : "uipace: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
