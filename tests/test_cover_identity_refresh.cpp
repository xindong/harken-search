#include "CoverIdentityRefreshState.h"
#include <iostream>
#include <stdexcept>

static int checks = 0;
static void expect(bool condition, const char* description) {
    ++checks;
    if (!condition) throw std::runtime_error(description);
}

int main() {
    try {
        aura::cover::IdentityRefreshState state;
        const auto first = state.request("cold.wav", 1, 0.0);
        expect(first != 0, "cold file queues discovery");
        for (int frame = 0; frame < 1000; ++frame)
            expect(state.request("cold.wav", 1, frame / 1000.0) == 0,
                   "repeated frames do not duplicate a slow pending disk probe");
        state.finish("cold.wav", 1, first, 2.0);
        expect(state.request("cold.wav", 1, 31.99) == 0,
               "completed positive/negative probe is retained for refresh interval");
        const auto refresh = state.request("cold.wav", 1, 32.0);
        expect(refresh != 0, "sidecar and source changes can be discovered on refresh");
        state.invalidate("cold.wav");
        const auto replaced = state.request("cold.wav", 1, 32.1);
        expect(!state.current("cold.wav", 1, refresh), "invalidated completion cannot publish");
        state.finish("cold.wav", 1, refresh, 100.0);
        expect(state.current("cold.wav", 1, replaced), "old completion cannot clear new pending probe");
        const auto switched = state.request("cold.wav", 2, 32.2);
        expect(switched != 0, "new search generation can replace cancelled pending work");
        expect(!state.current("cold.wav", 1, replaced), "old generation result is rejected");
        state.clear();
        const auto reset = state.request("cold.wav", 2, 32.3);
        expect(reset != switched, "cache reset never reuses outstanding ticket");
        expect(!state.current("cold.wav", 2, switched), "reset rejects late result");
        expect(state.current("cold.wav", 2, reset), "new request survives reset");
        state.finish("cold.wav", 2, reset, 32.4);
        state.invalidate("cold.wav");
        expect(state.request("cold.wav", 2, 32.5) != 0, "explicit refresh bypasses cooldown");
        aura::cover::IdentityRefreshState bounded;
        for (int i = 0; i < 10000; ++i)
            expect(bounded.request(std::to_string(i), 1, 0.0) != 0, "bounded admission");
        expect(bounded.request("overflow", 1, 0.0) == 0, "pending request storage is bounded");
        expect(bounded.request("new-generation", 2, 0.0) != 0, "cancelled generation does not starve new work");
        std::cout << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
