# M91 Hands IK Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make two gloved hands visibly grip the M91 and follow the bolt knob / stripper clip during firing and reload, replacing the currently floating weapon.

**Architecture:** A self-contained `game/weapon_hands_ik` C++ subsystem builds a procedural, vertex-coloured tactical-glove mesh (no textures / new assets), solves each arm with analytic 2-bone IK toward sockets defined in the pack's `hand_ik_mounts.json` (coords converted pack→d3d), and draws the glove through the existing `d3d::DrawViewmodelMesh` foreground pass right after the weapon mesh. Weapon phase/pose drives right-hand target selection with smooth blending.

**Tech Stack:** C++20, D3D8 viewmodel pass, CMake (root globs `src/game/*.cpp`; tests list sources explicitly).

**Spec:** in-chat approved design (brainstorming session, 2026-11-17) — no spec file written; user gave explicit "start implementation" approval.

## Global Constraints
- Pack axis map: pack(x=forward, y=up, z=right) → d3d(z, y, x); reuse `tools/build_m91_glb.py` convention.
- Socket positions come from `hand_ik_mounts.json`, transformed by the same `world[16]` matrix used for the gun so sway/recoil/ADS are inherited.
- No new texture assets and no skeletal animation — engine uses rigid per-part transforms; use procedural vertex-coloured glove and analytic IK instead.
- Draw glove AFTER the textured gun using `d3d::DrawViewmodelMesh` (FVF XYZ|NORMAL|DIFFUSE) — same foreground depth slice [0,0.08], ZWrite=1.
- Scope: M91 only. Do not change behaviour of the other three special weapons.
- Preserve all existing uncommitted user work; do not revert or delete unrelated files.
- No adb device available: verification is host tests + arm64 objects + assembleRelease; visual QA must be done by user on device.

---

### Task 1: Pure IK/socket math + host tests

**Files:**
- Create: `src/game/weapon_hands_ik.h`
- Create: `src/game/weapon_hands_ik.cpp` (math portion)
- Modify: `tests/CMakeLists.txt` (add test executable)
- Test: `tests/test_weapon_hands_ik.cpp`

**Interfaces:**
- Produces: `namespace game::hands_ik` with `Vec3`, `PackToD3d(const float[3])`, `SolveTwoBoneIK(Vec3 shoulder, Vec3 target, float upper, float lower, Vec3 bend_dir, Vec3& elbow)`, `enum class GripTarget { Grip, BoltKnob, ClipThumb }`, `GripTarget SelectGripTarget(ActionPhase phase, bool reloading, float bolt_lift_deg, float clip_insert)`, `float SmoothTargetBlend(float dt, float speed)`.
- Produces: `void RenderWeaponHands(const float world[16], const float view[16], const float proj[16], const float eye[3], const float right[3], const float up[3], const float fwd[3], const sa::WeaponStatus& st, float dt)` — declared in header, implemented in Task 2.

- [ ] Step 1: Create header with the pure math declarations and socket constants; no D3D or SDL types in the public surface beyond `sa::WeaponStatus` from `special_arsenal_core.h`.
- [ ] Step 2: Write host tests: pack→d3d conversion of `[0.245,-0.018,0.008]`; IK reaches target exactly when distance < upper+lower and clamps at full extension; bend hint selects elbow side; SelectGripTarget maps Idle→Grip, BoltLift/BoltBack/BoltPushCocking/BoltLock→BoltKnob, reloading+clip_insert>0→ClipThumb, back to Grip in Idle after reload.
- [ ] Step 3: Add `tests/test_weapon_hands_ik.cpp` + `${REPO_ROOT}/src/game/weapon_hands_ik.cpp` to a new `add_executable` in `tests/CMakeLists.txt` with same include dirs as `test_settings_core`; register with `add_test`.
- [ ] Step 4: Implement the math in `weapon_hands_ik.cpp` so tests compile and pass (the render function can be a stub that returns immediately until Task 2).
- [ ] Step 5: Run `cmake -S tests -B tests/build_hands && cmake --build tests/build_hands && tests\build_hands\test_weapon_hands_ik.exe` → all pass.
- [ ] Step 6: Commit `feat: M91 hands IK math and host tests`.

### Task 2: Procedural glove mesh + viewmodel rendering

**Files:**
- Modify: `src/game/weapon_hands_ik.cpp` (render function)
- Modify: `src/game/flamethrower.cpp` (add include + one call after gun draw at ~line 1259)

**Interfaces:**
- Consumes: all Task 1 symbols; `d3d::DrawViewmodelMesh` and `d3d::ViewmodelVertex` from `src/d3d/world_overlay.h`.
- Produces: visible gloved hands in the M91 viewmodel.

- [ ] Step 1: In `RenderWeaponHands`, guard `st.id == sa::SpecialWeaponId::Mauser1891` and non-null pointers; early return otherwise.
- [ ] Step 2: Compute camera-relative shoulder anchors from `right/up/fwd`; convert all six socket world positions (pack→d3d) into eye-relative vectors then transform by `world[16]` to get final wrist targets; apply current right-target selection with a smoothed blend toward the chosen socket (state held in function-static, updated with `SmoothTargetBlend(dt, ...)`).
- [ ] Step 3: Solve 2-bone IK per arm (upper 0.28m, lower 0.26m, bend hint outward/forward), build a procedural glove mesh for each hand: palm box, four fingers × 3 tapered segments, thumb × 2 segments, forearm tapered elbow→wrist; vertex colours dark olive leather with darker crease bands and subtle knuckle highlights; compute per-vertex normals; index into a `uint16_t` triangle list.
- [ ] Step 4: Drive finger curl: index finger bend proportional to `st.pose.trigger_pull`; remaining fingers use curl values from `finger_curl_poses_deg` for the active grip role (hard-coded in a small const table in the .cpp).
- [ ] Step 5: Draw both hands with `d3d::DrawViewmodelMesh(verts, count, indices, icount, world, view, proj, pilot_world, 0.0f)` immediately after the weapon draw in `RenderAndStepFrame`.
- [ ] Step 6: Rebuild host tests (must still pass) and run arm64 ninja objects + `.\gradlew.bat assembleRelease --console=plain` → BUILD SUCCESSFUL.
- [ ] Step 7: Commit `feat: procedural gloved hands with IK for M91 viewmodel`.

### Task 3: Verification and handoff

**Files:** none (verification only)

- [ ] Step 1: Run `.\tests\build\test_settings_core.exe` → 54+ passed, 0 failed.
- [ ] Step 2: Run `cmake --build app\.cxx\RelWithDebInfo\1m2l1xh6\arm64-v8a` (or the existing gradle native build path) and confirm new `weapon_hands_ik.cpp.o` produced.
- [ ] Step 3: Run `.\gradlew.bat assembleRelease --console=plain` → BUILD SUCCESSFUL, APK size sane.
- [ ] Step 4: Report to user with explicit caveat: no adb device was available, so on-device visual QA was not performed; ask them to test and give numeric feedback for glove size/colour/position if needed.
