#include "common/gyro_filter.h"

#include <algorithm>
#include <cmath>

namespace gyro {

namespace {

constexpr float kTwoPi = 6.28318530718f;

float SmoothingAlpha(float cutoff_hz, float dt) {
    if (cutoff_hz <= 1e-4f || dt <= 0.0f) return 1.0f;
    const float tau = 1.0f / (kTwoPi * cutoff_hz);
    return 1.0f / (1.0f + tau / dt);
}

float ApplyMicroDeadzone(float v, float deadzone) {
    const float av = std::fabs(v);
    if (av <= deadzone) return 0.0f;
    // Soft-knee continuity past deadzone avoids jerky step transitions on tiny tracking edits.
    const float knee = std::min(av - deadzone, deadzone);
    return std::copysign((av - deadzone) + 0.5f * knee * (av / (deadzone * 2.0f)), v);
}

void FilterAxis(float target, float dt, const OneEuroParams& p,
                float& x_hat, float& dx_hat) {
    const float dx = (target - x_hat) / dt;
    const float alpha_d = SmoothingAlpha(p.d_cutoff, dt);
    dx_hat += alpha_d * (dx - dx_hat);
    const float cutoff = p.min_cutoff + p.beta * std::fabs(dx_hat);
    const float alpha = SmoothingAlpha(cutoff, dt);
    x_hat += alpha * (target - x_hat);
}

}  // namespace

OneEuroGyroFilter::OneEuroGyroFilter(const OneEuroParams& params) : params_(params) {}

void OneEuroGyroFilter::Reset() {
    initialized_ = false;
    bias_yaw_ = 0.0f;
    bias_pitch_ = 0.0f;
    x_hat_yaw_ = 0.0f;
    x_hat_pitch_ = 0.0f;
    dx_hat_yaw_ = 0.0f;
    dx_hat_pitch_ = 0.0f;
}

void OneEuroGyroFilter::Update(float raw_yaw, float raw_pitch, float dt,
                               float& out_yaw, float& out_pitch) {
    if (!std::isfinite(raw_yaw) || !std::isfinite(raw_pitch) || !std::isfinite(dt) || dt <= 0.0f) {
        out_yaw = 0.0f;
        out_pitch = 0.0f;
        return;
    }

    const float c_dt = std::clamp(dt, 1e-4f, 0.1f);

    // Learn static hardware offset only while sensor angular rate is genuinely near rest.
    const float dy_rest = raw_yaw - bias_yaw_;
    const float dp_rest = raw_pitch - bias_pitch_;
    if (std::fabs(dy_rest) < params_.bias_threshold &&
        std::fabs(dp_rest) < params_.bias_threshold) {
        const float bias_alpha = std::clamp(params_.bias_adapt_rate * c_dt, 0.0f, 0.25f);
        bias_yaw_ = std::clamp(bias_yaw_ + bias_alpha * dy_rest, -params_.max_bias, params_.max_bias);
        bias_pitch_ = std::clamp(bias_pitch_ + bias_alpha * dp_rest, -params_.max_bias, params_.max_bias);
    }

    const float clean_yaw = raw_yaw - bias_yaw_;
    const float clean_pitch = raw_pitch - bias_pitch_;

    if (!initialized_) {
        initialized_ = true;
        x_hat_yaw_ = clean_yaw;
        x_hat_pitch_ = clean_pitch;
        dx_hat_yaw_ = 0.0f;
        dx_hat_pitch_ = 0.0f;
    } else {
        FilterAxis(clean_yaw, c_dt, params_, x_hat_yaw_, dx_hat_yaw_);
        FilterAxis(clean_pitch, c_dt, params_, x_hat_pitch_, dx_hat_pitch_);
    }

    out_yaw = ApplyMicroDeadzone(x_hat_yaw_, params_.deadzone);
    out_pitch = ApplyMicroDeadzone(x_hat_pitch_, params_.deadzone);
}

}  // namespace gyro
