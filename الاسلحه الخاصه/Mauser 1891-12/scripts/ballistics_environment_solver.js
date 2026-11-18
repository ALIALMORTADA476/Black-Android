/**
 * Kar98k 7.92x57mm Ballistics, Penetration & Environmental Impact Solver (Three.js)
 * يحسب مسار الرصاصة، الجاذبية، مقاومة الهواء، اختراق الجدران/الخشب/الحديد، الارتداد (Ricochet)، ودفع الأجسام الفيزيائية
 */
import * as THREE from 'three';

export class Kar98kBallisticsAndEnvironmentSolver {
  constructor(scene, weaponStats, onSurfaceHitCallback = null) {
    this.scene = scene;
    this.stats = weaponStats;
    this.onSurfaceHit = onSurfaceHitCallback;
    this.activeProjectiles = [];
    this.gravity = new THREE.Vector3(0, -9.81, 0);
    this.raycaster = new THREE.Raycaster();
  }

  /**
   * إطلاق مقذوف 7.92x57mm حقيقي من فوهة السلاح مع موجة صدمة الفوهة (Muzzle Shockwave) في المحيط
   */
  spawnProjectile(muzzleWorldPos, aimDirection, zeroingDistanceM = 100, collidableMeshes = [], rigidbodies = []) {
    // 1. تأثير عصف الفوهة (Muzzle Blast Shockwave) على الأجسام القريبة والغبار في المحيط
    this._applyMuzzleShockwave(muzzleWorldPos, aimDirection, rigidbodies);

    // 2. حساب زاوية الرفع (Zeroing Elevation Angle) لتعويض سقوط الجاذبية عند المسافة المحددة
    const v0 = this.stats.muzzle_velocity_m_s || 760.0;
    const flightTimeZero = zeroingDistanceM / v0;
    const dropAtZero = 0.5 * 9.81 * flightTimeZero * flightTimeZero;
    const pitchCompensationRad = Math.atan2(dropAtZero, zeroingDistanceM);

    const launchDir = aimDirection.clone().normalize();
    launchDir.y += Math.sin(pitchCompensationRad);
    launchDir.normalize();

    this.activeProjectiles.push({
      pos: muzzleWorldPos.clone(),
      vel: launchDir.multiplyScalar(v0),
      damage: this.stats.damage_profile.base_damage,
      distanceTraveled: 0.0,
      penetrationsLeft: 3,
      alive: true,
      collidableMeshes,
      rigidbodies
    });
  }

  _applyMuzzleShockwave(muzzlePos, dir, rigidbodies) {
    const radius = this.stats.environmental_interactions.muzzle_shockwave_radius_m || 2.8;
    const maxForce = this.stats.environmental_interactions.muzzle_dust_kickup_force_n || 180.0;
    for (const rb of rigidbodies) {
      if (!rb.position) continue;
      const d = rb.position.distanceTo(muzzlePos);
      if (d > 0.05 && d < radius) {
        const falloff = Math.pow(1.0 - d / radius, 2);
        const pushDir = rb.position.clone().sub(muzzlePos).normalize();
        if (rb.applyImpulse) {
          rb.applyImpulse(pushDir.multiplyScalar(maxForce * falloff * 0.02));
        }
      }
    }
  }

  update(dt) {
    for (const p of this.activeProjectiles) {
      if (!p.alive) continue;

      const speed = p.vel.length();
      if (speed < 45.0 || p.distanceTraveled > 1000.0) {
        p.alive = false;
        continue;
      }

      // تباطؤ مقاومة الهواء (G7 Ballistic Drag) + الجاذبية الأرضية
      const dragDecel = 0.00042 * speed * speed;
      const dragVec = p.vel.clone().normalize().multiplyScalar(-dragDecel * dt);
      p.vel.add(dragVec).addScaledVector(this.gravity, dt);

      const stepVec = p.vel.clone().multiplyScalar(dt);
      const stepLen = stepVec.length();
      const stepDir = stepVec.clone().normalize();

      this.raycaster.set(p.pos, stepDir);
      this.raycaster.far = stepLen;
      const hits = this.raycaster.intersectObjects(p.collidableMeshes, true);

      if (hits.length > 0) {
        const hit = hits[0];
        const matType = (hit.object.userData && hit.object.userData.surfaceType) || 'concrete_brick_wall';
        const surf = this.stats.environmental_interactions.surface_penetration_table[matType]
                  || this.stats.environmental_interactions.surface_penetration_table.concrete_brick_wall;

        // حساب زاوية الاصطدام للتحقق من ارتداد الرصاصة (Ricochet)
        const normal = hit.face ? hit.face.normal.clone().transformDirection(hit.object.matrixWorld).normalize() : new THREE.Vector3(0, 1, 0);
        const cosIncident = Math.abs(stepDir.dot(normal));
        const grazingAngleDeg = 90.0 - THREE.MathUtils.radToDeg(Math.acos(Math.min(1.0, cosIncident)));

        // دفع الجسم الفيزيائي في المحيط (مثل البراميل، الأبواب، الصناديق)
        if (hit.object.userData && hit.object.userData.rigidbody && hit.object.userData.rigidbody.applyImpulseAtPoint) {
          const impulseMag = (this.stats.environmental_interactions.rigidbody_impact_impulse_newton_sec || 48.5) * (speed / 760.0);
          hit.object.userData.rigidbody.applyImpulseAtPoint(stepDir.clone().multiplyScalar(impulseMag), hit.point);
        }

        // إشعار النظام البصري والصوتي لتوليد الثقب (Decal) والغبار/الشرار والصوت الموضعي
        if (this.onSurfaceHit) {
          this.onSurfaceHit({
            point: hit.point,
            normal,
            surfaceType: matType,
            decalTexture: surf.impact_decal,
            particleEffect: surf.particle_effect,
            impactSound: surf.impact_sound,
            damageDealt: p.damage
          });
        }

        if (grazingAngleDeg <= (surf.ricochet_angle_deg || 15.0) && p.penetrationsLeft > 0) {
          // ارتداد الرصاصة عن السطح بزاوية حادة (Ricochet)
          p.vel.reflect(normal).multiplyScalar(0.55);
          p.damage *= 0.50;
          p.penetrationsLeft -= 1;
          p.pos.copy(hit.point).addScaledVector(normal, 0.02);
        } else if (p.penetrationsLeft > 0 && ((hit.object.userData && hit.object.userData.thicknessCm) || 8.0) <= surf.max_penetration_thickness_cm) {
          // اختراق الحاجز (Wallbang / Material Penetration) مع فقدان جزء من السرعة والضرر
          p.vel.multiplyScalar(surf.velocity_retained_ratio || 0.6);
          p.damage *= (surf.damage_retained_ratio || 0.65);
          p.penetrationsLeft -= 1;
          p.pos.copy(hit.point).addScaledVector(stepDir, 0.08);
        } else {
          p.alive = false;
        }
      } else {
        p.pos.add(stepVec);
        p.distanceTraveled += stepLen;
      }
    }
    this.activeProjectiles = this.activeProjectiles.filter(p => p.alive);
  }
}
