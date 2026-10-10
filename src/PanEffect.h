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
// 'f' a configurable focal length in pixels, 720 by default. The translation is
// snapped to whole source texels (see PanEffectSnapToTexels) so that the picture
// is moved without being resampled. Pixels pushed outside of the output are
// cropped and the vacated space is left black. Releasing the key clears the
// accumulated motion, which puts the image back where it was.
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

    // Side effect free inverse of PanEffectComputeTranslation, so the math can be
    // unit tested. Clamped to the same angle as the forward direction, so a
    // translation that the projection could never produce maps to the end of its
    // range instead of growing without bound.
    inline double PanEffectCountsForTranslation( double flTranslationPx, double flRadiansPerCount, double flFocalPx )
    {
        if ( flRadiansPerCount <= 0.0 || flFocalPx <= 0.0 )
            return 0.0;

        const double flTheta = std::clamp( std::atan( flTranslationPx / flFocalPx ), -k_flPanEffectMaxTheta, k_flPanEffectMaxTheta );
        return flTheta / flRadiansPerCount;
    }

    // Snap a translation in output pixels to whole source texels, so that the
    // image is moved without being resampled.
    //
    // The layer is sampled at (output_pixel + offset) * scale + 0.5 in texture
    // space, where 'scale' is texels per output pixel. Making offset * scale a
    // whole number of texels leaves every sample at exactly the texel phase it
    // has without the pan, which means no interpolation blur is introduced and a
    // 1:1 layer keeps using the plain (cheapest) sampler path. Snapping to whole
    // output pixels instead would still resample a scaled layer, and snapping
    // nowhere (a fractional offset) blurs everything.
    inline double PanEffectSnapToTexels( double flTranslationPx, double flTexelsPerPx )
    {
        if ( flTexelsPerPx <= 0.0 )
            return flTranslationPx;

        return std::round( flTranslationPx * flTexelsPerPx ) / flTexelsPerPx;
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

    // Clamp the accumulated motion so that the translation stays within
    // [flMinX, flMaxX] x [flMinY, flMaxY] output pixels. The compositor calls this
    // every frame with the bounds it applies to the translation, so that pushing
    // the mouse past a limit does not keep accumulating motion that would have to
    // be undone again before the picture starts moving back. Callable from any
    // thread.
    void PanEffectLimitTranslation( double flMinX, double flMaxX, double flMinY, double flMaxY );

    // Forget the held key and the accumulated motion, eg. when the session
    // pauses and we can no longer see the matching release.
    void PanEffectReset();
}
