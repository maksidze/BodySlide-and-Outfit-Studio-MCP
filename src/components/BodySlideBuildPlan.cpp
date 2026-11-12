/*
BodySlide and Outfit Studio
See the included LICENSE file
*/
#include "BodySlideBuildPlan.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>

namespace {
std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}
}

std::string BodySlideOutputKey(const std::string& path) {
	std::string normalized = path;
	std::replace(normalized.begin(), normalized.end(), '\\', '/');
	if (normalized.empty() || normalized.front() == '/' || normalized.find(':') != std::string::npos)
		throw std::runtime_error("Output must be a relative game asset path: '" + path + "'.");
	std::string result;
	size_t start = 0;
	while (start < normalized.size()) {
		size_t end = normalized.find('/', start);
		if (end == std::string::npos)
			end = normalized.size();
		std::string part = normalized.substr(start, end - start);
		if (part == ".." || part.find_first_of("<>\"|?*\r\n") != std::string::npos ||
			std::any_of(part.begin(), part.end(), [](unsigned char c) { return c < 32; }) ||
			(!part.empty() && part != "." && (part.back() == '.' || part.back() == ' ')))
			throw std::runtime_error("Unsafe output path: '" + path + "'.");
		const std::string stem = Lower(part.substr(0, part.find('.')));
		if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
			(stem.size() == 4 && (stem.substr(0, 3) == "com" || stem.substr(0, 3) == "lpt") && stem[3] >= '1' && stem[3] <= '9'))
			throw std::runtime_error("Reserved device name in output path: '" + path + "'.");
		if (!part.empty() && part != ".")
			result += (result.empty() ? "" : "/") + Lower(part);
		start = end + 1;
	}
	if (result.empty())
		throw std::runtime_error("Empty output path.");
	return result;
}

bool BodySlideBuildResolution::Ready() const {
	return std::all_of(conflicts.begin(), conflicts.end(), [](const auto& c) { return !c.at("winner").is_null(); });
}

BodySlideBuildResolution ResolveBodySlideBuild(const std::vector<BodySlideBuildCandidate>& candidates,
	const std::map<std::string, std::string>& savedChoices,
	const std::map<std::string, std::string>& choices,
	const std::vector<std::string>& prefer) {
	std::map<std::string, std::vector<size_t>> writers;
	for (const auto& rule : prefer)
		if (rule.empty())
			throw std::runtime_error("Preference rules cannot be empty.");
	std::set<std::string> names;
	for (size_t i = 0; i < candidates.size(); ++i) {
		if (!names.insert(candidates[i].name).second)
			throw std::runtime_error("Duplicate build candidate: " + candidates[i].name);
		std::set<std::string> ownFiles;
		for (const auto& file : candidates[i].files)
			if (ownFiles.insert(BodySlideOutputKey(file)).second)
				writers[BodySlideOutputKey(file)].push_back(i);
	}
	std::map<std::string, std::string> explicitChoices;
	for (const auto& choice : choices) {
		std::string key = BodySlideOutputKey(choice.first);
		auto writer = writers.find(key);
		if (writer == writers.end() || writer->second.size() < 2)
			throw std::runtime_error("No conflict for '" + choice.first + "'.");
		bool found = false;
		for (size_t i : writer->second)
			found |= candidates[i].name == choice.second;
		if (!found)
			throw std::runtime_error("'" + choice.second + "' does not write '" + choice.first + "'.");
		auto inserted = explicitChoices.emplace(key, choice.second);
		if (!inserted.second && inserted.first->second != choice.second)
			throw std::runtime_error("Contradictory choices for '" + key + "'.");
	}
	std::map<std::string, std::string> saved;
	for (const auto& choice : savedChoices) {
		try { saved[BodySlideOutputKey(choice.first)] = choice.second; }
		catch (const std::exception&) { /* Obsolete paths never select a winner. */ }
	}
	BodySlideBuildResolution result;
	std::set<std::string> losers;
	for (const auto& writer : writers) {
		if (writer.second.size() < 2)
			continue;
		std::string winner, reason;
		nlohmann::json options = nlohmann::json::array();
		for (size_t i : writer.second)
			options.push_back(candidates[i].name);
		auto explicitChoice = explicitChoices.find(writer.first);
		if (explicitChoice != explicitChoices.end()) {
			winner = explicitChoice->second;
			reason = "explicit";
		}
		else {
			std::set<std::string> savedWinners;
			for (size_t i : writer.second) {
				auto choice = saved.find(BodySlideOutputKey(candidates[i].outputBase));
				if (choice != saved.end() && std::find(options.begin(), options.end(), choice->second) != options.end())
					savedWinners.insert(choice->second);
			}
			if (savedWinners.size() == 1) {
				winner = *savedWinners.begin();
				reason = "saved";
			}
			else {
				for (const auto& rule : prefer) {
					std::vector<std::string> matches;
					for (size_t i : writer.second)
						if (Lower(candidates[i].name).find(Lower(rule)) != std::string::npos)
							matches.push_back(candidates[i].name);
					if (matches.empty())
						continue;
					if (matches.size() == 1) {
						winner = matches.front();
						reason = "prefer:" + rule;
					}
					break; // A tied higher-priority rule stays ambiguous.
				}
			}
		}
		if (!winner.empty())
			for (size_t i : writer.second)
				if (candidates[i].name != winner)
					losers.insert(candidates[i].name);
		result.conflicts.push_back({{"path", writer.first}, {"candidates", options},
			{"winner", winner.empty() ? nlohmann::json(nullptr) : nlohmann::json(winner)}, {"reason", reason}});
	}
	for (const auto& conflict : result.conflicts)
		if (!conflict["winner"].is_null() && losers.count(conflict["winner"].get<std::string>()))
			throw std::runtime_error("Conflicting winners across output files. A project must win all its shared outputs: " + conflict["winner"].get<std::string>());
	for (const auto& candidate : candidates)
		(losers.count(candidate.name) ? result.skipped : result.selected).push_back(candidate.name);
	return result;
}
