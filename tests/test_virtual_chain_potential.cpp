#include "../ai/evaluation/virtual_chain_potential.h"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace puyo;

int main() {
    // B is a latent trigger and A is a 3+1 tail. A virtual two-puyo probe
    // should be able to discover the resulting two-wave chain even though no
    // real future pair is being assumed.
    Board relay;
    relay.set(0, 0, Cell::Blue);
    relay.set(0, 1, Cell::Blue);
    relay.set(0, 2, Cell::Blue);
    relay.set(1, 0, Cell::Red);
    relay.set(1, 1, Cell::Red);
    relay.set(1, 2, Cell::Red);
    relay.set(0, 3, Cell::Red);

    const VirtualChainFeatures f = analyzeVirtualChainPotential(relay);
    assert(f.bestChain >= 2);
    assert(f.top3ChainSum >= f.bestChain);
    assert(f.count2Plus >= 1);
    assert(f.count3Plus >= 0);
    assert(f.bestScore > 0);
    assert(std::isfinite(virtualChainPotentialScore(f, relay)));

    // Empty/very small boards intentionally skip the expensive virtual probe.
    Board empty;
    const VirtualChainFeatures e = analyzeVirtualChainPotential(empty);
    assert(e.bestChain == 0);
    assert(e.resultCount == 0);


    // Regression: mature 12/13/14-chain virtual boards must not collapse to
    // the same score because of an overly low upper clamp. The search needs
    // to distinguish the high-chain tail that v13 explicitly optimizes.
    VirtualChainFeatures f12;
    f12.bestChain = 12;
    f12.top3ChainSum = 34;
    f12.count2Plus = 6;
    f12.count3Plus = 6;

    VirtualChainFeatures f13 = f12;
    f13.bestChain = 13;
    f13.top3ChainSum = 35;

    VirtualChainFeatures f14 = f13;
    f14.bestChain = 14;
    f14.top3ChainSum = 36;

    const double s12 = virtualChainPotentialScore(f12, relay);
    const double s13 = virtualChainPotentialScore(f13, relay);
    const double s14 = virtualChainPotentialScore(f14, relay);
    assert(s13 > s12);
    assert(s14 > s13);
    assert(s14 < 1500000.0);

    std::cout << "virtual-chain potential tests passed: "
              << f.bestChain << "," << f.top3ChainSum << "\n";
    return 0;
}
