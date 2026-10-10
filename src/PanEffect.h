#pragma once

#include <algorithm>
#include <cmath>
#include <string>

#include <xkbcommon/xkbcommon.h>

#include "convar.h"

// Gamescope screen pan effect.
//
// While the configured key is held, relative mouse motion is accumulated as
// (delta_x, delta_y) counts and the game's frame is translated in output
// pixels, right before it is handed to the backend for presentation:
//
//     theta_x = k * delta_x
//     theta_y = k * delta_y
//     trans_X = f * tan(theta_x)
//     trans_Y = f * tan(theta_y)
//
// 'k' is a configurable angle per mouse count, 0.022 degrees by default, and
// 'f' a configurable focal length in pixels, 720 by default. Pixels pushed
// outside of the output are cropped and the vacated space is left black.
// Releasing the key clears the accumulated motion, which puts the image back
// where it was.
namespace gamescope
{
    // tan() has a pole at +-90 degrees, so the angle is clamped just short of
    // it. The resulting translation is off screen long before that anyway.
    inline constexpr double k_flPanEffectMaxTheta = 1.5533430342749532; // ~89 degrees
    inline constexpr double k_flPanEffectPi = 3.14159265358979323846;

    extern ConVar<bool> cv_pan_effect_enabled;
    extern ConVar<std::string> cv_pan_effect_key;
    extern ConVar<float> cv_pan_effect_angle;
    extern ConVar<float> cv_pan_effect_focal;
    extern ConVar<bool> cv_pan_effect_invert;

    // Side effect free projection helper, so the math can be unit tested.
    inline double PanEffectComputeTranslation( double flDeltaCounts, double flRadiansPerCount, double flFocalPx )
    {
        const double flTheta = std::clamp( flDeltaCounts * flRadiansPerCount, -k_flPanEffectMaxTheta, k_flPanEffectMaxTheta );
        return flFocalPx * std::tan( flTheta );
    }

    // Observe a keyboard event. Called with the wlserver lock held.
    void PanEffectObserveKey( xkb_keysym_t eKeysym, bool bPressed );

    // Accumulate relative pointer motion while the pan key is held. Called with
    // the wlserver lock held. Returns true when the motion was accumulated.
    bool PanEffectAccumulateMotion( double flDx, double flDy );

    // Current translation in output pixels. Callable from any thread. Returns
    // false when nothing should be applied (key not held, no motion
    // accumulated for this hold, or the effect is disabled).
    bool PanEffectGetTranslation( double &flX, double &flY );

    // Forget the held key and the accumulated motion, eg. when the session
    // pauses and we can no longer see the matching release.
    void PanEffectReset();
}
