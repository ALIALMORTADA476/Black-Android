/**
 * Kar98k Two-Handed IK & FPS Camera Mount Controller (Three.js)
 * يمنع بقاء السلاح طائراً في الهواء عبر ربطه بحامل الكاميرا وربط اليدين اليمنى واليسرى بمقابس الـ GLB المدمجة
 */
import * as THREE from 'three';

export class Kar98kHandMountController {
  constructor(camera, weaponScene, rightHandBone, leftHandBone, rightIndexTipBone = null) {
    this.camera = camera;
    this.weaponScene = weaponScene;
    this.rightHandBone = rightHandBone;
    this.leftHandBone = leftHandBone;
    this.rightIndexTipBone = rightIndexTipBone;

    // 1. إنشاء حامل السلاح المربوط بالكاميرا حتى لا يطفو السلاح في الفضاء العالمي
    this.weaponRigPivot = new THREE.Group();
    this.weaponRigPivot.name = 'FPS_Weapon_Rig_Holder';
    this.hipOffset = new THREE.Vector3(0.165, -0.145, -0.340);
    this.adsOffset = new THREE.Vector3(0.000, -0.071, -0.265);
    this.adsAlpha = 0.0;

    this.weaponRigPivot.position.copy(this.hipOffset);
    this.weaponRigPivot.rotation.set(0, Math.PI * 0.5, 0);
    this.camera.add(this.weaponRigPivot);
    this.weaponRigPivot.add(this.weaponScene);

    // 2. استخراج مقابس المسك المدمجة داخل ملف kar_98_lod0.glb
    this.sockets = {
      rightGrip: weaponScene.getObjectByName('Socket_RightHand_Grip'),
      triggerFinger: weaponScene.getObjectByName('Socket_TriggerFinger_IK'),
      leftForegrip: weaponScene.getObjectByName('Socket_LeftHand_Foregrip'),
      boltKnob: weaponScene.getObjectByName('Socket_BoltKnob_IK'),
      clipThumb: weaponScene.getObjectByName('Socket_ClipThumb_IK'),
      shoulderStock: weaponScene.getObjectByName('Socket_Shoulder_Stock'),
    };

    this.activeRightSocket = this.sockets.rightGrip;
    this._tmpPos = new THREE.Vector3();
    this._tmpQuat = new THREE.Quaternion();
  }

  /**
   * تبديل هدف اليد اليمنى تلقائياً أثناء التعمير (bolt_cycle) أو التلقيم (reload_stripper_clip)
   */
  updateRightHandTargetForAnimation(animName, elapsedTimeSec) {
    if (animName === 'bolt_cycle') {
      if (elapsedTimeSec >= 0.08 && elapsedTimeSec <= 0.84) {
        this.activeRightSocket = this.sockets.boltKnob;
      } else {
        this.activeRightSocket = this.sockets.rightGrip;
      }
    } else if (animName === 'reload_stripper_clip') {
      if (elapsedTimeSec >= 0.48 && elapsedTimeSec <= 1.72) {
        this.activeRightSocket = this.sockets.clipThumb;
      } else if (elapsedTimeSec >= 0.08 && elapsedTimeSec <= 2.48) {
        this.activeRightSocket = this.sockets.boltKnob;
      } else {
        this.activeRightSocket = this.sockets.rightGrip;
      }
    } else {
      this.activeRightSocket = this.sockets.rightGrip;
    }
  }

  /**
   * يُستدعى في كل فريم (requestAnimationFrame) لتثبيت اليدين على خشب البندقية والترباس
   */
  update(dt, isAiming = false) {
    const targetAlpha = isAiming ? 1.0 : 0.0;
    this.adsAlpha = THREE.MathUtils.lerp(this.adsAlpha, targetAlpha, Math.min(1.0, dt * 10.0));
    this.weaponRigPivot.position.lerpVectors(this.hipOffset, this.adsOffset, this.adsAlpha);

    // تثبيت اليد اليسرى دائماً تحت خشب السبطانة الأمامي (Socket_LeftHand_Foregrip)
    if (this.leftHandBone && this.sockets.leftForegrip) {
      this.sockets.leftForegrip.getWorldPosition(this._tmpPos);
      this.sockets.leftForegrip.getWorldQuaternion(this._tmpQuat);
      this.leftHandBone.position.copy(this._tmpPos);
      this.leftHandBone.quaternion.slerp(this._tmpQuat, Math.min(1.0, dt * 25.0));
    }

    // تثبيت اليد اليمنى على القبضة الخلفية أو كرة الترباس أو مشط الذخيرة
    if (this.rightHandBone && this.activeRightSocket) {
      this.activeRightSocket.getWorldPosition(this._tmpPos);
      this.activeRightSocket.getWorldQuaternion(this._tmpQuat);
      this.rightHandBone.position.copy(this._tmpPos);
      this.rightHandBone.quaternion.slerp(this._tmpQuat, Math.min(1.0, dt * 22.0));
    }

    // تثبيت إصبع السبابة على الزناد عندما تكون اليد على القبضة
    if (this.rightIndexTipBone && this.sockets.triggerFinger && this.activeRightSocket === this.sockets.rightGrip) {
      this.sockets.triggerFinger.getWorldPosition(this._tmpPos);
      this.rightIndexTipBone.position.copy(this._tmpPos);
    }
  }
}
