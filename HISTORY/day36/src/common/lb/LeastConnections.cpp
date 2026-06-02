#include "lb/LeastConnections.h"
#include "http/HttpRequest.h"
#include <climits>

int LeastConnections::pick(const std::vector<std::unique_ptr<Backend>> &backends,
                            const HttpRequest & /*req*/) {
    int best      = -1;
    int bestValue = INT_MAX;

    for (size_t i = 0; i < backends.size(); ++i) {
        if (!backends[i]->isAlive()) continue;
        int f = backends[i]->inflight.load(std::memory_order_relaxed);
        if (f < bestValue) {
            bestValue = f;
            best      = static_cast<int>(i);
        }
    }
    return best;
}
