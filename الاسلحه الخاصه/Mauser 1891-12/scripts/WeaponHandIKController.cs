using UnityEngine;

/// <summary>
/// يربط يدي اللاعب بمقابس الـ Kar98k المدمجة داخل ملف الـ GLB عبر OnAnimatorIK لمنع بقاء السلاح طائراً
/// </summary>
[RequireComponent(typeof(Animator))]
public class WeaponHandIKController : MonoBehaviour
{
    public Transform weaponRoot;
    public Transform socketRightHandGrip;
    public Transform socketLeftHandForegrip;
    public Transform socketBoltKnobIK;
    public Transform socketClipThumbIK;

    [Range(0f, 1f)] public float leftHandIKWeight = 1.0f;
    [Range(0f, 1f)] public float rightHandIKWeight = 1.0f;

    private Animator _animator;
    private Transform _currentRightTarget;

    void Awake()
    {
        _animator = GetComponent<Animator>();
        if (weaponRoot != null)
        {
            socketRightHandGrip = FindDeepChild(weaponRoot, "Socket_RightHand_Grip");
            socketLeftHandForegrip = FindDeepChild(weaponRoot, "Socket_LeftHand_Foregrip");
            socketBoltKnobIK = FindDeepChild(weaponRoot, "Socket_BoltKnob_IK");
            socketClipThumbIK = FindDeepChild(weaponRoot, "Socket_ClipThumb_IK");
        }
        _currentRightTarget = socketRightHandGrip;
    }

    public void SetBoltCyclePhase(bool grabBoltKnob)
    {
        _currentRightTarget = grabBoltKnob ? socketBoltKnobIK : socketRightHandGrip;
    }

    public void SetClipReloadPhase(bool grabClip)
    {
        _currentRightTarget = grabClip ? socketClipThumbIK : socketBoltKnobIK;
    }

    void OnAnimatorIK(int layerIndex)
    {
        if (_animator == null) return;

        // 1. اليد اليسرى تحمل الحاضن الخشبي الأمامي دائماً
        if (socketLeftHandForegrip != null)
        {
            _animator.SetIKPositionWeight(AvatarIKGoal.LeftHand, leftHandIKWeight);
            _animator.SetIKRotationWeight(AvatarIKGoal.LeftHand, leftHandIKWeight);
            _animator.SetIKPosition(AvatarIKGoal.LeftHand, socketLeftHandForegrip.position);
            _animator.SetIKRotation(AvatarIKGoal.LeftHand, socketLeftHandForegrip.rotation);
        }

        // 2. اليد اليمنى تمسك القبضة أو تنتقل إلى كرة الترباس أثناء التعمير
        if (_currentRightTarget != null)
        {
            _animator.SetIKPositionWeight(AvatarIKGoal.RightHand, rightHandIKWeight);
            _animator.SetIKRotationWeight(AvatarIKGoal.RightHand, rightHandIKWeight);
            _animator.SetIKPosition(AvatarIKGoal.RightHand, _currentRightTarget.position);
            _animator.SetIKRotation(AvatarIKGoal.RightHand, _currentRightTarget.rotation);
        }
    }

    private Transform FindDeepChild(Transform parent, string targetName)
    {
        foreach (Transform child in parent)
        {
            if (child.name == targetName) return child;
            Transform found = FindDeepChild(child, targetName);
            if (found != null) return found;
        }
        return null;
    }
}
