#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "PanEffect.h"

using namespace gamescope;

static constexpr double k_flDegToRad = k_flPanEffectPi / 180.0;

TEST_CASE("PanEffect", "[pan_effect]") {
	// No motion, no translation.
	REQUIRE(PanEffectComputeTranslation(0.0, 0.022 * k_flDegToRad, 720.0) == 0.0);

	// 100 counts at 0.022 degrees each is 2.2 degrees.
	{
		const double flExpected = 720.0 * std::tan(2.2 * k_flDegToRad);
		REQUIRE(std::abs(PanEffectComputeTranslation(100.0, 0.022 * k_flDegToRad, 720.0) - flExpected) < 1e-9);
	}

	// f scales the projection linearly.
	{
		const double flA = PanEffectComputeTranslation(100.0, 0.022 * k_flDegToRad, 720.0);
		const double flB = PanEffectComputeTranslation(100.0, 0.022 * k_flDegToRad, 1440.0);
		REQUIRE(std::abs(flB - 2.0 * flA) < 1e-9);
	}

	// k scales the angle linearly for small angles.
	{
		const double flA = PanEffectComputeTranslation(100.0, 0.022 * k_flDegToRad, 720.0);
		const double flB = PanEffectComputeTranslation(100.0, 0.044 * k_flDegToRad, 720.0);
		REQUIRE(flB > flA);
	}

	// Symmetric in the sign of the motion.
	REQUIRE(PanEffectComputeTranslation(-100.0, 0.022 * k_flDegToRad, 720.0) ==
	        -PanEffectComputeTranslation(100.0, 0.022 * k_flDegToRad, 720.0));

	// A huge accumulation is clamped so the tangent cannot blow up.
	{
		const double flClamped = PanEffectComputeTranslation(1e9, 0.022 * k_flDegToRad, 720.0);
		REQUIRE(std::isfinite(flClamped));
		REQUIRE(std::abs(flClamped) < 1e6);
	}
}
