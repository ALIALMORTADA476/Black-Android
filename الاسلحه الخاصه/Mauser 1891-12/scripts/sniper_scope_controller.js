/**
 * Kar98k ZF39 Sniper Scope & Breath-Hold Optical System (Three.js)
 * يدعم التكبير المتغير (4x / 6x)، تصفير المسافة (100m - 800m)، حبس النفس، وظل عدسة العين (Eye-Relief Vignette)
 */
import * as THREE from 'three';

export class Kar98kScopeSystem {
  constructor(camera, weaponScene, reticleTextureUrl = './ui/sniper_scope_reticle_1024.png') {
    this.camera = camera;
    this.weaponScene = weaponScene;
    this.baseFov = camera.fov || 60.0;
    this.zoomLevels = [
      { label: '4x', fov: 15.0, sens: 0.25 },
      { label: '6x', fov: 10.0, sens: 0.165 }
    ];
    this.currentZoomIdx = 0;
    this.zeroingStepsM = [100, 200, 300, 400, 500, 600, 800];
    this.zeroingIdx = 0;

    this.isScoped = false;
    this.scopeBlend = 0.0;
    this.isHoldingBreath = false;
    this.breathStamina = 1.0; // 1.0 = full (4.5s), 0.0 = exhausted
    this.swayTime = 0.0;
    this.eyeReliefOffset = new THREE.Vector2(0, 0);

    this.scopeSocket = weaponScene.getObjectByName('Socket_ScopeAim');
    this.scopeMeshGroup = weaponScene.getObjectByName('Scope_Assembly');
    this._createScopeHUD(reticleTextureUrl);
  }

  _createScopeHUD(reticleUrl) {
    if (typeof document === 'undefined') return;
    this.overlay = document.createElement('div');
    this.overlay.style.cssText = `
      position: fixed; inset: 0; pointer-events: none; opacity: 0;
      transition: opacity 0.12s ease-out; display: flex; align-items: center; justify-content: center;
      background: radial-gradient(circle at center, transparent 36%, rgba(0,0,0,0.92) 43%, #000 52%);
      z-index: 50;
    `;
    this.reticleImg = document.createElement('img');
    this.reticleImg.src = reticleUrl;
    this.reticleImg.style.cssText = 'width: 68vmin; height: 68vmin; object-fit: contain; transform: translate(0px, 0px);';
    this.overlay.appendChild(this.reticleImg);

    this.infoBadge = document.createElement('div');
    this.infoBadge.style.cssText = `
      position: fixed; bottom: 8%; right: 12%; color: #e2e8f0; font-family: monospace;
      font-size: 13px; background: rgba(15,23,42,0.75); padding: 6px 12px; border-radius: 4px;
    `;
    this.overlay.appendChild(this.infoBadge);
    document.body.appendChild(this.overlay);
    this._updateBadge();
  }

  toggleZoom() {
    this.currentZoomIdx = (this.currentZoomIdx + 1) % this.zoomLevels.length;
    this._updateBadge();
    return this.zoomLevels[this.currentZoomIdx];
  }

  cycleZeroing(direction = 1) {
    this.zeroingIdx = Math.max(0, Math.min(this.zeroingStepsM.length - 1, this.zeroingIdx + direction));
    this._updateBadge();
    return this.zeroingStepsM[this.zeroingIdx];
  }

  applyRecoilEyeReliefImpulse(kickStrength = 1.0) {
    this.eyeReliefOffset.y += 18.0 * kickStrength;
    this.eyeReliefOffset.x += (Math.random() - 0.5) * 10.0 * kickStrength;
  }

  _updateBadge() {
    if (!this.infoBadge) return;
    const z = this.zoomLevels[this.zoomLevels.length > 0 ? this.currentZoomIdx : 0];
    const dist = this.zeroingStepsM[this.zeroingIdx];
    this.infoBadge.textContent = `ZF39 ${z.label} | ZERO: ${dist}m | STAMINA: ${Math.round(this.breathStamina * 100)}%`;
  }

  update(dt, isAiming, holdBreathInput = false) {
    this.isScoped = isAiming;
    const targetBlend = isAiming ? 1.0 : 0.0;
    this.scopeBlend = THREE.MathUtils.lerp(this.scopeBlend, targetBlend, Math.min(1.0, dt * 11.0));

    // تحديث زاوية الرؤية (FOV) للتكبير البصري الحقيقي
    const targetFov = isAiming ? this.zoomLevels[this.currentZoomIdx].fov : this.baseFov;
    this.camera.fov = THREE.MathUtils.lerp(this.camera.fov, targetFov, Math.min(1.0, dt * 12.0));
    this.camera.updateProjectionMatrix();

    // إدارة حبس النفس واهتزاز التنفس الطبيعي للقناص
    if (isAiming && holdBreathInput && this.breathStamina > 0.05) {
      this.isHoldingBreath = true;
      this.breathStamina = Math.max(0.0, this.breathStamina - dt / 4.5);
    } else {
      this.isHoldingBreath = false;
      this.breathStamina = Math.min(1.0, this.breathStamina + dt / 3.0);
    }

    const swayAmp = this.isHoldingBreath ? 0.0003 : (this.breathStamina < 0.15 ? 0.0045 : 0.0016);
    this.swayTime += dt * (this.isHoldingBreath ? 0.8 : 2.1);
    const swayX = Math.sin(this.swayTime * 1.7) * swayAmp;
    const swayY = Math.cos(this.swayTime * 2.3) * swayAmp;

    if (isAiming) {
      this.camera.rotation.x += swayY;
      this.camera.rotation.y += swayX;
    }

    // استعادة تدريجية لظل عدسة المنظار بعد الارتداد
    this.eyeReliefOffset.lerp(new THREE.Vector2(0, 0), Math.min(1.0, dt * 9.0));
    if (this.overlay) {
      this.overlay.style.opacity = this.scopeBlend > 0.72 ? String((this.scopeBlend - 0.72) / 0.28) : '0';
      if (this.reticleImg) {
        this.reticleImg.style.transform = `translate(${this.eyeReliefOffset.x.toFixed(1)}px, ${this.eyeReliefOffset.y.toFixed(1)}px)`;
      }
      this._updateBadge();
    }
  }
}
