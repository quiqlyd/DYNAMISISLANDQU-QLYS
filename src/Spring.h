#pragma once
#include <cmath>

// Kritik altı sönümlü yay: hedefe hafif bir "zıplama" ile oturur.
struct Spring {
    float value = 0, velocity = 0, target = 0;
    float stiffness = 320.f;
    float damping = 24.f;

    void Snap(float v) { value = target = v; velocity = 0; }

    // true dönerse hâlâ hareket ediyor.
    bool Step(float dt) {
        float force = (target - value) * stiffness - velocity * damping;
        velocity += force * dt;
        value += velocity * dt;
        if (std::fabs(target - value) < 0.25f && std::fabs(velocity) < 0.5f) {
            value = target;
            velocity = 0;
            return false;
        }
        return true;
    }
};
