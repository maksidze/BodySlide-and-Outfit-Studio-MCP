/*
BodySlide and Outfit Studio
See the included LICENSE file
*/
#include "BodySlideMcp.h"
#include "McpArgs.h"
#include "../components/BodySlideBatch.h"
#include "../components/BodySlideBuildPlan.h"
#include "../program/BodySlideApp.h"
#include "../utils/ProjectUtil.h"

#include <filesystem>
#include <cmath>
#include <chrono>
#include <set>
#include <wx/dialog.h>
#include <wx/utils.h>

using nlohmann::json;
namespace fs = std::filesystem;
extern ConfigurationManager BodySlideConfig;

namespace {
std::string AbsolutePath(const std::string& value) {
	wxFileName path(wxString::FromUTF8(value));
	path.MakeAbsolute(wxString::FromUTF8(Config["AppDir"]));
	path.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
	return path.GetFullPath().ToUTF8().data();
}

std::string AssetPath(const std::string& root, std::string relative) {
	BodySlideOutputKey(relative);
	std::replace(relative.begin(), relative.end(), '\\', '/');
	const fs::path path = (fs::u8path(root) / fs::u8path(relative)).lexically_normal();
	const auto resolvedRoot = fs::weakly_canonical(fs::u8path(root)).generic_u8string();
	const auto resolvedPath = fs::weakly_canonical(path).generic_u8string();
	const wxString prefix = wxString::FromUTF8(resolvedRoot).Lower() + (resolvedRoot.back() == '/' ? "" : "/");
	if (!wxString::FromUTF8(resolvedPath).Lower().StartsWith(prefix))
		throw McpToolError("Output escapes targetDir through a link: " + path.u8string());
	return path.u8string();
}

std::string CanonicalKey(const std::string& path) {
	return wxString::FromUTF8(fs::weakly_canonical(fs::u8path(path)).generic_u8string()).Lower().ToUTF8().data();
}

// File metadata also records absent dependencies. New/deleted project resources
// invalidate a plan, including changes to inherited projects and stacked add-ons.
using FileStamp = std::map<std::string, std::pair<uintmax_t, int64_t>>;
void StampFile(FileStamp& stamp, const fs::path& path) {
	if (fs::is_regular_file(path))
		stamp[path.u8string()] = {fs::file_size(path), fs::last_write_time(path).time_since_epoch().count()};
	else
		stamp[path.u8string()] = {0, 0};
}
FileStamp Snapshot(const std::string& root, const std::set<std::string>& extra) {
	FileStamp stamp;
	for (const char* folder : {"SliderSets", "ShapeData", "SliderPresets", "SliderGroups"}) {
		fs::path dir = fs::u8path(root) / folder;
		StampFile(stamp, dir);
		if (fs::is_directory(dir))
			for (const auto& file : fs::recursive_directory_iterator(dir))
				if (file.is_regular_file())
					StampFile(stamp, file.path());
	}
	for (const auto& file : extra)
		StampFile(stamp, fs::u8path(file));
	return stamp;
}

std::map<std::string, std::string> Choices(const json& args) {
	std::map<std::string, std::string> result;
	if (const auto* value = McpArgs::Find(args, "choices")) {
		if (!value->is_object())
			McpArgs::Wrong("choices", "an object of relative output filenames to outfit names");
		for (const auto& item : value->items()) {
			if (!item.value().is_string())
				McpArgs::Wrong("choices", "an object of relative output filenames to outfit names");
			result[item.key()] = item.value().get<std::string>();
		}
	}
	return result;
}

json ZapChoices(BuildSelection selection) {
	json zaps = json::object();
	for (const auto& zap : selection.GetZapChoices())
		zaps[zap.first.first][zap.first.second] = zap.second;
	return zaps;
}

class BuildLog : public wxLog {
public:
	json entries = json::array();
	std::shared_ptr<std::string> outfit;
protected:
	void DoLogRecord(wxLogLevel level, const wxString& message, const wxLogRecordInfo&) override {
		if (level <= wxLOG_Warning)
			entries.push_back({{"level", level <= wxLOG_Error ? "error" : "warning"}, {"text", message.ToUTF8().data()},
				{"outfit", outfit ? *outfit : ""}});
	}
};
}

struct BodySlideMcp::Plan {
	std::string id, projectPath, targetDir;
	std::string skeleton;
	TargetGame game;
	bool tri = false, forceNormals = false, consumed = false;
	BodySlideBatchOptions options;
	std::vector<BodySlideBuildCandidate> candidates;
	std::map<std::string, std::string> saved, choices;
	std::vector<std::string> prefer;
	BodySlideBuildResolution resolution;
	json outfits = json::array();
	json errors = json::array();
	std::set<std::string> dependencies;
	FileStamp stamp;
};

BodySlideMcp::BodySlideMcp(BodySlideApp& inApp)
	: app(inApp), protocol(registry, "bodyslide", "1.0.0",
		"Controls the open BodySlide window. Start with get_status and list_outfits/list_groups/list_presets. "
		"plan_build snapshots a preset, zap choices and build options and lists actual output conflicts. "
		"resolve_conflicts returns a new plan; explicit choices override saved choices, then ordered prefer rules apply. "
		"Unresolved or contradictory winners block builds. build_batch executes a server-owned plan once and returns "
		"per-outfit results and files actually written. It is synchronous and reports MCP progress. "
		"get_build_report retrieves the last result even if the client disconnected. Plan IDs expire when BodySlide closes. "
		"Builds write to targetDir, including configured game output when selected; existing files may be overwritten. "
		"UI slider edits are ignored. Planning and resolution do not write files or change saved build selections."), server(protocol) {
	auto owner = &app;
	auto weak = std::make_shared<std::weak_ptr<MainThreadDispatcher>>();
	dispatcher = std::make_shared<MainThreadDispatcher>([owner, weak]() {
		owner->CallAfter([weak]() { if (auto live = weak->lock()) live->ProcessPending(); });
	});
	*weak = dispatcher;
	RegisterTools();
}

BodySlideMcp::~BodySlideMcp() { Stop(); }

bool BodySlideMcp::Start(int port, const std::string& token, std::string& error) {
	*stopping = false;
	protocol.SetToken(token);
	return server.Start(port, error);
}

void BodySlideMcp::Stop() {
	*stopping = true;
	server.Stop([this]() { dispatcher->ProcessPending(); });
}

void BodySlideMcp::AddTool(McpTool tool) {
	auto handler = std::move(tool.handler);
	auto progressHandler = std::move(tool.progressHandler);
	auto stop = stopping;
	tool.progressHandler = [this, stop, handler, progressHandler](const json& args, const McpProgress& progress) {
		auto result = std::make_shared<McpToolResult>();
		auto state = dispatcher->Run([this, stop, handler, progressHandler, args, progress, result]() {
			if (*stop)
				throw McpToolError("BodySlide MCP is stopping.");
			if (busy || !app.sliderView || !app.sliderView->IsEnabled() || app.sliderView->delayLoad.IsRunning())
				throw McpToolError("BodySlide is busy. Retry after the current operation finishes.");
			for (auto* window : wxTopLevelWindows)
				if (auto* dialog = wxDynamicCast(window, wxDialog); dialog && dialog->IsModal())
					throw McpToolError("Close the open BodySlide dialog before calling tools.");
			struct BusyScope {
				bool& busy;
				explicit BusyScope(bool& value) : busy(value) { busy = true; }
				~BusyScope() { busy = false; }
			} scope(busy);
			*result = progressHandler ? progressHandler(args, progress) : handler(args);
		}, std::chrono::seconds(30));
		if (state != MainThreadDispatcher::Result::Done)
			return McpToolResult::Error("BodySlide is busy or closing; the queued call was not started.");
		return *result;
	};
	registry.Add(std::move(tool));
}

std::shared_ptr<BodySlideMcp::Plan> BodySlideMcp::FindPlan(const json& args) {
	const std::string id = McpArgs::String(args, "planId");
	auto found = plans.find(id);
	if (found == plans.end())
		throw McpToolError("Unknown or expired planId. Call plan_build again.");
	return found->second;
}

void BodySlideMcp::StorePlan(const std::shared_ptr<Plan>& plan) {
	plans[plan->id] = plan;
	planOrder.push_back(plan->id);
	while (planOrder.size() > 16) {
		reports.erase(planOrder.front());
		plans.erase(planOrder.front());
		planOrder.pop_front();
	}
}

json BodySlideMcp::DescribePlan(const Plan& plan) {
	json errors = plan.errors;
	for (const auto& outfit : plan.outfits)
		if (std::find(plan.resolution.selected.begin(), plan.resolution.selected.end(), outfit["name"].get<std::string>()) != plan.resolution.selected.end())
			for (const auto& error : outfit["errors"])
				errors.push_back({{"outfit", outfit["name"]}, {"message", error}});
	return {{"planId", plan.id}, {"ready", errors.empty() && plan.resolution.Ready() && !plan.consumed},
		{"consumed", plan.consumed}, {"targetDir", plan.targetDir}, {"preset", plan.options.preset},
		{"tri", plan.tri}, {"forceNormals", plan.forceNormals}, {"conformAddOns", plan.options.conformAddOns},
		{"clippingFixStrength", plan.options.clippingFixStrength}, {"zaps", ZapChoices(plan.options.selection)},
		{"outfits", plan.outfits}, {"selected", plan.resolution.selected}, {"skipped", plan.resolution.skipped},
		{"conflicts", plan.resolution.conflicts}, {"errors", errors}};
}

McpToolResult BodySlideMcp::Catalog(const json& args) {
	const auto filter = McpArgs::String(args, "filter", "");
	const bool regex = McpArgs::Bool(args, "regex", false);
	std::vector<std::string> names;
	app.GetOutfits(names);
	std::string error;
	names = app.FilterOutfitNames(names, filter, regex, &error);
	if (!error.empty())
		throw McpToolError(error);
	int offset = McpArgs::Int(args, "offset", 0), limit = McpArgs::Int(args, "limit", 100);
	if (offset < 0 || limit < 1 || limit > 1000)
		throw McpToolError("offset must be nonnegative; limit must be between 1 and 1000.");
	json outfits = json::array();
	for (size_t i = static_cast<size_t>(offset); i < names.size() && outfits.size() < static_cast<size_t>(limit); ++i) {
		const auto* entry = app.projectCatalog.Find(names[i]);
		if (!entry)
			continue;
		std::vector<std::string> groups;
		app.gCollection.GetOutfitGroups(entry->setName, groups);
		json addOns = json::array();
		for (const auto* addOn : app.projectCatalog.AddOnsOf(entry->setName, entry->projectID, true))
			addOns.push_back(addOn->setName);
		outfits.push_back({{"name", entry->setName}, {"sourceFile", entry->fileName}, {"outputBase", entry->outputFilePath},
			{"groups", groups}, {"baseProject", entry->baseName}, {"addOns", addOns}});
	}
	McpToolResult result;
	result.data = {{"outfits", outfits}, {"total", names.size()}, {"offset", offset}};
	return result;
}

McpToolResult BodySlideMcp::PlanBuild(const json& args) {
	auto plan = std::make_shared<Plan>();
	plan->id = "plan-" + std::to_string(++nextId);
	plan->projectPath = ProjectUtil::GetProjectPath();
	plan->game = app.targetGame;
	plan->skeleton = Config["Anim/DefaultSkeletonReference"];
	if (!plan->skeleton.empty())
		plan->dependencies.insert(AbsolutePath(plan->skeleton));
	const auto before = Snapshot(plan->projectPath, {});
	plan->tri = McpArgs::Bool(args, "tri", false);
	plan->forceNormals = McpArgs::Bool(args, "forceNormals", false);
	plan->options.conformAddOns = McpArgs::Bool(args, "conformAddOns", false);
	plan->options.clippingFixStrength = McpArgs::Float(args, "clippingFixStrength", 0.0f);
	if (!std::isfinite(plan->options.clippingFixStrength) || plan->options.clippingFixStrength < 0 || plan->options.clippingFixStrength > 100)
		throw McpToolError("clippingFixStrength must be between 0 and 100.");
	plan->targetDir = McpArgs::String(args, "targetDir", app.GetOutputDataPath());
	if (plan->targetDir.empty())
		throw McpToolError("Configure an output path or pass targetDir.");
	plan->targetDir = AbsolutePath(plan->targetDir);
	BuildSelectionFile selectionFile;
	app.GetBuildSelection(selectionFile, plan->options.selection);
	plan->saved = plan->options.selection.GetOutputChoices();
	plan->choices = Choices(args);
	plan->prefer = McpArgs::Strings(args, "prefer");

	std::vector<std::string> groups;
	plan->options.preset = McpArgs::String(args, "preset", BodySlideConfig["SelectedPreset"]);
	const std::string presetFile = McpArgs::String(args, "presetFile", "");
	if (!presetFile.empty()) {
		const std::string path = AbsolutePath(presetFile);
		plan->dependencies.insert(path);
		if (!plan->options.sliders.LoadPresetFile(path, "", groups, true))
			throw McpToolError("Cannot load presetFile: " + path);
	}
	plan->options.sliders.LoadPresets(plan->projectPath + "/SliderPresets", "", groups, true);
	std::vector<std::string> presets;
	plan->options.sliders.GetPresetNames(presets);
	if (!plan->options.preset.empty() && std::find(presets.begin(), presets.end(), plan->options.preset) == presets.end())
		throw McpToolError("Unknown preset '" + plan->options.preset + "'. list_presets lists installed names; preset: \"\" uses project defaults.");

	const auto requested = McpArgs::Strings(args, "outfits");
	const auto requestedGroups = McpArgs::Strings(args, "groups");
	const std::string filter = McpArgs::String(args, "filter", "");
	if (requested.empty() && requestedGroups.empty() && filter.empty() && !McpArgs::Bool(args, "all", false))
		throw McpToolError("Select outfits, groups, a filter, or all: true.");
	std::set<std::string> selected;
	if (!requested.empty()) {
		for (const auto& name : requested) {
			const auto* entry = app.projectCatalog.Find(name);
			if (!entry)
				throw McpToolError("Unknown outfit: " + name);
			selected.insert(app.projectCatalog.StackRoot(entry->setName));
		}
	}
	else
		selected.insert(app.outfitNameOrder.begin(), app.outfitNameOrder.end());
	std::vector<std::string> knownGroups;
	app.GetAllGroupNames(knownGroups);
	for (const auto& group : requestedGroups)
		if (std::find(knownGroups.begin(), knownGroups.end(), group) == knownGroups.end())
			throw McpToolError("Unknown group: " + group);
	std::vector<std::string> names(selected.begin(), selected.end());
	std::string regexError;
	names = app.FilterOutfitNames(names, filter, McpArgs::Bool(args, "regex", false), &regexError);
	if (!regexError.empty())
		throw McpToolError(regexError);
	selected = {names.begin(), names.end()};
	const json zaps = args.value("zaps", json::object());
	if (!zaps.is_object())
		McpArgs::Wrong("zaps", "an object of outfit names to zap boolean maps");
	std::set<std::string> appliedZaps;
	for (const auto& name : app.outfitNameOrder) {
		if (!selected.count(name))
			continue;
		std::vector<std::string> outfitGroups;
		app.gCollection.GetOutfitGroups(name, outfitGroups);
		if (!requestedGroups.empty() && std::none_of(requestedGroups.begin(), requestedGroups.end(), [&](const auto& group) {
			return std::find(outfitGroups.begin(), outfitGroups.end(), group) != outfitGroups.end();
		}))
			continue;
		json info = {{"name", name}, {"sourceFile", app.outfitNameSource.at(name)}, {"groups", outfitGroups},
			{"errors", json::array()}, {"warnings", json::array()}, {"zaps", json::array()}};
		BodySlideBuildCandidate candidate;
		candidate.name = name;
		try {
			SliderSet set;
			BodySlideApp::ProjectAddOns addOns;
			if (app.LoadProjectSet(app.outfitNameSource.at(name), name, set, false, addOns))
				throw McpToolError("Cannot load slider set.");
			set.SetBaseDataPath(plan->projectPath + "/ShapeData");
			candidate.outputBase = set.GetOutputFilePath();
			BodySlideOutputKey(candidate.outputBase);
			candidate.files.push_back(candidate.outputBase + (set.GenWeights() ? "_0.nif" : ".nif"));
			if (set.GenWeights())
				candidate.files.push_back(candidate.outputBase + "_1.nif");
			if (plan->tri && !set.PreventMorphFile()) {
				if (plan->game == SF) {
					if (!set.GetSFMorphPath().empty() && !set.GetSFMorphTargetShape().empty())
						candidate.files.push_back(set.GetSFMorphPath() + "/morph.dat");
				}
				else
					candidate.files.push_back(candidate.outputBase + ".tri");
			}
			const std::string input = set.GetInputFileName();
			info["inputFile"] = input;
			plan->dependencies.insert(input);
			if (!fs::is_regular_file(fs::u8path(input)))
				info["errors"].push_back("Missing input NIF: " + input);
			if (set.GetResolveStatus().baseMissing)
				info["errors"].push_back("Missing base project: " + set.GetBaseProjectName());
			for (const auto& missing : addOns.missing)
				info["errors"].push_back("Missing add-on: " + missing);
			for (const auto& warning : set.GetResolveStatus().warnings)
				info["warnings"].push_back(warning);
			for (const auto& warning : addOns.warnings)
				info["warnings"].push_back(warning);
			for (size_t i = 0; i < set.size(); ++i) {
				if (set[i].bZap && !set[i].bHidden && !set[i].bUV)
					info["zaps"].push_back(set[i].name);
				for (const auto& data : set[i].dataFiles) {
					if (data.bGenerated)
						continue;
					auto resolved = set.ResolveSliderDataFile(data);
					if (!resolved.resolved)
						info["errors"].push_back("Missing slider data: " + data.fileName);
					else
						plan->dependencies.insert(resolved.resolvedPath);
				}
			}
			if (zaps.contains(name)) {
				appliedZaps.insert(name);
				json zapArgs = {{"values", zaps.at(name)}};
				for (const auto& zap : McpArgs::BoolMap(zapArgs, "values")) {
					if (std::find(info["zaps"].begin(), info["zaps"].end(), zap.first) == info["zaps"].end())
						throw McpToolError("Unknown visible zap '" + zap.first + "'.");
					plan->options.selection.SetZapChoice(name, zap.first, zap.second);
				}
			}
			json files = json::array();
			for (const auto& file : candidate.files) {
				std::string path = AssetPath(plan->targetDir, file);
				files.push_back({{"relativePath", file}, {"path", path}, {"exists", fs::exists(fs::u8path(path))}});
			}
			info["files"] = files;
			info["outputBase"] = candidate.outputBase;
		}
		catch (const std::exception& error) { info["errors"].push_back(error.what()); }
		plan->outfits.push_back(info);
		plan->candidates.push_back(std::move(candidate));
	}
	for (const auto& zap : zaps.items())
		if (!appliedZaps.count(zap.key()))
			throw McpToolError("Zap override does not name a selected outfit: " + zap.key());
	if (plan->candidates.empty())
		throw McpToolError("The selection matched no buildable outfits.");
	plan->resolution = ResolveBodySlideBuild(plan->candidates, plan->saved, plan->choices, plan->prefer);
	if (Snapshot(plan->projectPath, {}) != before)
		throw McpToolError("Project resources changed during planning. Create a new plan.");
	plan->stamp = Snapshot(plan->projectPath, plan->dependencies);
	StorePlan(plan);
	McpToolResult result;
	result.data = DescribePlan(*plan);
	return result;
}

McpToolResult BodySlideMcp::Resolve(const json& args) {
	auto source = FindPlan(args);
	if (source->consumed)
		throw McpToolError("This plan was already executed. Create a new plan.");
	auto plan = std::make_shared<Plan>(*source);
	plan->id = "plan-" + std::to_string(++nextId);
	for (const auto& choice : Choices(args))
		plan->choices[choice.first] = choice.second;
	if (McpArgs::Has(args, "prefer"))
		plan->prefer = McpArgs::Strings(args, "prefer");
	plan->resolution = ResolveBodySlideBuild(plan->candidates, plan->saved, plan->choices, plan->prefer);
	StorePlan(plan);
	McpToolResult result;
	result.data = DescribePlan(*plan);
	return result;
}

McpToolResult BodySlideMcp::Build(const json& args, const McpProgress& progress) {
	auto plan = FindPlan(args);
	json description = DescribePlan(*plan);
	if (!description["ready"].get<bool>())
		throw McpToolError("Plan is not ready or was already executed. Resolve its conflicts and errors first.");
	if (app.targetGame != plan->game || Config["Anim/DefaultSkeletonReference"] != plan->skeleton || ProjectUtil::GetProjectPath() != plan->projectPath ||
		Snapshot(plan->projectPath, plan->dependencies) != plan->stamp)
		throw McpToolError("Build inputs or game changed. Create a new plan before building.");
	for (const auto& outfit : plan->outfits)
		if (std::find(plan->resolution.selected.begin(), plan->resolution.selected.end(), outfit["name"].get<std::string>()) != plan->resolution.selected.end() && !outfit["errors"].empty())
			throw McpToolError("Selected outfit has preflight errors: " + outfit["name"].get<std::string>());
	std::set<std::string> inputs;
	for (const auto& dependency : plan->stamp)
		inputs.insert(CanonicalKey(dependency.first));
	for (const auto& candidate : plan->candidates)
		if (std::find(plan->resolution.selected.begin(), plan->resolution.selected.end(), candidate.name) != plan->resolution.selected.end())
			for (const auto& file : candidate.files) {
				const std::string output = AssetPath(plan->targetDir, file);
				if (inputs.count(CanonicalKey(output)))
					throw McpToolError("Output would overwrite a build input: " + output);
			}

	// Disable the window for the entire synchronous build. The unattended engine
	// does not yield, show dialogs, read UI sliders, or persist build selections.
	wxWindowDisabler disable;
	app.conformBodies.clear();
	app.conformedSets.clear();
	plan->consumed = true;
	const auto started = std::chrono::steady_clock::now();
	auto activeOutfit = std::make_shared<std::string>();
	plan->options.progress = [progress, activeOutfit](size_t done, size_t total, const std::string& name) {
		*activeOutfit = name;
		if (progress)
			progress(static_cast<double>(done), static_cast<double>(total), name);
	};
	json report = {{"planId", plan->id}, {"targetDir", plan->targetDir}, {"preset", plan->options.preset},
		{"status", "running"}, {"outfits", json::array()}, {"skipped", plan->resolution.skipped}, {"conflicts", plan->resolution.conflicts}};
	reports[plan->id] = report;
	auto capture = new BuildLog();
	capture->outfit = activeOutfit;
	std::map<std::string, std::string> failures;
	auto selected = plan->resolution.selected;
	{
		wxLogChain chain(capture);
		try {
			fs::create_directories(fs::u8path(plan->targetDir));
			std::string target = plan->targetDir + PathSepStr;
			app.BuildListBodies(selected, failures, false, plan->tri, plan->forceNormals, target, &plan->options);
		}
		catch (const std::exception& error) {
			for (const auto& name : selected)
				failures[name] = error.what();
		}
		report["log"] = capture->entries;
	}
	plan->options.progress = {};
	size_t succeeded = 0;
	for (const auto& name : selected) {
		const auto failed = failures.find(name);
		json entry = {{"name", name}, {"status", failed == failures.end() ? "succeeded" : "failed"},
			{"files", plan->options.writtenFiles[name]}, {"warnings", json::array()}};
		for (const auto& message : report["log"])
			if (message["outfit"] == name)
				entry["warnings"].push_back(message);
		if (failed != failures.end())
			entry["error"] = failed->second;
		else
			++succeeded;
		report["outfits"].push_back(entry);
	}
	report["succeeded"] = succeeded;
	report["failed"] = selected.size() - succeeded;
	report["status"] = succeeded == selected.size() ? "succeeded" : (succeeded == 0 ? "failed" : "partial");
	report["elapsedSeconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	reports[plan->id] = report;
	McpToolResult result;
	result.data = report;
	result.isError = succeeded != selected.size();
	return result;
}

void BodySlideMcp::RegisterTools() {
	McpTool status;
	status.name = "get_status";
	status.description = "BodySlide game, project and output paths, current UI selection and server URL.";
	status.readOnly = true;
	status.handler = [this](const json&) {
		McpToolResult result;
		result.data = {{"server", "bodyslide"}, {"url", GetUrl()}, {"targetGame", static_cast<int>(app.targetGame)},
			{"projectPath", ProjectUtil::GetProjectPath()}, {"outputDataPath", app.GetOutputDataPath()},
			{"selectedOutfit", BodySlideConfig["SelectedOutfit"]}, {"selectedPreset", BodySlideConfig["SelectedPreset"]}};
		return result;
	};
	AddTool(std::move(status));

	McpTool outfits;
	outfits.name = "list_outfits";
	outfits.description = "Installed buildable outfits with OSP sources, groups, base projects, stacked add-ons and output bases. Paginated.";
	outfits.readOnly = true;
	outfits.inputSchema = McpSchema({{"filter", McpProp("string", "Case-insensitive name substring")},
		{"regex", McpProp("boolean", "Interpret filter as a regular expression")}, {"offset", McpProp("integer", "First result, default 0")},
		{"limit", McpProp("integer", "Page size 1..1000, default 100")}});
	outfits.handler = [this](const json& args) { return Catalog(args); };
	AddTool(std::move(outfits));

	McpTool groups;
	groups.name = "list_groups";
	groups.description = "Installed BodySlide groups and their member outfit names.";
	groups.readOnly = true;
	groups.handler = [this](const json&) {
		McpToolResult result;
		result.data["groups"] = json::array();
		std::vector<std::string> names;
		app.GetAllGroupNames(names);
		for (const auto& name : names) {
			std::vector<std::string> members;
			app.gCollection.GetGroupMembers(name, members);
			result.data["groups"].push_back({{"name", name}, {"outfits", members}});
		}
		return result;
	};
	AddTool(std::move(groups));

	McpTool presets;
	presets.name = "list_presets";
	presets.description = "Installed presets with source XML files and assigned groups. Does not change the UI preset.";
	presets.readOnly = true;
	presets.handler = [](const json&) {
		PresetCollection collection;
		std::vector<std::string> groups, names;
		collection.LoadPresets(ProjectUtil::GetProjectPath() + "/SliderPresets", "", groups, true);
		collection.GetPresetNames(names);
		McpToolResult result;
		result.data["presets"] = json::array();
		for (const auto& name : names) {
			collection.GetPresetGroups(name, groups);
			result.data["presets"].push_back({{"name", name}, {"sourceFile", collection.GetPresetFileName(name)}, {"groups", groups}});
		}
		return result;
	};
	AddTool(std::move(presets));

	McpTool selection;
	selection.name = "get_build_selection";
	selection.description = "Saved output winners and per-project zap choices from BuildSelection.xml.";
	selection.readOnly = true;
	selection.handler = [this](const json&) {
		BuildSelectionFile file;
		BuildSelection choices;
		app.GetBuildSelection(file, choices);
		McpToolResult result;
		result.data = {{"outputChoices", choices.GetOutputChoices()}, {"zaps", ZapChoices(choices)}};
		return result;
	};
	AddTool(std::move(selection));

	json resolutionProps = {{"choices", {{"type", "object"}, {"additionalProperties", {{"type", "string"}}},
		{"description", "Actual relative conflict filenames to exact winner outfit names"}}},
		{"prefer", McpStringList("Ordered case-insensitive outfit-name substrings. Only a unique match selects a winner; saved choices take precedence.")}};
	McpTool plan;
	plan.name = "plan_build";
	plan.description = "Preflight a batch without writing files: snapshot preset/zaps/options, select outfits, list actual outputs, missing inputs and conflicts. No automatic first choice.";
	plan.readOnly = true;
	json props = resolutionProps;
	props.update({{"outfits", McpStringList("Exact outfit names; stacked add-ons resolve to their buildable base")},
		{"groups", McpStringList("Keep members of any of these groups; intersects outfits and filter")},
		{"filter", McpProp("string", "Case-insensitive substring of outfit names")}, {"regex", McpProp("boolean", "Filter is a regular expression")},
		{"all", McpProp("boolean", "Explicitly select the whole buildable catalog")},
		{"preset", McpProp("string", "Preset name; default current selection. Empty string uses project defaults.")},
		{"presetFile", McpProp("string", "Optional preset XML, absolute or relative to BodySlide")},
		{"targetDir", McpProp("string", "Output root; default configured output path. Existing outputs may be overwritten.")},
		{"tri", McpProp("boolean", "Build TRI or Starfield morph.dat, default false")},
		{"forceNormals", McpProp("boolean", "Force body normals, default false")},
		{"conformAddOns", McpProp("boolean", "Conform missing body add-on sliders, default false")},
		{"clippingFixStrength", McpProp("number", "Clipping fix strength 0..100, default 0")},
		{"zaps", {{"type", "object"}, {"additionalProperties", McpBoolMap("Visible zap names to enabled flags")},
			{"description", "Per-outfit visible zap overrides, on top of saved choices"}}}});
	plan.inputSchema = McpSchema(props);
	plan.handler = [this](const json& args) { return PlanBuild(args); };
	AddTool(std::move(plan));

	McpTool resolve;
	resolve.name = "resolve_conflicts";
	resolve.description = "Return a new plan with additional explicit conflict choices or replacement preference rules. Does not write BuildSelection.xml.";
	resolve.readOnly = true;
	resolutionProps["planId"] = McpProp("string", "Server-owned plan ID");
	resolve.inputSchema = McpSchema(resolutionProps, {"planId"});
	resolve.handler = [this](const json& args) { return Resolve(args); };
	AddTool(std::move(resolve));

	McpTool build;
	build.name = "build_batch";
	build.description = "Execute a ready plan once. Refuses stale inputs and unresolved conflicts. Synchronous with MCP progress; returns per-outfit success/failure, actual written files and warnings. Retrieve the report by planId after a disconnect.";
	build.destructive = true;
	build.inputSchema = McpSchema({{"planId", McpProp("string", "Ready plan ID returned by plan_build or resolve_conflicts")}}, {"planId"});
	build.progressHandler = [this](const json& args, const McpProgress& progress) { return Build(args, progress); };
	AddTool(std::move(build));

	McpTool report;
	report.name = "get_build_report";
	report.description = "Retrieve the report of an executed plan, including a partially failed build.";
	report.readOnly = true;
	report.inputSchema = McpSchema({{"planId", McpProp("string", "Executed plan ID")}}, {"planId"});
	report.handler = [this](const json& args) {
		auto found = reports.find(McpArgs::String(args, "planId"));
		if (found == reports.end())
			throw McpToolError("No report for that planId.");
		McpToolResult result;
		result.data = found->second;
		return result;
	};
	AddTool(std::move(report));
}
