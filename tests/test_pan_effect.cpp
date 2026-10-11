#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "PanEffect.h"

using namespace gamescope;

static constexpr double k_flDegToRad = k_flPanEffectPi / 180.0;

// Helper so the tests can stay about numbers instead of out parameters.
static std::pair<double, double> Project(double flThetaX, double flPhi0, double flPhi, double flFocalPx)
{
	double flX = 0.0, flY = 0.0;
	PanEffectComputeTranslation(flThetaX, flPhi0, flPhi, flFocalPx, flX, flY);
	return { flX, flY };
}

TEST_CASE("PanEffect", "[pan_effect]") {
	// Nothing moved, nothing translated.
	{
		const auto [flX, flY] = Project(0.0, 0.4, 0.4, 720.0);
		REQUIRE(flX == 0.0);
		REQUIRE(flY == 0.0);
	}

	// With the engine level and no pan pitch the projection is the plain tangent of
	// the yaw, which is what the effect did before the pitch was tracked.
	{
		const double flThetaX = 2.2 * k_flDegToRad;
		const auto [flX, flY] = Project(flThetaX, 0.0, 0.0, 720.0);
		REQUIRE(std::abs(flX - 720.0 * std::tan(flThetaX)) < 1e-9);
		REQUIRE(flY == 0.0);
	}

	// f scales the projection linearly.
	{
		const double flThetaX = 2.2 * k_flDegToRad;
		const auto [flA, flAY] = Project(flThetaX, 0.3, 0.35, 720.0);
		const auto [flB, flBY] = Project(flThetaX, 0.3, 0.35, 1440.0);
		REQUIRE(std::abs(flB - 2.0 * flA) < 1e-9);
		REQUIRE(std::abs(flBY - 2.0 * flAY) < 1e-9);
	}

	// The projection is odd in the yaw: panning the other way translates the other way.
	{
		const auto [flXPos, flYPos] = Project(0.2, -0.3, -0.25, 720.0);
		const auto [flXNeg, flYNeg] = Project(-0.2, -0.3, -0.25, 720.0);
		REQUIRE(std::abs(flXNeg + flXPos) < 1e-9);
		REQUIRE(std::abs(flYNeg - flYPos) < 1e-9);
	}

	// Pitching changes both components: the yaw no longer moves the picture straight
	// sideways once the engine is looking up or down.
	{
		const auto [flXLevel, flYLevel] = Project(0.2, 0.0, 0.0, 720.0);
		const auto [flXPitched, flYPitched] = Project(0.2, 0.0, -0.5, 720.0);
		REQUIRE(std::abs(flXPitched - flXLevel) > 1e-3);
		REQUIRE(std::abs(flYPitched) > 1e-3);
	}

	// Larger yaw, larger translation, until the yaw is clamped.
	{
		const auto [flSmall, flSmallY] = Project(0.1, 0.2, 0.2, 720.0);
		const auto [flLarge, flLargeY] = Project(0.2, 0.2, 0.2, 720.0);
		REQUIRE(flLarge > flSmall);
	}

	// Past the clamp the yaw stops mattering.
	{
		const auto [flClamped, flClampedY] = Project(k_flPanEffectMaxTheta, 0.0, 0.0, 720.0);
		const auto [flBeyond, flBeyondY] = Project(k_flPanEffectMaxTheta * 4.0, 0.0, 0.0, 720.0);
		REQUIRE(flClamped == flBeyond);
	}

	// Degenerate input (both pitches small and opposite, yaw at the clamp) can drive
	// the denominator towards zero. The result has to stay finite; the caller clamps
	// it to the output anyway.
	{
		const auto [flX, flY] = Project(k_flPanEffectMaxTheta, 0.0, -1e-9, 720.0);
		REQUIRE(std::isfinite(flX));
		REQUIRE(std::isfinite(flY));
		REQUIRE(std::abs(flX) < 1e9);
		REQUIRE(std::abs(flY) < 1e9);
	}

	// Extreme pitches stay finite too.
	{
		const auto [flX, flY] = Project(k_flPanEffectMaxTheta, k_flPanEffectMaxPitch, -k_flPanEffectMaxPitch, 720.0);
		REQUIRE(std::isfinite(flX));
		REQUIRE(std::isfinite(flY));
	}
}

TEST_CASE("PanEffectTrackPitch", "[pan_effect]") {
	const double flLimit = 89.0 * k_flDegToRad;

	// Starts at zero and accumulates both ways.
	REQUIRE(PanEffectTrackPitch(0.0, 0.1, flLimit) == 0.1);
	REQUIRE(PanEffectTrackPitch(0.0, -0.1, flLimit) == -0.1);
	REQUIRE(std::abs(PanEffectTrackPitch(0.1, 0.1, flLimit) - 0.2) < 1e-12);

	// Saturates at the limit, so pulling further in the same direction does nothing.
	REQUIRE(PanEffectTrackPitch(flLimit, 0.5, flLimit) == flLimit);
	REQUIRE(PanEffectTrackPitch(flLimit, -0.5, flLimit) == flLimit - 0.5);
	REQUIRE(PanEffectTrackPitch(-flLimit, -0.5, flLimit) == -flLimit);

	// A configured limit is respected, and cannot exceed the angle where tan() blows
	// up any more than the hard cap allows.
	REQUIRE(PanEffectTrackPitch(0.0, 10.0, 30.0 * k_flDegToRad) == 30.0 * k_flDegToRad);
	REQUIRE(PanEffectTrackPitch(0.0, 10.0, 360.0 * k_flDegToRad) == k_flPanEffectMaxPitch);
	REQUIRE(PanEffectTrackPitch(1.0, 0.0, 0.0) == 0.0);
}

TEST_CASE("PanEffectYawForTranslation", "[pan_effect]") {
	const double flFocal = 720.0;
	const double flBound = 640.0;

	// The inverse lands on the requested translation wherever the projection can reach
	// it, which is everywhere within the bounds in practice.
	for (double flPhi0Deg : { 0.0, 30.0, -45.0, 70.0 })
	{
		for (double flPhiDeg : { 0.0, 25.0, -50.0, 80.0 })
		{
			const double flPhi0 = flPhi0Deg * k_flDegToRad;
			const double flPhi = flPhiDeg * k_flDegToRad;
			const double flMaxYaw = PanEffectMaxYawForPitch(flPhi0, flPhi);

			// With the two pitches equal the projection is f * tan(yaw) / ... and the
			// translation therefore very reachable; with them far apart it may not be.
			const auto [flReach, flReachY] = Project(flMaxYaw, flPhi0, flPhi, flFocal);

			const double flYaw = PanEffectYawForTranslation(flBound, flPhi0, flPhi, flFocal);
			REQUIRE(flYaw >= 0.0);
			REQUIRE(flYaw <= flMaxYaw + 1e-9);

			const auto [flX, flY] = Project(flYaw, flPhi0, flPhi, flFocal);
			if (flReach < 0.0)
			{
				// Denominator negative: the tracked point is behind the camera, the
				// projection has no meaning, nothing accumulates.
				REQUIRE(flYaw == 0.0);
			}
			else if (flReach >= flBound)
			{
				REQUIRE(std::abs(flX - flBound) < 1e-6);
			}
			else
			{
				// Valid but out of reach: the whole yaw range is allowed.
				REQUIRE(flYaw == Catch::Approx(flMaxYaw));
			}
		}
	}

	// Nonsense input cannot produce a yaw.
	REQUIRE(PanEffectYawForTranslation(0.0, 0.0, 0.0, flFocal) == 0.0);
	REQUIRE(PanEffectYawForTranslation(-10.0, 0.0, 0.0, flFocal) == 0.0);
	REQUIRE(PanEffectYawForTranslation(100.0, 0.0, 0.0, 0.0) == 0.0);

	// Where the pitch pair puts the tracked point behind the camera the denominator is
	// negative and the projection has no meaning, so nothing accumulates.
	{
		const double flPhi0 = 89.0 * k_flDegToRad;
		const double flPhi = -2.0 * k_flDegToRad;
		REQUIRE(PanEffectYawForTranslation(flBound, flPhi0, flPhi, flFocal) == 0.0);
		REQUIRE(PanEffectYawForTranslation(1.0, flPhi0, flPhi, flFocal) == 0.0);
	}

	// Sanity: the reachable range the caller clamps to really is in bounds.
	{
		const double flPhi0 = 60.0 * k_flDegToRad;
		const double flPhi = -30.0 * k_flDegToRad;
		const double flYaw = PanEffectYawForTranslation(flBound, flPhi0, flPhi, flFocal);
		const auto [flX, flY] = Project(flYaw, flPhi0, flPhi, flFocal);
		REQUIRE(flX <= flBound + 1e-6);
		REQUIRE(flX >= 0.0);
	}
}

TEST_CASE("PanEffectMaxYawForPitch", "[pan_effect]") {
	// No pole with the two pitches of the same sign (or either of them level).
	REQUIRE(PanEffectMaxYawForPitch(0.0, 0.0) == k_flPanEffectMaxTheta);
	REQUIRE(PanEffectMaxYawForPitch(30.0 * k_flDegToRad, 40.0 * k_flDegToRad) == k_flPanEffectMaxTheta);
	REQUIRE(PanEffectMaxYawForPitch(30.0 * k_flDegToRad, -100.0 * k_flDegToRad) == k_flPanEffectMaxTheta);

	// Opposite signs bring the pole in: cos(theta_pole) == -tan(phi_0) tan(phi).
	{
		const double flPhi0 = 30.0 * k_flDegToRad;
		const double flPhi = -50.0 * k_flDegToRad;
		const double flPoleCos = -std::tan(flPhi0) * std::tan(flPhi);
		const double flMaxYaw = PanEffectMaxYawForPitch(flPhi0, flPhi);
		REQUIRE(flMaxYaw < k_flPanEffectMaxTheta);
		REQUIRE(std::abs(std::cos(flMaxYaw) - flPoleCos) < 1e-9);

		// And the projection stays finite and on one side of it.
		const auto [flX, flY] = Project(flMaxYaw * 4.0, flPhi0, flPhi, 720.0);
		REQUIRE(std::isfinite(flX));
		REQUIRE(std::isfinite(flY));
		REQUIRE(flX > 0.0);
	}
}

TEST_CASE("PanEffectSnapToTexels", "[pan_effect]") {
	// A layer covering the output 1:1 moves in whole output pixels.
	REQUIRE(PanEffectSnapToTexels(0.0, 1.0) == 0.0);
	REQUIRE(PanEffectSnapToTexels(10.4, 1.0) == 10.0);
	REQUIRE(PanEffectSnapToTexels(10.5, 1.0) == 11.0);
	REQUIRE(PanEffectSnapToTexels(-10.4, 1.0) == -10.0);
	REQUIRE(PanEffectSnapToTexels(-10.5, 1.0) == -11.0);

	// Sub-texel motion snaps to nothing, which is what lets a hold that does not
	// actually move anything cost nothing.
	REQUIRE(PanEffectSnapToTexels(0.4, 1.0) == 0.0);
	REQUIRE(PanEffectSnapToTexels(-0.4, 1.0) == 0.0);

	// A layer that is upscaled 1.5x moves in whole source texels, which is
	// 1.5 output pixels at a time.
	REQUIRE(PanEffectSnapToTexels(2.0, 1.5) == 2.0);
	REQUIRE(PanEffectSnapToTexels(10.0 / 1.5, 1.5) == 10.0 / 1.5);
	REQUIRE(PanEffectSnapToTexels(10.0 / 1.5 + 0.1, 1.5) == 10.0 / 1.5);

	// The whole point: offset * scale stays a whole number of texels, so the
	// layer is sampled at exactly the phase it has without any pan.
	for (double flScale : { 1.0, 1.5, 2.0, 0.5, 1536.0 / 1920.0, 1280.0 / 1080.0 })
	{
		for (double flTranslation : { 0.0, 0.3, -0.3, 1.0, -1.0, 224.6, -224.6, 1000.0 })
		{
			const double flSnapped = PanEffectSnapToTexels(flTranslation, flScale);
			REQUIRE(std::abs(flSnapped * flScale - std::round(flSnapped * flScale)) < 1e-9);

			// And it never moves further than half a texel.
			REQUIRE(std::abs(flSnapped - flTranslation) * flScale <= 0.5 + 1e-9);
		}
	}

	// A layer without a texture extent is left alone instead of dividing by zero.
	REQUIRE(PanEffectSnapToTexels(12.5, 0.0) == 12.5);
}

static PanEffectTrigger ParseOne(const std::string &sConfig)
{
	std::vector<std::string> vsUnknown;
	const std::vector<PanEffectTrigger> triggers = PanEffectParseTriggers(sConfig, vsUnknown);
	REQUIRE(vsUnknown.empty());
	REQUIRE(triggers.size() == 1);
	return triggers[0];
}

TEST_CASE("PanEffectParseTriggers", "[pan_effect]") {
	// A keysym still works, and a bare modifier expands to both of its sides.
	{
		std::vector<std::string> vsUnknown;
		const auto triggers = PanEffectParseTriggers("Alt", vsUnknown);
		REQUIRE(vsUnknown.empty());
		REQUIRE(triggers.size() == 2);
		REQUIRE(triggers[0] == PanEffectTrigger{ PanEffectTrigger::Kind::Key, XKB_KEY_Alt_L });
		REQUIRE(triggers[1] == PanEffectTrigger{ PanEffectTrigger::Kind::Key, XKB_KEY_Alt_R });
	}

	// The existing aliases keep working. A bare modifier has no keysym of its own,
	// so it comes out as both of its sides.
	REQUIRE(ParseOne("Super_L") == PanEffectTrigger{ PanEffectTrigger::Kind::Key, XKB_KEY_Super_L });
	{
		std::vector<std::string> vsUnknown;
		const auto ctrl = PanEffectParseTriggers("Ctrl", vsUnknown);
		REQUIRE(vsUnknown.empty());
		REQUIRE(ctrl.size() == 2);
		REQUIRE(ctrl[0] == PanEffectTrigger{ PanEffectTrigger::Kind::Key, XKB_KEY_Control_L });
		REQUIRE(ctrl[1] == PanEffectTrigger{ PanEffectTrigger::Kind::Key, XKB_KEY_Control_R });

		const auto win = PanEffectParseTriggers("win", vsUnknown);
		REQUIRE(vsUnknown.empty());
		REQUIRE(win.size() == 2);
		REQUIRE(win[0] == PanEffectTrigger{ PanEffectTrigger::Kind::Key, XKB_KEY_Super_L });
		REQUIRE(win[1] == PanEffectTrigger{ PanEffectTrigger::Kind::Key, XKB_KEY_Super_R });
	}

	// Mouse buttons, by their Linux input names.
	REQUIRE(ParseOne("BTN_RIGHT") == PanEffectTrigger{ PanEffectTrigger::Kind::Button, BTN_RIGHT });
	REQUIRE(ParseOne("BTN_SIDE") == PanEffectTrigger{ PanEffectTrigger::Kind::Button, BTN_SIDE });
	REQUIRE(ParseOne("BTN_EXTRA") == PanEffectTrigger{ PanEffectTrigger::Kind::Button, BTN_EXTRA });

	// Names are case insensitive, and the friendly spellings resolve too.
	REQUIRE(ParseOne("btn_middle") == PanEffectTrigger{ PanEffectTrigger::Kind::Button, BTN_MIDDLE });
	REQUIRE(ParseOne("MouseLeft") == PanEffectTrigger{ PanEffectTrigger::Kind::Button, BTN_LEFT });
	REQUIRE(ParseOne("Mouseright") == PanEffectTrigger{ PanEffectTrigger::Kind::Button, BTN_RIGHT });
	REQUIRE(ParseOne("mouse4") == PanEffectTrigger{ PanEffectTrigger::Kind::Button, BTN_SIDE });
	REQUIRE(ParseOne("Mouse5") == PanEffectTrigger{ PanEffectTrigger::Kind::Button, BTN_EXTRA });
	REQUIRE(ParseOne("mouse3") == PanEffectTrigger{ PanEffectTrigger::Kind::Button, BTN_RIGHT });

	// A raw code is the escape hatch for buttons that have no name here.
	REQUIRE(ParseOne("button:275") == PanEffectTrigger{ PanEffectTrigger::Kind::Button, BTN_SIDE });
	REQUIRE(ParseOne("Button:0x113") == PanEffectTrigger{ PanEffectTrigger::Kind::Button, BTN_SIDE });

	// A malformed one is reported, not read as some wrapped-around code.
	{
		std::vector<std::string> vsUnknown;
		REQUIRE(PanEffectParseTriggers("button:", vsUnknown).empty());
		REQUIRE(PanEffectParseTriggers("button:-1", vsUnknown).empty());
		REQUIRE(PanEffectParseTriggers("button:0x11g", vsUnknown).empty());
		REQUIRE(vsUnknown.size() == 3);
	}

	// A list mixes keys and buttons, is forgiving about spaces, and skips empty
	// entries instead of reporting them.
	{
		std::vector<std::string> vsUnknown;
		const auto triggers = PanEffectParseTriggers(" Super_L , BTN_EXTRA,Mouse3 ,, ", vsUnknown);
		REQUIRE(vsUnknown.empty());
		REQUIRE(triggers.size() == 3);
		REQUIRE(triggers[0] == PanEffectTrigger{ PanEffectTrigger::Kind::Key, XKB_KEY_Super_L });
		REQUIRE(triggers[1] == PanEffectTrigger{ PanEffectTrigger::Kind::Button, BTN_EXTRA });
		REQUIRE(triggers[2] == PanEffectTrigger{ PanEffectTrigger::Kind::Button, BTN_RIGHT });
	}

	// Unknown names are reported rather than silently dropped.
	{
		std::vector<std::string> vsUnknown;
		const auto triggers = PanEffectParseTriggers("Alt, NotAThing", vsUnknown);
		REQUIRE(triggers.size() == 2);
		REQUIRE(vsUnknown == std::vector<std::string>{ "NotAThing" });
	}

	// Nothing at all is not an error, it just leaves the effect inert.
	{
		std::vector<std::string> vsUnknown;
		REQUIRE(PanEffectParseTriggers("", vsUnknown).empty());
		REQUIRE(PanEffectParseTriggers(" , ,, ", vsUnknown).empty());
		REQUIRE(vsUnknown.empty());
	}

	// A bare "forward" is not the button: the BTN_/Mouse prefix is what says a name
	// is a mouse button, and xkb does not know the bare word either.
	{
		std::vector<std::string> vsUnknown;
		REQUIRE(PanEffectParseTriggers("forward", vsUnknown).empty());
		REQUIRE(vsUnknown == std::vector<std::string>{ "forward" });
	}
	REQUIRE(ParseOne("BTN_FORWARD") == PanEffectTrigger{ PanEffectTrigger::Kind::Button, BTN_FORWARD });

	// A key and a button never collide, even at the same numeric code.
	REQUIRE(PanEffectTrigger{ PanEffectTrigger::Kind::Key, BTN_RIGHT }.Packed() !=
	        PanEffectTrigger{ PanEffectTrigger::Kind::Button, BTN_RIGHT }.Packed());
}
