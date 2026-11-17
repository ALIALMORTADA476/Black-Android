# Heavy Assault Flamethrower (`flamethrower`) — Engine-Ready Hard-Surface 3D Asset Pack

## 1. Human Summary
This package delivers a complete, engine-ready 3D hard-surface weapon asset pack of the **Heavy Assault Flamethrower (`flamethrower`)** with twin underslung pressurized olive-drab fuel tanks, perforated cylindrical barrel heat shroud, heat-blued conical flame nozzle, under-barrel pilot igniter rod with brass tip, vertical tactical foregrip, top pressure gauge, and braided fuel hoses, faithfully reproduced from the multi-view reference sheet (`SIDE VIEW`, `FRONT VIEW`, and `3/4 PERSPECTIVE VIEW`).

- **Coordinate System & Units (`§1`)**: Centimeters (`cm`), `+X = FORWARD` (muzzle direction), `+Y = UP`, `+Z = RIGHT`.
- **Origin `(0, 0, 0)`**: Positioned at the center of the pistol grip / trigger housing (`[0.0, 0.0, 0.0]`), extending rearward to the rubber buttpad (`X = -26.5 cm`) and forward along `+X` to the heat-blued muzzle nozzle tip (`X = 62.5 cm`).
- **Smooth Shading & Real Geometric Bevels (`§1`)**: Every hard edge across all 8 groups carries a real geometric chamfer/bevel of `0.03–0.08 cm` (`0.3–0.8 mm`) with outward-facing counter-clockwise (`CCW`) normals, closed manifold shells, zero interior faces, and minimum feature size `>= 0.25 cm` (`2.5 mm > 1.5 mm` limit).

---

## 2. §10 Self-Check Readout (Exact Required Keys & Verified Numbers)

- `triangles_final`: **35264** (strictly within `32,000–40,000`)
- `vertices_final`: **17706** (strictly under `60,000` / `65,535` cap)
- `triangles_highpoly`: **202806** (strictly within `150,000–400,000`)
- `group_count`: **8** (`8 or fewer`)
- `material_count`: **7** (`8 or fewer`)
- `bbox_min` (in cm): **`[-26.5, -23.042, -5.534]`**
- `bbox_max` (in cm): **`[62.5, 9.8, 5.194]`**
- `all faces triangles?`: **YES** (`0` quads, `0` n-gons)
- `all normals outward?`: **YES** (verified CCW winding against local shell centroids)
- `all transforms applied?`: **YES** (scale `1.0`, rotation `0.0`)
- `any interior faces?`: **NO** (closed exterior manifold shells only)
- `bevel on every hard edge?`: **YES** (`0.03–0.08 cm` real geometric chamfers on all hard edges)
- `every moving part is its own group with a declared pivot?`: **YES**
- `reload animation returns to rest pose?`: **YES** (frame `0` and frame `78` are exact rest pose)
- `zip contains every file listed in section 0?`: **YES** (`24/24` exact files)

---

## 3. Part Groups, Triangle Sub-Budgets (`§2`, `§7`) & Pivots (`§3`)
| Group Name | Role Token | Moving? | Pivot `[X, Y, Z]` (`cm`) | Axis | Triangles (`Final`) | `§7` Sub-Budget Category |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `Receiver_Body` | Static Base | No | `[0.0, 0.0, 0.0]` | Static | **26,720** | Receiver+static (`14,200`) + Barrel+muzzle (`6,200`) + Stock+grip (`5,900`) + Sights (`960`) |
| `FuelTankPack_TANK` | `_TANK` | Yes | `[16.0, -5.5, 0.0]` | Slides `Y` | **3,216** | Magazine / feed group (`2,000 – 3,500`) |
| `ChargingHandle_CHARGE` | `_CHARGE` | Yes | `[8.5, 5.35, 0.0]` | Slides `X` | **2,592** | Bolt / charging group (`2,500 – 3,500`) |
| `Trigger_TRIGGER` | `_TRIGGER` | Yes | `[-0.4, -0.35, 0.0]` | Rotates `Z` | **480** | Trigger + safety (`800 – 1,200` combined: `960`) |
| `SafetyLever_SAFETY` | `_SAFETY` | Yes | `[-1.8, 4.45, -2.2]` | Rotates `Z` | **480** | Trigger + safety (`800 – 1,200` combined) |
| `TankLatch_LATCH` | `_LATCH` | Yes | `[9.9, -4.8, 0.0]` | Rotates `Z` | **480** | Weapon-specific moving part |
| `RegulatorValve_VALVE` | `_VALVE` | Yes | `[15.5, -14.0, 0.0]` | Rotates `X` | **672** | Weapon-specific moving part |
| `PressureGauge_GAUGE` | `_GAUGE` | Yes | `[19.7, -3.2, 0.0]` | Rotates `Z` | **624** | Weapon-specific moving part |
