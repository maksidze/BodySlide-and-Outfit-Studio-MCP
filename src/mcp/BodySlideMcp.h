/*
BodySlide and Outfit Studio
See the included LICENSE file
*/
#pragma once

#include "McpProtocol.h"
#include "McpServer.h"
#include "../utils/MainThreadDispatcher.h"
#include <atomic>
#include <deque>
#include <map>
#include <memory>

class BodySlideApp;
constexpr int BodySlideMcpDefaultPort = 47824;

// A separate server for the open BodySlide window. Plans and reports belong to
// this server instance and never accept a client-supplied executable build plan.
class BodySlideMcp {
public:
	explicit BodySlideMcp(BodySlideApp& app);
	~BodySlideMcp();
	bool Start(int port, const std::string& token, std::string& error);
	void Stop();
	bool IsRunning() const { return server.IsRunning(); }
	std::string GetUrl() const { return server.GetUrl(); }

private:
	struct Plan;
	BodySlideApp& app;
	McpToolRegistry registry;
	McpProtocol protocol;
	McpServer server;
	std::shared_ptr<MainThreadDispatcher> dispatcher;
	std::shared_ptr<std::atomic<bool>> stopping = std::make_shared<std::atomic<bool>>(false);
	bool busy = false;
	uint64_t nextId = 0;
	std::map<std::string, std::shared_ptr<Plan>> plans;
	std::deque<std::string> planOrder;
	std::map<std::string, nlohmann::json> reports;
	void AddTool(McpTool tool);
	void RegisterTools();
	std::shared_ptr<Plan> FindPlan(const nlohmann::json& args);
	void StorePlan(const std::shared_ptr<Plan>& plan);
	nlohmann::json DescribePlan(const Plan& plan);
	McpToolResult Catalog(const nlohmann::json& args);
	McpToolResult PlanBuild(const nlohmann::json& args);
	McpToolResult Resolve(const nlohmann::json& args);
	McpToolResult Build(const nlohmann::json& args, const McpProgress& progress);
};
