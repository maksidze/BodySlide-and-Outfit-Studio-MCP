/*
BodySlide and Outfit Studio
See the included LICENSE file
*/
#pragma once

#include <nlohmann/json.hpp>
#include <map>
#include <string>
#include <vector>

struct BodySlideBuildCandidate {
	std::string name;
	std::string outputBase;
	std::vector<std::string> files;
};

struct BodySlideBuildResolution {
	std::vector<std::string> selected;
	std::vector<std::string> skipped;
	nlohmann::json conflicts = nlohmann::json::array();
	bool Ready() const;
};

// Relative output paths, normalized for Windows game assets. Rejects absolute paths,
// parent traversal, alternate streams and empty paths before any output is written.
std::string BodySlideOutputKey(const std::string& path);

// Choices use the actual relative output filenames returned in conflicts. A project is
// built in full or skipped in full; contradictory winners across its outputs are rejected.
BodySlideBuildResolution ResolveBodySlideBuild(
	const std::vector<BodySlideBuildCandidate>& candidates,
	const std::map<std::string, std::string>& savedChoices,
	const std::map<std::string, std::string>& choices,
	const std::vector<std::string>& prefer);
