# Mauser C96 (`mauser_c96`) - Hard-Surface 3D Engine Asset Pack

## 1. Asset Overview
This package reproduces the historic German **Mauser C96 ("Broomhandle")** semi-automatic pistol chambered in `7.63x25mm Mauser` with its top-loading 10-round stripper clip (`StripperClip_MAG`), reciprocating square bolt (`Bolt_BOLT`), external perforated ring hammer (`Hammer_HAMMER`), curved trigger (`Trigger_TRIGGER`), left-side safety lever (`SafetyLever_SAFETY`), and graduated tangent rear sight (`RearSight_SIGHT`).

- **Format**: ASCII Wavefront `.OBJ` + `.MTL` (strictly triangulated, smooth area-weighted normals, physical 0.3–0.8 mm edge bevels).
- **Coordinate System**: `+X = FORWARD (muzzle)`, `+Y = UP`, `+Z = RIGHT`, units in **centimeters (`cm`)**, origin `(0, 0, 0)` at the grip / trigger housing center.
- **PBR Textures (`1024x1024`)**: `albedo_1024.png` (sRGB, unlit mid-tone), `normal_1024.png` (OpenGL `+Y` up), `roughness_1024.png` (linear), `metalness_1024.png` (linear).

## 2. Section 10 Self-Check Verification Table

| Check Item | Verified Value | Requirement |
| :--- | :--- | :--- |
| `triangles_final` | `38356` | `32,000 - 40,000` |
| `vertices_final` | `47079` | `Under 60,000 (< 65,535 16-bit cap)` |
| `triangles_highpoly` | `153424` | `150,000 - 400,000` |
| `group_count` | `7` | `8 or fewer` |
| `material_count` | `5` | `8 or fewer (all Kd in 0.08..0.45)` |
| `bbox_min` (cm) | `[-3.239000082015991, -8.805999755859375, -1.8799999952316284]` | `Model space in cm` |
| `bbox_max` (cm) | `[27.82699966430664, 16.149999618530273, 1.8799999952316284]` | `Model space in cm` |
| `all faces triangles?` | `YES` | `YES (100% 3-vertex faces)` |
| `all normals outward?` | `YES` | `YES (CCW winding verified)` |
| `all transforms applied?` | `YES` | `YES (Scale 1.0, Rotation 0)` |
| `any interior faces?` | `NO` | `must be NO` |
| `bevel on every hard edge?` | `YES` | `YES (0.03-0.08 cm bevels)` |
| `every moving part is its own group with a declared pivot?` | `YES` | `YES (6 moving groups + Receiver_Body)` |
| `reload animation returns to rest pose?` | `YES` | `YES (Frame 0 == Frame 78 rest pose)` |
| `zip contains every file listed in section 0?` | `YES` | `YES (Exact directory tree)` |

## 3. Triangle Breakdown per Group (`mauser_c96.obj`)
- `Receiver_Body`: `27500` triangles
- `StripperClip_MAG`: `2600` triangles
- `Bolt_BOLT`: `2910` triangles
- `RearSight_SIGHT`: `2678` triangles
- `Hammer_HAMMER`: `1270` triangles
- `SafetyLever_SAFETY`: `784` triangles
- `Trigger_TRIGGER`: `614` triangles
