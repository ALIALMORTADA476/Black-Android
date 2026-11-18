# First-Person Skinned Hands (`hands_lod0.glb`)

- **Format**: Binary glTF 2.0 (`.glb`) with embedded `JOINTS_0`, `WEIGHTS_0`, `inverseBindMatrices`, and `hands_tex_0.jpg` (1024×1024).
- **Geometry**: 14160 vertices (`uint16` indices < 65,536), 25632 triangles.
- **Skeleton**: Full 41-node armature (`World`, `Extra`, `Root`, plus `Shoulder`, `Elbow`, `Joint_wrist_twist`, `Wrist`, `Thumb_Top|Mid|Tip`, `Finger1..4_Top|Mid|Tip` for both `_lft` and `_rt`).
- **Alignment**: Positioned in `pack(x=forward, y=up, z=right)` meters to directly grasp `Socket_LeftHand_Foregrip` and `Socket_RightHand_Grip` / `Socket_TriggerFinger_IK`.
