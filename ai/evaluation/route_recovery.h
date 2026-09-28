#pragma once
namespace puyo {
enum class RouteRecoveryStatus { None, Connected, Weak, Stale, Abandon };
const char* routeRecoveryStatusName(RouteRecoveryStatus status);
struct RouteRecoveryState { RouteRecoveryStatus status = RouteRecoveryStatus::None; int staleAge = 0; int deltaSafeMoves = 0; int deltaBestNextSafeMoves = 0; double score = 0.0; bool stale = false; };
RouteRecoveryState analyzeRouteRecovery(int routeLength, int trueTriggerPath, int safeMoves, int previousSafeMoves, int bestNextSafeMoves, int previousBestNextSafeMoves, int inheritedStaleAge, int productiveNextMoves, int productiveFollowupMoves, int bestImmediateNetClear, int bestRebuildChain, int bestRebuildNetClear, int bestRebuildNextSafeMoves, int maxHeight, bool rebuildPolicy);
} // namespace puyo
