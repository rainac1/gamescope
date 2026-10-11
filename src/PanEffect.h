#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <xkbcommon/xkbcommon.h>

#include "convar.h"

// Gamescope screen pan effect.
//
// While a configured trigger is held, relative mouse motion is accumulated as
// (delta_x, delta_y) counts and the game's frame is translated in output pixels,
// right before it is handed to the backend for presentation. A trigger is either
// a keyboard key or a mouse button (see PanEffectTrigger), so eg. the right
// button or a side button can drive the pan.
//
//     theta_x = k * delta_x        (pan yaw, accumulated while a trigger is held)
//     theta_y = k * delta_y        (pan pitch, accumulated while a trigger is held)
//     phi     = phi_0 + theta_y    (engine pitch now, phi_0 was the pitch the
//                                   engine was at when the trigger went down)
//
//     dX = f * (sin(theta_x) / cos(phi)) / (cos(theta_x) + tan(phi_0) * tan(phi))
//     dY = f * (tan(phi) * cos(theta_x) - tan(phi_0)) / (cos(theta_x) + tan(phi_0) * tan(phi))
//
// 'k' is a configurable angle per mouse count, 0.022 degrees by default, and
// 'f' a configurable focal length in pixels, 720 by default. The engine pitch is
// tracked at all times, trigger held or not, because the projection is relative to
// where the camera was pitched when the trigger went down, and it saturates at
// pan_effect_pitch_limit (89 degrees by default) exactly like a game's own pitch
// clamp does. Holding the mouse against that limit is therefore also how the
// tracked pitch can be resynchronised with the game by hand.
//
// The translation is snapped to whole source texels (see PanEffectSnapToTexels)
// so that the picture is moved without being resampled. Pixels pushed outside of
// the output are cropped and the vacated space is left black. Releasing the
// trigger clears the accumulated motion, which puts the image back where it was.
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

    // The pan yaw that projects to flTranslationPx: the direct inverse of
    // PanEffectComputeTranslation, no search. Writing the projection as
    //
    //     flTranslationPx * cos(phi) * cos(theta) - f * sin(theta)
    //         == -flTranslationPx * sin(phi) * tan(phi_0)
    //
    // makes it a * cos(theta) + b * sin(theta) == c, so the yaw is
    // atan2(b, a) + acos(c / hypot(a, b)). The projection is odd in the yaw, so
    // flTranslationPx must be positive.
    //
    // Returns 0 when the projection cannot reach the target for a yaw on that side,
    // which includes the pitch pairs whose denominator comes out negative (the tracked
    // point has gone behind the camera, so the projection has no meaning there): the
    // pan then simply does not move. Only used to stop accumulating yaw that is
    // already past what can be shown.
    inline double PanEffectYawForTranslation( double flTranslationPx, double flPhi0, double flPhi, double flFocalPx )
    {
        if ( flFocalPx <= 0.0 || flTranslationPx <= 0.0 )
            return 0.0;

        const double flClampedPhi0 = std::clamp( flPhi0, -k_flPanEffectMaxPitch, k_flPanEffectMaxPitch );
        const double flClampedPhi = std::clamp( flPhi, -k_flPanEffectMaxPitch, k_flPanEffectMaxPitch );

        const double flA = flTranslationPx * std::cos( flClampedPhi );
        const double flB = -flFocalPx;
        const double flC = -flTranslationPx * std::sin( flClampedPhi ) * std::tan( flClampedPhi0 );

        const double flR = std::hypot( flA, flB );
        if ( flR <= 0.0 )
            return 0.0;

        const double flYaw = std::atan2( flB, flA ) + std::acos( std::clamp( flC / flR, -1.0, 1.0 ) );
        return std::clamp( flYaw, 0.0, PanEffectMaxYawForPitch( flClampedPhi0, flClampedPhi ) );
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

    // What can drive the pan: a keyboard key (an xkb_keysym_t) or a mouse button
    // (a Linux input event code, BTN_LEFT and friends). The two live in separate
    // namespaces, so the kind is part of the identity.
    struct PanEffectTrigger
    {
        enum class Kind : uint8_t
        {
            Key,
            Button,
        };

        Kind eKind = Kind::Key;
        uint32_t uCode = 0;

        bool operator==( const PanEffectTrigger & ) const = default;

        // Identity as a single integer, so a set of triggers needs no custom hash.
        constexpr uint64_t Packed() const
        {
            return ( uint64_t( eKind ) << 32 ) | uCode;
        }
    };

    // Parse a pan_effect_key value: a comma separated list of keysym names or
    // mouse button names. A name that is neither is appended to vsUnknown. Exposed
    // for the unit tests.
    std::vector<PanEffectTrigger> PanEffectParseTriggers( std::string_view svConfig, std::vector<std::string> &vsUnknown );

    // Observe a keyboard event. Called with the wlserver lock held.
    void PanEffectObserveKey( xkb_keysym_t eKeysym, bool bPressed );

    // Observe a mouse button event (a Linux input event code, BTN_RIGHT and
    // friends). Called with the wlserver lock held. Like the key, the button is
    // observed and never consumed: the game still sees it.
    void PanEffectObserveButton( uint32_t uButton, bool bPressed );

    // Accumulate relative pointer motion. Called with the wlserver lock held. The
    // engine pitch always follows the y motion, the pan yaw only accumulates while
    // a pan trigger is held. Returns true when the held pan moved, ie. when the
    // frame needs to be redrawn for it.
    bool PanEffectAccumulateMotion( double flDx, double flDy );

    // Current translation in output pixels. Callable from any thread. Returns false
    // when nothing should be applied (no trigger held, the held pan has not moved
    // yet, or the effect is disabled). Once it has returned true for a hold it keeps
    // returning true until every trigger is released, so that the presentation
    // layout can stay put even on the frames where the translation lands back on
    // zero.
    bool PanEffectGetTranslation( double &flX, double &flY );

    // Clamp the accumulated pan yaw so that the translation stays within
    // [flMinX, flMaxX] output pixels. The compositor calls this every frame with the
    // bounds it applies to the translation, so that pushing the mouse past a limit
    // does not keep accumulating yaw that would have to be undone again before the
    // picture starts moving back. Callable from any thread. The pitch is tracked,
    // not accumulated for the pan, so it needs no equivalent.
    void PanEffectLimitTranslation( double flMinX, double flMaxX );

    // Forget the held triggers and the accumulated motion, eg. when the session
    // pauses and we can no longer see the matching release.
    void PanEffectReset();
}
