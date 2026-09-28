#include "../ai/evaluation/route_recovery.h"

#include <cassert>
#include <iostream>

using namespace puyo;

int main() {
    // A route remains healthy when the visible pair can continue it.
    const auto connected = analyzeRouteRecovery(
        8, 3, 4, 4, 5, 4, 0, 2, 2, 8, 3, 12, 5, 9, false);
    assert(connected.status == RouteRecoveryStatus::Connected);
    assert(!connected.stale);
    assert(connected.deltaSafeMoves == 0);
    assert(connected.deltaBestNextSafeMoves == 1);

    // A theoretical route with no visible trigger and low safety is stale.
    const auto stale = analyzeRouteRecovery(
        9, 0, 4, 6, 1, 3, 0, 0, 0, 0, 0, 0, 0, 11, false);
    assert(stale.status == RouteRecoveryStatus::Stale);
    assert(stale.stale);
    assert(stale.staleAge == 1);
    assert(stale.score < 0.0);

    // Persistence escalates stale -> abandon rather than rewarding the dead route.
    const auto abandon = analyzeRouteRecovery(
        9, 0, 3, 4, 1, 2, 2, 0, 0, 0, 0, 0, 0, 12, false);
    assert(abandon.status == RouteRecoveryStatus::Abandon);
    assert(abandon.staleAge == 3);
    assert(abandon.score < stale.score);

    // Recovery can improve safety even before the route fully reconnects.
    const auto recovery = analyzeRouteRecovery(
        7, 1, 5, 2, 4, 1, 0, 1, 3, 10, 4, 12, 7, 10, false);
    assert(recovery.status == RouteRecoveryStatus::Weak);
    assert(recovery.deltaSafeMoves == 3);
    assert(recovery.deltaBestNextSafeMoves == 3);
    assert(recovery.score > 0.0);

    std::cout << "route recovery tests passed\n";
}
