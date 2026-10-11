#include "PanEffect.h"

#include <atomic>
#include <cctype>
#include <format>
#include <mutex>
#include <string_view>
#include <unordered_set>

#include <xkbcommon/xkbcommon-keysyms.h>

#include "log.hpp"
#include "Utils/String.h"

namespace gamescope
{
    static LogScope s_PanEffectLog( "pan_effect" );

    ConVar<bool> cv_pan_effect_enabled( "pan_effect_enabled", true,
        "Enable the screen pan effect. Hold pan_effect_key and move the mouse to shift the game's frame." );
    ConVar<std::string> cv_pan_effect_key( "pan_effect_key", "Alt",
        "xkb keysym name, or a comma separated list of them, that drives the screen pan effect while held. Default: Alt" );
    ConVar<float> cv_pan_effect_angle( "pan_effect_angle", 0.022f,
        "Pan angle in degrees per accumulated mouse count. Default: 0.022" );
    ConVar<float> cv_pan_effect_focal( "pan_effect_focal", 720.0f,
        "Focal length in pixels used to project the pan angle onto the screen. Default: 720" );
    ConVar<float> cv_pan_effect_pitch_limit( "pan_effect_pitch_limit", 89.0f,
        "Engine pitch limit in degrees. The tracked pitch saturates here, the same way a game's own pitch clamp does, "
        "which is also how it can be resynchronised by hand. Default: 89" );
    ConVar<bool> cv_pan_effect_invert( "pan_effect_invert", false,
        "Invert the pan direction." );
    static ConVar<bool> cv_pan_effect_debug( "pan_effect_debug", false,
        "Print the pan counts, the tracked pitch and the resulting translation on every press and release. "
        "A convar of its own because the gamescope_* environment variable override cannot raise a log channel." );

    // Written and read under the wlserver lock, except for the readers on the
    // steamcompmgr thread, which just want the most recent value.
    static std::atomic<bool> s_bPanKeyHeld{ false };
    // Set once the held pan has moved, and kept until the key is released: the
    // presentation layout follows this rather than the translation, which can come
    // back to zero in the middle of a hold.
    static std::atomic<bool> s_bPanEngaged{ false };
    // Pan yaw, in accumulated mouse counts, only while the key is held.
    static std::atomic<double> s_flPanYawCounts{ 0.0 };
    // Engine pitch, in radians, tracked at all times.
    static std::atomic<double> s_flPitchRad{ 0.0 };
    // Engine pitch when the key went down, phi_0 in the projection.
    static std::atomic<double> s_flPitchAtPressRad{ 0.0 };

    // A single event larger than this is a cursor jump (focus change, mode
    // switch), not pointer movement, so it is dropped.
    static constexpr double k_flPanEffectMaxStep = 4096.0;

    static std::mutex s_KeyMutex;
    static std::unordered_set<xkb_keysym_t> s_PanKeysyms;
    static std::string s_CachedKeyConfig;

    static xkb_keysym_t PanEffectResolveKeysym( std::string_view svKey )
    {
        return xkb_keysym_from_name( std::string( svKey ).c_str(), XKB_KEYSYM_CASE_INSENSITIVE );
    }

    // A couple of spellings people reach for that xkb does not know itself.
    static std::string_view PanEffectKeyAlias( std::string_view svKey )
    {
        std::string sLower{ svKey };
        for ( char &c : sLower )
            c = char( std::tolower( (unsigned char) c ) );

        if ( sLower == "ctrl" )
            return "Control";
        if ( sLower == "win" || sLower == "windows" )
            return "Super";

        return svKey;
    }

    // Add one configured key name. "Alt" and friends have no keysym of their
    // own, so they expand to the left/right pair.
    static bool PanEffectAddKey( std::string_view svKey )
    {
        xkb_keysym_t eKeysym = PanEffectResolveKeysym( svKey );
        if ( eKeysym != XKB_KEY_NoSymbol )
        {
            s_PanKeysyms.insert( eKeysym );

            // Either side of a modifier pair drives the effect.
            if ( eKeysym == XKB_KEY_Alt_L )
                s_PanKeysyms.insert( XKB_KEY_Alt_R );
            else if ( eKeysym == XKB_KEY_Alt_R )
                s_PanKeysyms.insert( XKB_KEY_Alt_L );

            return true;
        }

        const std::string sLeft = std::string( svKey ) + "_L";
        const xkb_keysym_t eLeft = PanEffectResolveKeysym( sLeft );
        if ( eLeft != XKB_KEY_NoSymbol )
        {
            s_PanKeysyms.insert( eLeft );

            const std::string sRight = std::string( svKey ) + "_R";
            const xkb_keysym_t eRight = PanEffectResolveKeysym( sRight );
            if ( eRight != XKB_KEY_NoSymbol )
                s_PanKeysyms.insert( eRight );

            return true;
        }

        return false;
    }

    static void PanEffectRebuildKeyList()
    {
        std::lock_guard<std::mutex> lock( s_KeyMutex );

        const std::string sConfig{ cv_pan_effect_key.Get() };
        if ( sConfig == s_CachedKeyConfig )
            return;

        s_CachedKeyConfig = sConfig;
        s_PanKeysyms.clear();

        for ( std::string_view svToken : Split( sConfig, "," ) )
        {
            // Be forgiving about "Alt, Super_L" style lists.
            const size_t nStart = svToken.find_first_not_of( " \t" );
            if ( nStart == std::string_view::npos )
                continue;

            const size_t nEnd = svToken.find_last_not_of( " \t" );
            const std::string_view svKey = svToken.substr( nStart, nEnd - nStart + 1 );

            if ( !PanEffectAddKey( PanEffectKeyAlias( svKey ) ) )
                s_PanEffectLog.errorf( "pan_effect_key: unknown key \"%.*s\".", int( svKey.length() ), svKey.data() );
        }

        if ( s_PanKeysyms.empty() )
            s_PanEffectLog.errorf( "pan_effect_key: no usable keys in \"%s\", the pan effect is inert.", sConfig.c_str() );
    }

    static bool PanEffectIsPanKey( xkb_keysym_t eKeysym )
    {
        std::lock_guard<std::mutex> lock( s_KeyMutex );
        return s_PanKeysyms.contains( eKeysym );
    }

    static void PanEffectComputeCurrentTranslation( double &flX, double &flY )
    {
        const double flRadiansPerCount = double( cv_pan_effect_angle.Get() ) * ( k_flPanEffectPi / 180.0 );
        const double flFocalPx = double( cv_pan_effect_focal.Get() );
        const double flSign = cv_pan_effect_invert.Get() ? -1.0 : 1.0;

        const double flThetaX = s_flPanYawCounts.load( std::memory_order_relaxed ) * flRadiansPerCount;
        const double flPhi0 = s_flPitchAtPressRad.load( std::memory_order_relaxed );
        const double flPhi = s_flPitchRad.load( std::memory_order_relaxed );

        PanEffectComputeTranslation( flThetaX, flPhi0, flPhi, flFocalPx, flX, flY );
        flX *= flSign;
        flY *= flSign;
    }

    static double PanEffectGetPitchLimitRad()
    {
        return double( cv_pan_effect_pitch_limit.Get() ) * ( k_flPanEffectPi / 180.0 );
    }

    static bool PanEffectIsActive()
    {
        // s_bPanEngaged is what drives the presentation layout: it goes true as soon as
        // the held pan moves and stays true until the key is released, even if the
        // translation comes back to zero in between.
        return cv_pan_effect_enabled.Get() &&
            s_bPanKeyHeld.load( std::memory_order_relaxed ) &&
            s_bPanEngaged.load( std::memory_order_relaxed );
    }

    void PanEffectReset()
    {
        s_bPanKeyHeld.store( false, std::memory_order_relaxed );
        s_bPanEngaged.store( false, std::memory_order_relaxed );
        s_flPanYawCounts.store( 0.0, std::memory_order_relaxed );
        // The tracked pitch is deliberately left alone: it mirrors the game's camera,
        // which does not recentre just because the session paused.
    }

    void PanEffectObserveKey( xkb_keysym_t eKeysym, bool bPressed )
    {
        if ( !cv_pan_effect_enabled.Get() )
        {
            // Don't leave a hold behind if the effect was switched off mid-press.
            PanEffectReset();
            return;
        }

        PanEffectRebuildKeyList();

        if ( !PanEffectIsPanKey( eKeysym ) )
            return;

        if ( bPressed )
        {
            if ( !s_bPanKeyHeld.exchange( true, std::memory_order_relaxed ) )
            {
                // A fresh hold starts from the centre again, relative to the pitch the
                // engine (the game's camera) is at right now.
                s_flPanYawCounts.store( 0.0, std::memory_order_relaxed );
                s_bPanEngaged.store( false, std::memory_order_relaxed );
                s_flPitchAtPressRad.store( s_flPitchRad.load( std::memory_order_relaxed ), std::memory_order_relaxed );
                s_PanEffectLog.log( cv_pan_effect_debug.Get() ? LOG_INFO : LOG_DEBUG,
                    std::format( "Pan hold started at pitch {:.1f} deg.",
                        s_flPitchAtPressRad.load( std::memory_order_relaxed ) * 180.0 / k_flPanEffectPi ) );
            }
            return;
        }

        if ( s_bPanKeyHeld.exchange( false, std::memory_order_relaxed ) )
        {
            // Log the numbers so the angle/focal length can be tuned against a
            // known mouse movement instead of guessing.
            const double flYawCounts = s_flPanYawCounts.load( std::memory_order_relaxed );
            const double flPitchDeg = s_flPitchRad.load( std::memory_order_relaxed ) * 180.0 / k_flPanEffectPi;
            const double flPitchAtPressDeg = s_flPitchAtPressRad.load( std::memory_order_relaxed ) * 180.0 / k_flPanEffectPi;
            double flX = 0.0, flY = 0.0;
            PanEffectComputeCurrentTranslation( flX, flY );

            s_PanEffectLog.log( cv_pan_effect_debug.Get() ? LOG_INFO : LOG_DEBUG,
                std::format(
                    "Pan hold released, recentring. yaw={:.1f} counts -> trans=({:.1f}, {:.1f}) px "
                    "(pitch {:.1f} -> {:.1f} deg, angle={:.4f} deg/count, focal={:.1f} px)",
                    flYawCounts, flX, flY, flPitchAtPressDeg, flPitchDeg,
                    double( cv_pan_effect_angle.Get() ), double( cv_pan_effect_focal.Get() ) ) );
        }

        s_flPanYawCounts.store( 0.0, std::memory_order_relaxed );
        s_bPanEngaged.store( false, std::memory_order_relaxed );
    }

    bool PanEffectAccumulateMotion( double flDx, double flDy )
    {
        if ( !cv_pan_effect_enabled.Get() )
            return false;

        // A single event larger than this is a cursor jump (focus change, mode
        // switch), not pointer movement, so it is dropped.
        if ( std::abs( flDx ) > k_flPanEffectMaxStep || std::abs( flDy ) > k_flPanEffectMaxStep )
            return false;

        const double flRadiansPerCount = double( cv_pan_effect_angle.Get() ) * ( k_flPanEffectPi / 180.0 );

        // The engine pitch follows y motion at all times, held or not, so that the
        // pitch the pan is relative to is where the game's camera actually is. Only
        // the input thread writes it, under the wlserver lock.
        s_flPitchRad.store(
            PanEffectTrackPitch( s_flPitchRad.load( std::memory_order_relaxed ), flDy * flRadiansPerCount, PanEffectGetPitchLimitRad() ),
            std::memory_order_relaxed );

        if ( !s_bPanKeyHeld.load( std::memory_order_relaxed ) )
            return false;

        // The pan yaw accumulates while the key is held. The pan pitch is the tracked
        // pitch itself, so the y motion is not accumulated a second time.
        s_flPanYawCounts.store( s_flPanYawCounts.load( std::memory_order_relaxed ) + flDx, std::memory_order_relaxed );
        s_bPanEngaged.store( true, std::memory_order_relaxed );

        return flDx != 0.0 || flDy != 0.0;
    }

    bool PanEffectGetTranslation( double &flX, double &flY )
    {
        flX = 0.0;
        flY = 0.0;

        if ( !PanEffectIsActive() )
            return false;

        PanEffectComputeCurrentTranslation( flX, flY );
        return true;
    }

    // The input thread accumulates the deltas under the wlserver lock, this one is
    // called without it, so update with a compare-exchange loop.
    static void PanEffectClampDelta( std::atomic<double> &flDelta, double flMin, double flMax )
    {
        double flCurrent = flDelta.load( std::memory_order_relaxed );
        while ( true )
        {
            const double flClamped = std::clamp( flCurrent, flMin, flMax );
            if ( flClamped == flCurrent || flDelta.compare_exchange_weak( flCurrent, flClamped, std::memory_order_relaxed ) )
                return;
        }
    }

    void PanEffectLimitTranslation( double flMinX, double flMaxX )
    {
        const double flRadiansPerCount = double( cv_pan_effect_angle.Get() ) * ( k_flPanEffectPi / 180.0 );
        const double flFocalPx = double( cv_pan_effect_focal.Get() );
        if ( flRadiansPerCount <= 0.0 || flFocalPx <= 0.0 )
            return;

        // Both bounds are inverted on their own side, since the projection is not
        // symmetric once the invert convar is in play.
        const double flSign = cv_pan_effect_invert.Get() ? -1.0 : 1.0;
        const double flPhi0 = s_flPitchAtPressRad.load( std::memory_order_relaxed );
        const double flPhi = s_flPitchRad.load( std::memory_order_relaxed );

        const double flYawMax = PanEffectYawLimitForBound( flMinX, flMaxX, flPhi0, flPhi, flFocalPx, flSign, true );
        const double flYawMin = -PanEffectYawLimitForBound( flMinX, flMaxX, flPhi0, flPhi, flFocalPx, flSign, false );

        PanEffectClampDelta( s_flPanYawCounts, flYawMin / flRadiansPerCount, flYawMax / flRadiansPerCount );
    }
}
