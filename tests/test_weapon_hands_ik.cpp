#include <cmath>
#include <cstdio>

#include "game/special_arsenal_core.h"
#include "game/weapon_hands_ik.h"

namespace hi = game::hands_ik;
namespace sa = game::special_arsenal;

namespace {

int g_passed = 0;
int g_failed = 0;

#define EXPECT_TRUE(cond)                                                                   \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::fprintf(stderr, "[FAIL] %s:%d %s\n", __func__, __LINE__, #cond);          \
            ++g_failed;                                                                     \
            return;                                                                         \
        }                                                                                   \
    } while (0)

#define EXPECT_NEAR(a, b, eps) EXPECT_TRUE(std::fabs((a) - (b)) <= (eps))

void RunTest(const char* name, void (*fn)()) {
    const int before = g_failed;
    fn();
    if (g_failed == before) {
        ++g_passed;
        std::printf("[ OK ] %s\n", name);
    }
}

void TestPackToD3d() {
    const float p[3] = {0.245f, -0.018f, 0.008f};
    const hi::Vec3 v = hi::PackToD3d(p);
    EXPECT_NEAR(v.x, 0.008f, 1e-6f);
    EXPECT_NEAR(v.y, -0.018f, 1e-6f);
    EXPECT_NEAR(v.z, 0.245f, 1e-6f);
}

void TestSocketPositions() {
    const hi::Vec3 grip = hi::SocketPos(hi::HandSocket::RightGrip);
    EXPECT_NEAR(grip.x, 0.008f, 1e-6f);
    EXPECT_NEAR(grip.z, 0.245f, 1e-6f);
    const hi::Vec3 fore = hi::SocketPos(hi::HandSocket::LeftForegrip);
    EXPECT_NEAR(fore.x, -0.006f, 1e-6f);
    EXPECT_NEAR(fore.z, 0.555f, 1e-6f);
    const hi::Vec3 bolt = hi::SocketPos(hi::HandSocket::BoltKnob);
    EXPECT_NEAR(bolt.x, 0.048f, 1e-6f);
    EXPECT_NEAR(bolt.z, 0.295f, 1e-6f);
}

void TestTwoBoneReachable() {
    const hi::Vec3 shoulder{0.0f, 0.0f, 0.0f};
    const hi::Vec3 target{0.4f, 0.0f, 0.0f};
    hi::Vec3 elbow{};
    const bool ok = hi::SolveTwoBoneIK(shoulder, target, 0.28f, 0.26f, hi::Vec3{0, 1, 0}, elbow);
    EXPECT_TRUE(ok);
    EXPECT_NEAR(hi::Length(elbow - shoulder), 0.28f, 1e-3f);
    EXPECT_NEAR(hi::Length(elbow - target), 0.26f, 1e-3f);
}

void TestTwoBoneClampsAtFullExtension() {
    const hi::Vec3 shoulder{0.0f, 0.0f, 0.0f};
    const hi::Vec3 target{2.0f, 0.0f, 0.0f};
    hi::Vec3 elbow{};
    const bool ok = hi::SolveTwoBoneIK(shoulder, target, 0.28f, 0.26f, hi::Vec3{0, 1, 0}, elbow);
    EXPECT_TRUE(!ok);
    EXPECT_NEAR(hi::Length(elbow - shoulder), 0.28f, 1e-3f);
}

void TestTwoBoneBendHint() {
    const hi::Vec3 target{0.35f, 0.0f, 0.0f};
    hi::Vec3 up{}, down{};
    hi::SolveTwoBoneIK({}, target, 0.28f, 0.26f, hi::Vec3{0, 1, 0}, up);
    hi::SolveTwoBoneIK({}, target, 0.28f, 0.26f, hi::Vec3{0, -1, 0}, down);
    EXPECT_TRUE(up.y > 0.01f);
    EXPECT_TRUE(down.y < -0.01f);
}

void TestGripTargetSelection() {
    using sa::ActionPhase;
    using hi::GripTarget;
    EXPECT_TRUE(hi::SelectGripTarget(ActionPhase::Idle, false, 0.0f, 0.0f) == GripTarget::Grip);
    EXPECT_TRUE(hi::SelectGripTarget(ActionPhase::BoltLift, false, 0.0f, 0.0f) == GripTarget::BoltKnob);
    EXPECT_TRUE(hi::SelectGripTarget(ActionPhase::BoltBack, false, 0.0f, 0.0f) == GripTarget::BoltKnob);
    EXPECT_TRUE(hi::SelectGripTarget(ActionPhase::BoltPushCocking, false, 0.0f, 0.0f) == GripTarget::BoltKnob);
    EXPECT_TRUE(hi::SelectGripTarget(ActionPhase::BoltLock, false, 0.0f, 0.0f) == GripTarget::BoltKnob);
    EXPECT_TRUE(hi::SelectGripTarget(ActionPhase::Idle, false, 30.0f, 0.0f) == GripTarget::BoltKnob);
    EXPECT_TRUE(hi::SelectGripTarget(ActionPhase::Reloading, true, 0.0f, 0.5f) == GripTarget::ClipThumb);
    EXPECT_TRUE(hi::SelectGripTarget(ActionPhase::Idle, true, 0.0f, 0.5f) == GripTarget::ClipThumb);
    EXPECT_TRUE(hi::SelectGripTarget(ActionPhase::Idle, false, 0.0f, 0.0f) == GripTarget::Grip);
}

void TestSmoothing() {
    EXPECT_NEAR(hi::SmoothTargetBlend(0.0f), 0.0f, 1e-6f);
    const float small = hi::SmoothTargetBlend(0.016f);
    const float large = hi::SmoothTargetBlend(0.1f);
    EXPECT_TRUE(small > 0.0f && small < large);
    EXPECT_TRUE(large > 0.6f && large < 1.0f);
    // dt is clamped to 0.1s so one long frame can never snap the target.
    EXPECT_NEAR(hi::SmoothTargetBlend(1.0f), large, 1e-6f);
}

}  // namespace

int main() {
    RunTest("1_PackToD3d", TestPackToD3d);
    RunTest("2_SocketPositions", TestSocketPositions);
    RunTest("3_TwoBoneReachable", TestTwoBoneReachable);
    RunTest("4_TwoBoneClampsAtFullExtension", TestTwoBoneClampsAtFullExtension);
    RunTest("5_TwoBoneBendHint", TestTwoBoneBendHint);
    RunTest("6_GripTargetSelection", TestGripTargetSelection);
    RunTest("7_Smoothing", TestSmoothing);
    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
