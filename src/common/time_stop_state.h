#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace common {

enum class TimeStopMode : uint32_t {
    Toggle = 0,
    Hold = 1,
};

struct TimeStopConfig {
    TimeStopMode mode = TimeStopMode::Toggle;
    float max_duration_sec = 0.0f;  // 0.0f = unlimited
    float cooldown_sec = 0.0f;      // 0.0f = no cooldown
};

inline float SanitizeFreezeWorldDelta(float world_dt) {
    if (!std::isfinite(world_dt) || world_dt <= 0.0f) {
        return 0.0f;
    }
    return std::min(world_dt, 0.25f);
}

class TimeStopStateMachine {
public:
    bool IsFrozen() const { return frozen_; }
    bool IsOnCooldown() const { return cooldown_remaining_ > 0.0001f; }
    float ActiveElapsedSec() const { return active_elapsed_; }
    float CooldownRemainingSec() const { return cooldown_remaining_; }

    float RemainingDurationSec(const TimeStopConfig& cfg) const {
        if (!frozen_ || cfg.max_duration_sec <= 0.0f) {
            return 0.0f;
        }
        return std::max(0.0f, cfg.max_duration_sec - active_elapsed_);
    }

    bool CanActivate(const TimeStopConfig& cfg, bool world_active) const {
        if (!world_active) return false;
        if (frozen_) return true;
        return cooldown_remaining_ <= 0.0001f || cfg.cooldown_sec <= 0.0f;
    }

    bool OnTriggerPressed(const TimeStopConfig& cfg, bool world_active) {
        if (!world_active) {
            ForceResume(false);
            return false;
        }
        if (cfg.mode == TimeStopMode::Hold) {
            if (!frozen_ && CanActivate(cfg, world_active)) {
                EnterFreeze();
                hold_pressed_ = true;
                return true;
            }
            return frozen_;
        }
        // Toggle mode
        if (frozen_) {
            ExitFreeze(cfg.cooldown_sec);
            return true;
        }
        if (!CanActivate(cfg, world_active)) {
            return false;
        }
        EnterFreeze();
        return true;
    }

    void SetHoldInput(bool held, const TimeStopConfig& cfg, bool world_active) {
        hold_pressed_ = held;
        if (!world_active) {
            ForceResume(false);
            return;
        }
        if (cfg.mode != TimeStopMode::Hold) {
            return;
        }
        if (held) {
            if (!frozen_ && CanActivate(cfg, world_active)) {
                EnterFreeze();
            }
        } else if (frozen_) {
            ExitFreeze(cfg.cooldown_sec);
        }
    }

    // Applies explicit UI/menu boolean desired state (works cleanly across both modes)
    void SetMenuEnabled(bool enable, const TimeStopConfig& cfg, bool world_active) {
        if (enable) {
            if (!frozen_ && CanActivate(cfg, world_active)) {
                EnterFreeze();
            }
        } else if (frozen_) {
            ExitFreeze(cfg.cooldown_sec);
        }
    }

    void ForceResume(bool apply_cooldown = false, float cooldown_sec = 0.0f) {
        hold_pressed_ = false;
        if (frozen_) {
            ExitFreeze(apply_cooldown ? cooldown_sec : 0.0f);
        }
        if (!apply_cooldown) {
            cooldown_remaining_ = 0.0f;
        }
    }

    void Update(float dt_sec, const TimeStopConfig& cfg, bool world_active) {
        const float safe_dt = SanitizeFreezeWorldDelta(dt_sec);

        if (!world_active) {
            ForceResume(false);
            return;
        }

        if (frozen_) {
            active_elapsed_ += safe_dt;
            if (cfg.mode == TimeStopMode::Hold && !hold_pressed_ && !menu_pinned_) {
                ExitFreeze(cfg.cooldown_sec);
                return;
            }
            if (cfg.max_duration_sec > 0.0f && active_elapsed_ >= cfg.max_duration_sec) {
                ExitFreeze(cfg.cooldown_sec);
            }
            return;
        }

        if (cooldown_remaining_ > 0.0f) {
            cooldown_remaining_ = std::max(0.0f, cooldown_remaining_ - safe_dt);
        }
    }

    void SetMenuPinned(bool pinned) { menu_pinned_ = pinned; }
    bool IsMenuPinned() const { return menu_pinned_; }

private:
    void EnterFreeze() {
        frozen_ = true;
        active_elapsed_ = 0.0f;
    }

    void ExitFreeze(float cooldown_sec) {
        frozen_ = false;
        active_elapsed_ = 0.0f;
        menu_pinned_ = false;
        cooldown_remaining_ = std::max(0.0f, cooldown_sec);
    }

    bool frozen_ = false;
    bool hold_pressed_ = false;
    bool menu_pinned_ = false;
    float active_elapsed_ = 0.0f;
    float cooldown_remaining_ = 0.0f;
};

}  // namespace common
