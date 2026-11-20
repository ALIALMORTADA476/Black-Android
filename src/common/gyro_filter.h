#pragma once

#include <cstdint>

namespace gyro {

struct OneEuroParams {
    float min_cutoff = 1.6f;       // Hz cutoff when hand is steady (filters micro-tremor)
    float beta = 0.22f;            // Speed-adaptive cutoff slope (eliminates flick lag)
    float d_cutoff = 1.0f;         // Hz cutoff for derivative speed estimation
    float bias_adapt_rate = 0.08f; // Per-second EWMA rate for stationary MEMS zero-bias
    float bias_threshold = 0.032f; // rad/s rest threshold for stationary drift learning
    float deadzone = 0.008f;       // Post-bias micro-deadzone (rad/s)
    float max_bias = 0.12f;        // Safety clamp on learned hardware drift offset
};

// Adaptive One Euro Filter (Casiez et al.) with stationary zero-rate bias estimation.
// Attenuates resting hand tremor jitter cleanly while preserving near-zero latency on flicks.
class OneEuroGyroFilter {
public:
    explicit OneEuroGyroFilter(const OneEuroParams& params = OneEuroParams{});

    void Reset();
    void Update(float raw_yaw, float raw_pitch, float dt,
                float& out_yaw, float& out_pitch);

    float bias_yaw() const { return bias_yaw_; }
    float bias_pitch() const { return bias_pitch_; }

private:
    OneEuroParams params_{};
    bool initialized_ = false;
    float bias_yaw_ = 0.0f;
    float bias_pitch_ = 0.0f;
    float x_hat_yaw_ = 0.0f;
    float x_hat_pitch_ = 0.0f;
    float dx_hat_yaw_ = 0.0f;
    float dx_hat_pitch_ = 0.0f;
};

// Mode gate: 0=Off, 1=Always On, 2=Firing Only, 3=ADS/Scoped Only
bool ShouldRunForMode(uint32_t mode, bool is_ads, bool is_firing, bool look_active) noexcept;

}  // namespace gyro
