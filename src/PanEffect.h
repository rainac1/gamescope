#pragma once

#include <algorithm>
#include <cmath>
#include <string>

#include <xkbcommon/xkbcommon.h>

#include "convar.h"

// Gamescope screen pan effect.
//
// While the configured key is held, relative mouse motion is accumulated as
// (delta_x, delta_y) counts and the game's frame is translated in output pixels,
// right before it is handed to the backend for presentation.
//
//     theta_x = k * delta_x        (pan yaw, accumulated while the key is held)
//     theta_y = k * delta_y        (pan pitch, accumulated while the key is held)
//     phi     = phi_0 + theta_y    (engine pitch now, phi_0 was the pitch the
//                                   engine was at when the key went down)
//
//     dX = f * (sin(theta_x) / cos(phi)) / (cos(theta_x) + tan(phi_0) * tan(phi))
//     dY = f * (tan(phi) * cos(theta_x) - tan(phi_0)) / (cos(theta_x) + tan(phi_0) * tan(phi))
//
// 'k' is a configurable angle per mouse count, 0.022 degrees by default, and
// 'f' a configurable focal length in pixels, 720 by default. The engine pitch is
// tracked at all times, key held or not, because the projection is relative to
// where the camera was pitched when the key went down, and it saturates at
// pan_effect_pitch_limit (89 degrees by default) exactly like a game's own pitch
// clamp does. Holding the mouse against that limit is therefore also how the
// tracked pitch can be resynchronised with the game by hand.
//
// The translation is snapped to whole source texels (see PanEffectSnapToTexels)
// so that the picture is moved without being resampled. Pixels pushed outside of
// the output are cropped and the vacated space is left black. Releasing the key
// clears the accumulated motion, which puts the image back where it was.
namespace gamescope
{
    // tan() has a pole at +-90 degrees, so angles are clamped just short of it.
    // The resulting translation is off screen long before that anyway.
    inline constexpr double k_flPanEffectPi = 3.14159265358979323846;
    inline constexpr double k_flPanEffectMaxTheta = 89.0 * k_flPanEffectPi / 180.0;
    // Hard cap for the configurable engine pitch limit, close enough to 90 that
    // tan() is still huge but finite.
    inline constexpr double k_flPanEffectMaxPitch = 89.9 * k_flPanEffectPi / 180.0;
    // The projection's denominator can approach zero (the two pitches small and
    // opposite, yaw near 90 degrees). Keep the result finite and let the caller's
    // clamp saturate it instead.
    inline constexpr double k_flPanEffectMinDenominator = 1e-3;

    extern ConVar<bool> cv_pan_effect_enabled;
    extern ConVar<std::string> cv_pan_effect_key;
    extern ConVar<float> cv_pan_effect_angle;
    extern ConVar<float> cv_pan_effect_focal;
    extern ConVar<float> cv_pan_effect_pitch_limit;
    extern ConVar<bool> cv_pan_effect_invert;

    // How far the yaw may go for a given pair of pitches. The projection's
    // denominator vanishes where cos(theta_x) == -tan(phi_0) * tan(phi): beyond that
    // point the projection flips sign, so the yaw is kept on the near side of it.
    // With the two pitches of the same sign, or either of them large, there is no
    // such point inside the allowed range and the plain limit applies.
    inline double PanEffectMaxYawForPitch( double flPhi0, double flPhi )
    {
        const double flPoleCos = -std::tan( std::clamp( flPhi0, -k_flPanEffectMaxPitch, k_flPanEffectMaxPitch ) ) *
                                  std::tan( std::clamp( flPhi, -k_flPanEffectMaxPitch, k_flPanEffectMaxPitch ) );

        // cos(theta_x) == flPoleCos only has a solution in [0, 90) for flPoleCos in
        // (0, 1]; anything else means the pole is outside the allowed range.
        if ( flPoleCos > 0.0 && flPoleCos <= 1.0 )
        {
            const double flPoleYaw = std::acos( flPoleCos );
            if ( flPoleYaw < k_flPanEffectMaxTheta )
                return flPoleYaw;
        }

        return k_flPanEffectMaxTheta;
    }

    // Side effect free projection helper, so the math can be unit tested. flThetaX
    // and flPhi0/flPhi are in radians, and flPhi is the engine pitch now, which is
    // flPhi0 + theta_y when the pitch has been tracked all along.
    inline void PanEffectComputeTranslation( double flThetaX, double flPhi0, double flPhi, double flFocalPx, double &flX, double &flY )
    {
        const double flClampedPhi0 = std::clamp( flPhi0, -k_flPanEffectMaxPitch, k_flPanEffectMaxPitch );
        const double flClampedPhi = std::clamp( flPhi, -k_flPanEffectMaxPitch, k_flPanEffectMaxPitch );
        const double flMaxYaw = PanEffectMaxYawForPitch( flClampedPhi0, flClampedPhi );
        const double flClampedThetaX = std::clamp( flThetaX, -flMaxYaw, flMaxYaw );

        const double flTanPhi0 = std::tan( flClampedPhi0 );
        const double flTanPhi = std::tan( flClampedPhi );
        const double flCosThetaX = std::cos( flClampedThetaX );

        double flDenominator = flCosThetaX + flTanPhi0 * flTanPhi;
        if ( std::abs( flDenominator ) < k_flPanEffectMinDenominator )
            flDenominator = std::copysign( k_flPanEffectMinDenominator, flDenominator );

        flX = flFocalPx * ( std::sin( flClampedThetaX ) / std::cos( flClampedPhi ) ) / flDenominator;
        flY = flFocalPx * ( flTanPhi * flCosThetaX - flTanPhi0 ) / flDenominator;
    }

    // Track the engine pitch: add one pan pitch step and saturate at the configured
    // limit, like a game's own pitch clamp does.
    inline double PanEffectTrackPitch( double flPitchRad, double flDeltaRad, double flLimitRad )
    {
        const double flLimit = std::clamp( flLimitRad, 0.0, k_flPanEffectMaxPitch );
        return std::clamp( flPitchRad + flDeltaRad, -flLimit, flLimit );
    }

    // The largest yaw magnitude on one side whose projected translation, after the
    // invert convar (flSign), still lies within [flMinX, flMaxX]. Walks outwards and
    // then bisects, rather than root finding, because the projection is not
    // guaranteed to be monotone once the pan pitch has travelled far. Only used to
    // stop accumulating yaw that is already past what can be shown, so a coarse
    // answer is fine.
    inline double PanEffectYawLimitForBound( double flMinX, double flMaxX, double flPhi0, double flPhi, double flFocalPx, double flSign, bool bPositiveYaw )
    {
        const double flMaxYaw = PanEffectMaxYawForPitch( flPhi0, flPhi );
        const int nSamples = 64;

        auto bInBounds = [&]( double flMagnitude ) -> bool
        {
            double flX = 0.0, flY = 0.0;
            PanEffectComputeTranslation( bPositiveYaw ? flMagnitude : -flMagnitude, flPhi0, flPhi, flFocalPx, flX, flY );
            const double flFinalX = flSign * flX;
            return std::isfinite( flFinalX ) && flFinalX >= flMinX && flFinalX <= flMaxX;
        };

        double flLastGood = 0.0;
        for ( int i = 1; i <= nSamples; i++ )
        {
            const double flYaw = flMaxYaw * double( i ) / double( nSamples );
            if ( !bInBounds( flYaw ) )
            {
                double flLow = flLastGood, flHigh = flYaw;
                for ( int j = 0; j < 24; j++ )
                {
                    const double flMid = 0.5 * ( flLow + flHigh );
                    if ( bInBounds( flMid ) )
                        flLow = flMid;
                    else
                        flHigh = flMid;
                }
                return flLow;
            }

            flLastGood = flYaw;
        }

        return flLastGood;
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

    // Accumulate relative pointer motion. Called with the wlserver lock held. The
    // engine pitch always follows the y motion, the pan yaw only accumulates while
    // the pan key is held. Returns true when the held pan moved, ie. when the frame
    // needs to be redrawn for it.
    bool PanEffectAccumulateMotion( double flDx, double flDy );

    // Current translation in output pixels. Callable from any thread. Returns false
    // when nothing should be applied (key not held, the held pan has not moved yet,
    // or the effect is disabled). Once it has returned true for a hold it keeps
    // returning true until the key is released, so that the presentation layout can
    // stay put even on the frames where the translation lands back on zero.
    bool PanEffectGetTranslation( double &flX, double &flY );

    // Clamp the accumulated pan yaw so that the translation stays within
    // [flMinX, flMaxX] output pixels. The compositor calls this every frame with the
    // bounds it applies to the translation, so that pushing the mouse past a limit
    // does not keep accumulating yaw that would have to be undone again before the
    // picture starts moving back. Callable from any thread. The pitch is tracked,
    // not accumulated for the pan, so it needs no equivalent.
    void PanEffectLimitTranslation( double flMinX, double flMaxX );

    // Forget the held key and the accumulated motion, eg. when the session
    // pauses and we can no longer see the matching release.
    void PanEffectReset();
}
