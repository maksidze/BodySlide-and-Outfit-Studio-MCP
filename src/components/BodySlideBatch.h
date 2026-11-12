/*
BodySlide and Outfit Studio
See the included LICENSE file
*/
#pragma once

#include "BuildSelection.h"
#include "SliderManager.h"
#include <functional>

// Optional unattended inputs to the same batch engine used by the window. The
// caller owns a snapshot of presets and zap choices; UI slider edits are ignored.
struct BodySlideBatchOptions {
	std::string preset;
	SliderManager sliders;
	BuildSelection selection;
	bool conformAddOns = false;
	float clippingFixStrength = 0.0f;
	std::function<void(size_t, size_t, const std::string&)> progress;
	std::map<std::string, std::vector<std::string>> writtenFiles;
};
