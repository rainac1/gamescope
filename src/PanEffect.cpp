#include "PanEffect.h"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <format>
#include <mutex>
#include <string_view>
#include <unordered_set>
#include <utility>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "log.hpp"
#include "Utils/String.h"

namespace gamescope
{
    static LogScope s_PanEffectLog( "pan_effect" );

    ConVar<bool> cv_pan_effect_enabled( "pan_effect_enabled", true,
        "Enable the screen pan effect. Hold pan_effect_key and move the mouse to shift the game's frame." );
    ConVar<std::string> cv_pan_effect_key( "pan_effect_key", "BTN_RIGHT",
        "xkb keysym name, or a mouse button name (BTN_RIGHT, BTN_SIDE, Mouse4, ...), or a comma separated list of "
        "them, that drives the screen pan effect while held. Default: BTN_RIGHT" );
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
    static std::atomic<bool> s_bPanTriggerHeld{ false };
    // Set once the held pan has moved, and kept until every trigger is released:
    // the presentation layout follows this rather than the translation, which can
    // come back to zero in the middle of a hold.
    static std::atomic<bool> s_bPanEngaged{ false };
    // Pan yaw, in accumulated mouse counts, only while a trigger is held.
    static std::atomic<double> s_flPanYawCounts{ 0.0 };
    // Engine pitch, in radians, tracked at all times.
    static std::atomic<double> s_flPitchRad{ 0.0 };
    // Engine pitch when the trigger went down, phi_0 in the projection.
    static std::atomic<double> s_flPitchAtPressRad{ 0.0 };

    // A single event larger than this is a cursor jump (focus change, mode
    // switch), not pointer movement, so it is dropped.
    static constexpr double k_flPanEffectMaxStep = 4096.0;

    // The configured triggers, and the ones currently held (their Packed() ids).
    // Both under s_TriggerMutex, which lets the convar be changed from another
    // thread while the input path is pressing and releasing.
    static std::mutex s_TriggerMutex;
    static std::unordered_set<uint64_t> s_PanTriggers;
    static std::unordered_set<uint64_t> s_HeldTriggers;
    static std::string s_CachedTriggerConfig;

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

    static std::string PanEffectToLower( std::string_view svText )
    {
        std::string sLower{ svText };
        for ( char &c : sLower )
            c = char( std::tolower( (unsigned char) c ) );
        return sLower;
    }

    // The mouse buttons a pan trigger can name, with the friendly spellings for
    // them. The codes are the Linux input codes the backends deliver.
    static constexpr std::pair<std::string_view, uint32_t> k_pPanEffectButtons[] =
    {
        { "btn_left",     BTN_LEFT },
        { "btn_right",    BTN_RIGHT },
        { "btn_middle",   BTN_MIDDLE },
        { "btn_side",     BTN_SIDE },
        { "btn_extra",    BTN_EXTRA },
        { "btn_forward",  BTN_FORWARD },
        { "btn_back",     BTN_BACK },
        { "btn_task",     BTN_TASK },
        { "mouseleft",    BTN_LEFT },
        { "mouseright",   BTN_RIGHT },
        { "mousemiddle",  BTN_MIDDLE },
        { "mouseside",    BTN_SIDE },
        { "mouseextra",   BTN_EXTRA },
        { "mouseforward", BTN_FORWARD },
        { "mouseback",    BTN_BACK },
        { "mousetask",    BTN_TASK },
        // The numbered spellings follow X11: 1/2/3 are left/middle/right, and
        // 4..8 are the extra buttons in the order they are named above.
        { "mouse1",       BTN_LEFT },
        { "mouse2",       BTN_MIDDLE },
        { "mouse3",       BTN_RIGHT },
        { "mouse4",       BTN_SIDE },
        { "mouse5",       BTN_EXTRA },
        { "mouse6",       BTN_FORWARD },
        { "mouse7",       BTN_BACK },
        { "mouse8",       BTN_TASK },
    };

    // "button:275" or "button:0x113": an escape hatch for buttons that are not
    // named above, eg. on a device with more of them.
    static bool PanEffectResolveRawButton( std::string_view svName, uint32_t &uButton )
    {
        constexpr std::string_view svPrefix = "button:";
        if ( svName.size() <= svPrefix.size() ||
             PanEffectToLower( svName.substr( 0, svPrefix.size() ) ) != svPrefix )
            return false;

        const std::string sNumber{ svName.substr( svPrefix.size() ) };
        if ( sNumber.empty() || !std::isdigit( (unsigned char) sNumber[0] ) )
            return false;

        char *pszEnd = nullptr;
        const unsigned long ulCode = std::strtoul( sNumber.c_str(), &pszEnd, 0 );
        if ( pszEnd == sNumber.c_str() || *pszEnd != '\0' )
            return false;

        uButton = uint32_t( ulCode );
        return true;
    }

    static bool PanEffectResolveButton( std::string_view svName, uint32_t &uButton )
    {
        const std::string sLower = PanEffectToLower( svName );

        for ( const auto &[svButtonName, uCode] : k_pPanEffectButtons )
        {
            if ( sLower == svButtonName )
            {
                uButton = uCode;
                return true;
            }
        }

        return PanEffectResolveRawButton( sLower, uButton );
    }

    // Add one configured name to vTriggers. A mouse button is tried first: none of
    // its names is a keysym, and it keeps "mouse4" from being read as anything
    // else. "Alt" and friends have no keysym of their own, so they expand to the
    // left/right pair.
    static bool PanEffectAddTrigger( std::vector<PanEffectTrigger> &vTriggers, std::string_view svName )
    {
        uint32_t uButton = 0;
        if ( PanEffectResolveButton( svName, uButton ) )
        {
            vTriggers.push_back( { PanEffectTrigger::Kind::Button, uButton } );
            return true;
        }

        const std::string_view svKey = PanEffectKeyAlias( svName );
        xkb_keysym_t eKeysym = PanEffectResolveKeysym( svKey );
        if ( eKeysym != XKB_KEY_NoSymbol )
        {
            vTriggers.push_back( { PanEffectTrigger::Kind::Key, eKeysym } );

            // Either side of a modifier pair drives the effect.
            if ( eKeysym == XKB_KEY_Alt_L )
                vTriggers.push_back( { PanEffectTrigger::Kind::Key, XKB_KEY_Alt_R } );
            else if ( eKeysym == XKB_KEY_Alt_R )
                vTriggers.push_back( { PanEffectTrigger::Kind::Key, XKB_KEY_Alt_L } );

            return true;
        }

        const std::string sLeft = std::string( svKey ) + "_L";
        const xkb_keysym_t eLeft = PanEffectResolveKeysym( sLeft );
        if ( eLeft != XKB_KEY_NoSymbol )
        {
            vTriggers.push_back( { PanEffectTrigger::Kind::Key, eLeft } );

            const std::string sRight = std::string( svKey ) + "_R";
            const xkb_keysym_t eRight = PanEffectResolveKeysym( sRight );
            if ( eRight != XKB_KEY_NoSymbol )
                vTriggers.push_back( { PanEffectTrigger::Kind::Key, eRight } );

            return true;
        }

        return false;
    }

    std::vector<PanEffectTrigger> PanEffectParseTriggers( std::string_view svConfig, std::vector<std::string> &vsUnknown )
    {
        std::vector<PanEffectTrigger> vTriggers;

        for ( std::string_view svToken : Split( svConfig, "," ) )
        {
            // Be forgiving about "Alt, BTN_SIDE" style lists.
            const size_t nStart = svToken.find_first_not_of( " \t" );
            if ( nStart == std::string_view::npos )
                continue;

            const size_t nEnd = svToken.find_last_not_of( " \t" );
            const std::string_view svName = svToken.substr( nStart, nEnd - nStart + 1 );

            if ( !PanEffectAddTrigger( vTriggers, svName ) )
                vsUnknown.emplace_back( svName );
        }

        return vTriggers;
    }

    static void PanEffectRebuildTriggerList()
    {
        std::lock_guard<std::mutex> lock( s_TriggerMutex );

        const std::string sConfig{ cv_pan_effect_key.Get() };
        if ( sConfig == s_CachedTriggerConfig )
            return;

        s_CachedTriggerConfig = sConfig;
        s_PanTriggers.clear();

        std::vector<std::string> vsUnknown;
        for ( const PanEffectTrigger &trigger : PanEffectParseTriggers( sConfig, vsUnknown ) )
            s_PanTriggers.insert( trigger.Packed() );

        for ( const std::string &sName : vsUnknown )
            s_PanEffectLog.errorf( "pan_effect_key: unknown key or mouse button \"%s\".", sName.c_str() );

        if ( s_PanTriggers.empty() )
            s_PanEffectLog.errorf( "pan_effect_key: no usable keys in \"%s\", the pan effect is inert.", sConfig.c_str() );
    }

    static bool PanEffectIsPanTrigger( uint64_t uTrigger )
    {
        std::lock_guard<std::mutex> lock( s_TriggerMutex );
        return s_PanTriggers.contains( uTrigger );
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
        // the held pan moves and stays true until every trigger is released, even if
        // the translation comes back to zero in between.
        return cv_pan_effect_enabled.Get() &&
            s_bPanTriggerHeld.load( std::memory_order_relaxed ) &&
            s_bPanEngaged.load( std::memory_order_relaxed );
    }

    void PanEffectReset()
    {
        {
            std::lock_guard<std::mutex> lock( s_TriggerMutex );
            s_HeldTriggers.clear();
        }

        s_bPanTriggerHeld.store( false, std::memory_order_relaxed );
        s_bPanEngaged.store( false, std::memory_order_relaxed );
        s_flPanYawCounts.store( 0.0, std::memory_order_relaxed );
        // The tracked pitch is deliberately left alone: it mirrors the game's camera,
        // which does not recentre just because the session paused.
    }

    // A fresh hold starts from the centre again, relative to the pitch the engine
    // (the game's camera) is at right now.
    static void PanEffectStartHold()
    {
        s_flPanYawCounts.store( 0.0, std::memory_order_relaxed );
        s_bPanEngaged.store( false, std::memory_order_relaxed );
        s_flPitchAtPressRad.store( s_flPitchRad.load( std::memory_order_relaxed ), std::memory_order_relaxed );
        s_PanEffectLog.log( cv_pan_effect_debug.Get() ? LOG_INFO : LOG_DEBUG,
            std::format( "Pan hold started at pitch {:.1f} deg.",
                s_flPitchAtPressRad.load( std::memory_order_relaxed ) * 180.0 / k_flPanEffectPi ) );
    }

    static void PanEffectEndHold()
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

        s_flPanYawCounts.store( 0.0, std::memory_order_relaxed );
        s_bPanEngaged.store( false, std::memory_order_relaxed );
    }

    // The keys and buttons both land here, as their Packed() ids. The hold lasts
    // until every configured trigger that went down has been released, so holding
    // "Alt, Mouse4" and releasing only one of them keeps the pan alive.
    static void PanEffectObserveTrigger( uint64_t uTrigger, bool bPressed )
    {
        if ( !cv_pan_effect_enabled.Get() )
        {
            // Don't leave a hold behind if the effect was switched off mid-press.
            PanEffectReset();
            return;
        }

        PanEffectRebuildTriggerList();

        if ( !PanEffectIsPanTrigger( uTrigger ) )
            return;

        bool bHoldChanged = false;
        {
            std::lock_guard<std::mutex> lock( s_TriggerMutex );

            if ( bPressed )
                bHoldChanged = s_HeldTriggers.insert( uTrigger ).second && s_HeldTriggers.size() == 1;
            else
                bHoldChanged = s_HeldTriggers.erase( uTrigger ) > 0 && s_HeldTriggers.empty();
        }

        if ( !bHoldChanged )
            return;

        s_bPanTriggerHeld.store( bPressed, std::memory_order_relaxed );

        if ( bPressed )
            PanEffectStartHold();
        else
            PanEffectEndHold();
    }

    void PanEffectObserveKey( xkb_keysym_t eKeysym, bool bPressed )
    {
        PanEffectObserveTrigger( PanEffectTrigger{ PanEffectTrigger::Kind::Key, eKeysym }.Packed(), bPressed );
    }

    void PanEffectObserveButton( uint32_t uButton, bool bPressed )
    {
        PanEffectObserveTrigger( PanEffectTrigger{ PanEffectTrigger::Kind::Button, uButton }.Packed(), bPressed );
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

        if ( !s_bPanTriggerHeld.load( std::memory_order_relaxed ) )
            return false;

        // The pan yaw accumulates while a trigger is held. The pan pitch is the tracked
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

        // Truncate the accumulated yaw at the yaw that reaches each bound. The invert
        // convar is applied to the projection, so each bound is inverted on its own
        // side before it is asked what yaw it takes. The pitch is tracked rather than
        // accumulated for the pan, so it needs no equivalent.
        const double flSign = cv_pan_effect_invert.Get() ? -1.0 : 1.0;
        const double flPhi0 = s_flPitchAtPressRad.load( std::memory_order_relaxed );
        const double flPhi = s_flPitchRad.load( std::memory_order_relaxed );

        const double flYawMax = PanEffectYawForTranslation( flSign * flMaxX, flPhi0, flPhi, flFocalPx );
        const double flYawMin = -PanEffectYawForTranslation( -flSign * flMinX, flPhi0, flPhi, flFocalPx );

        PanEffectClampDelta( s_flPanYawCounts, flYawMin / flRadiansPerCount, flYawMax / flRadiansPerCount );
    }
}
