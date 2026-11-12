#include "../src/components/BodySlideBuildPlan.h"
#include "../src/components/SliderSet.h"
#include "../src/files/TriFile.h"
#include "NifFile.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>

namespace {
std::vector<BodySlideBuildCandidate> Variants() {
	return {{"Armor", "meshes/armor/body", {"meshes/armor/body_0.nif", "meshes/armor/body_1.nif", "meshes/armor/body.tri"}},
		{"Armor Physics", "MESHES\\armor\\body", {"MESHES\\armor\\body_0.nif", "MESHES\\armor\\body_1.nif", "meshes/armor/body.tri"}},
		{"Boots", "meshes/boots", {"meshes/boots.nif"}}};
}
}

TEST_CASE("BodySlide never picks the first unresolved output variant", "[bodyslide][build-plan]") {
	auto result = ResolveBodySlideBuild(Variants(), {}, {}, {});
	REQUIRE_FALSE(result.Ready());
	REQUIRE(result.conflicts.size() == 3);
	for (const auto& conflict : result.conflicts)
		REQUIRE(conflict["winner"].is_null());
}

TEST_CASE("BodySlide uses saved choices, with explicit choices taking precedence", "[bodyslide][build-plan]") {
	auto result = ResolveBodySlideBuild(Variants(), {{"meshes/armor/body", "Armor"}}, {}, {"Physics"});
	REQUIRE(result.Ready());
	REQUIRE(result.selected == std::vector<std::string>{"Armor", "Boots"});
	REQUIRE(result.skipped == std::vector<std::string>{"Armor Physics"});
	std::map<std::string, std::string> choices;
	for (const auto& c : result.conflicts)
		choices[c["path"]] = "Armor Physics";
	result = ResolveBodySlideBuild(Variants(), {{"meshes/armor/body", "Armor"}}, choices, {});
	REQUIRE(result.selected == std::vector<std::string>{"Armor Physics", "Boots"});
}

TEST_CASE("BodySlide preferences require a unique highest-priority match", "[bodyslide][build-plan]") {
	auto result = ResolveBodySlideBuild(Variants(), {{"meshes/armor/body", "Removed outfit"}}, {}, {"Physics"});
	REQUIRE(result.Ready());
	REQUIRE(result.selected == std::vector<std::string>{"Armor Physics", "Boots"});
	result = ResolveBodySlideBuild(Variants(), {}, {}, {"Armor", "Physics"});
	REQUIRE_FALSE(result.Ready());
}

TEST_CASE("BodySlide rejects contradictory winners across low high and morph files", "[bodyslide][build-plan]") {
	REQUIRE_THROWS(ResolveBodySlideBuild(Variants(), {}, {{"meshes/armor/body_0.nif", "Armor"},
		{"meshes/armor/body_1.nif", "Armor Physics"}}, {}));
	REQUIRE_THROWS(ResolveBodySlideBuild(Variants(), {}, {{"meshes/armor/body_0.nif", "Boots"}}, {}));
	REQUIRE_THROWS(ResolveBodySlideBuild(Variants(), {}, {{"meshes/not-a-conflict.nif", "Armor"}}, {}));
}

TEST_CASE("BodySlide detects collisions between weighted and single-mesh projects", "[bodyslide][build-plan]") {
	std::vector<BodySlideBuildCandidate> candidates = {
		{"Weighted", "meshes/body", {"meshes/body_0.nif", "meshes/body_1.nif"}},
		{"Single", "meshes/body_1", {"meshes/body_1.nif"}}};
	auto result = ResolveBodySlideBuild(candidates, {}, {}, {});
	REQUIRE_FALSE(result.Ready());
	REQUIRE(result.conflicts.size() == 1);
	result = ResolveBodySlideBuild(candidates, {}, {{"meshes/body_1.nif", "Single"}}, {});
	REQUIRE(result.selected == std::vector<std::string>{"Single"});
	REQUIRE(result.skipped == std::vector<std::string>{"Weighted"});
}

TEST_CASE("BodySlide normalizes paths and refuses output path escapes", "[bodyslide][build-plan]") {
	REQUIRE(BodySlideOutputKey("Meshes\\Armor//./Body_1.NIF") == "meshes/armor/body_1.nif");
	for (const auto& path : {"", "/tmp/a.nif", "C:\\a.nif", "meshes/../a.nif", "\\\\server\\a.nif", "meshes/a.nif:stream", "meshes./a.nif", "meshes/CON.nif", "meshes/LPT1.nif"})
		REQUIRE_THROWS(BodySlideOutputKey(path));
}

// Used by BodySlideMcpSmokeTest.py to exercise the real batch pipeline, with
// known geometry, morph data and a zap instead of relying on installed game mods.
TEST_CASE("Write BodySlide MCP integration fixture", "[.bodyslide-fixture]") {
	const char* directory = std::getenv("BSOS_MCP_FIXTURE_DIR");
	if (!directory)
		SKIP("Set BSOS_MCP_FIXTURE_DIR to generate live MCP test inputs.");
	namespace fs = std::filesystem;
	fs::path root = fs::u8path(directory);
	fs::create_directories(root / "ShapeData" / "Smoke");
	fs::create_directories(root / "SliderSets");
	nifly::NifFile nif;
	nif.Create(nifly::NiVersion::getSSE());
	std::vector<nifly::Vector3> verts = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
	std::vector<nifly::Triangle> triangles = {{0, 1, 2}, {0, 2, 3}};
	std::vector<nifly::Vector2> uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
	nif.CreateShapeFromData("Body", &verts, &triangles, &uvs);
	REQUIRE(nif.Save(root / "ShapeData" / "Smoke" / "body.nif") == 0);
	DiffDataSets data;
	data.AddEmptySet("Grow", "Body");
	data.AddEmptySet("HideCorner", "Body");
	data.UpdateDiff("Grow", "Body", 0, nifly::Vector3(0, 0, 1));
	data.UpdateDiff("HideCorner", "Body", 3, nifly::Vector3(0, 1, 0));
	REQUIRE(data.SaveSet("Grow", "Body", (root / "ShapeData" / "Smoke" / "grow.bsd").u8string()) == 0);
	REQUIRE(data.SaveSet("HideCorner", "Body", (root / "ShapeData" / "Smoke" / "hide.bsd").u8string()) == 0);
	SliderSetFile file;
	file.New((root / "SliderSets" / "Smoke.osp").u8string());
	for (const auto& name : {"Smoke", "Smoke Physics", "Smoke Broken"}) {
		SliderSet set;
		set.SetName(name);
		set.SetDataFolder("Smoke");
		set.SetInputFile(std::string(name) == "Smoke Broken" ? "broken.nif" : "body.nif");
		set.SetOutputPath("meshes/smoke");
		set.SetOutputFile(std::string(name) == "Smoke Broken" ? "broken" : "body");
		set.SetGenWeights(true);
		set.AddShapeTarget("Body", "Body");
		auto& grow = set[set.CreateSlider("Grow")];
		grow.AddDataFile("Body", "Grow", "grow.bsd", true);
		auto& zap = set[set.CreateSlider("HideCorner")];
		zap.bZap = true;
		zap.AddDataFile("Body", "HideCorner", "hide.bsd", true);
		file.UpdateSet(set);
	}
	REQUIRE(file.Save());
}

TEST_CASE("Verify BodySlide MCP generated geometry and morphs", "[.bodyslide-verify]") {
	const char* directory = std::getenv("BSOS_MCP_FIXTURE_DIR");
	if (!directory)
		SKIP("Set BSOS_MCP_FIXTURE_DIR to inspect live MCP build outputs.");
	const auto output = std::filesystem::u8path(directory) / "Output" / "meshes" / "smoke";
	for (const auto& name : {"body_0.nif", "body_1.nif"}) {
		nifly::NifFile nif;
		REQUIRE(nif.Load(output / name) == 0);
		std::vector<nifly::Vector3> verts;
		REQUIRE(nif.GetVertsForShape(nif.FindBlockByName<nifly::NiShape>("Body"), verts));
		REQUIRE(verts.size() == 3); // The saved zap override removed the fourth vertex.
		REQUIRE(verts[0].z == Catch::Approx(0.5f)); // The explicit preset applied Grow at 50%.
	}
	TriFile tri;
	REQUIRE(tri.Read((output / "body.tri").u8string()));
	auto grow = tri.GetMorph("Body", "Grow");
	REQUIRE(grow);
	REQUIRE(grow->offsets.at(0).z == Catch::Approx(1.0f));
}
