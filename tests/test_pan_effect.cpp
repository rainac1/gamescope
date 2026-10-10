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

TEST_CASE("PanEffectSnapToTexels", "[pan_effect]") {
	// A layer covering the output 1:1 moves in whole output pixels.
	REQUIRE(PanEffectSnapToTexels(0.0, 1.0) == 0.0);
	REQUIRE(PanEffectSnapToTexels(10.4, 1.0) == 10.0);
	REQUIRE(PanEffectSnapToTexels(10.5, 1.0) == 11.0);
	REQUIRE(PanEffectSnapToTexels(-10.4, 1.0) == -10.0);
	REQUIRE(PanEffectSnapToTexels(-10.5, 1.0) == -11.0);

	// Sub-texel motion snaps to nothing, which is what lets the pan stop
	// costing a composite while the key is held but the mouse is not moving.
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

TEST_CASE("PanEffectCountsForTranslation", "[pan_effect]") {
	const double flRad = 0.022 * k_flDegToRad;

	// Round trip with the projection it inverts.
	for (double flCounts : { 0.0, 1.0, -1.0, 100.0, -100.0, 1000.0, -1000.0, 2000.0 })
	{
		const double flTrans = PanEffectComputeTranslation(flCounts, flRad, 720.0);
		REQUIRE(std::abs(PanEffectCountsForTranslation(flTrans, flRad, 720.0) - flCounts) < 1e-9);
	}

	// No motion, no counts.
	REQUIRE(PanEffectCountsForTranslation(0.0, flRad, 720.0) == 0.0);

	// Symmetric and monotone.
	REQUIRE(PanEffectCountsForTranslation(-100.0, flRad, 720.0) == -PanEffectCountsForTranslation(100.0, flRad, 720.0));
	REQUIRE(PanEffectCountsForTranslation(200.0, flRad, 720.0) > PanEffectCountsForTranslation(100.0, flRad, 720.0));

	// A translation past what the projection can ever reach stays at its limit
	// instead of running off, so the accumulation stops there too.
	{
		const double flLimit = 720.0 * std::tan(k_flPanEffectMaxTheta);
		const double flCounts = PanEffectCountsForTranslation(flLimit * 4.0, flRad, 720.0);
		REQUIRE(std::abs(flCounts) < 1e6);
		REQUIRE(std::abs(flCounts * flRad) <= k_flPanEffectMaxTheta + 1e-9);
	}

	// A degenerate angle or focal length cannot produce a usable limit.
	REQUIRE(PanEffectCountsForTranslation(100.0, 0.0, 720.0) == 0.0);
	REQUIRE(PanEffectCountsForTranslation(100.0, flRad, 0.0) == 0.0);
}
