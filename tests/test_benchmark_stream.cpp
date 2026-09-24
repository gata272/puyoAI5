#include "ai/benchmark/chain_benchmark.h"

#include <cassert>
#include <string>
#include <utility>

namespace {

std::string normalizeField(std::string value, const std::string& field) {
    std::size_t pos = 0;
    while ((pos = value.find(field, pos)) != std::string::npos) {
        std::size_t begin = pos + field.size();
        std::size_t end = begin;
        while (end < value.size() &&
               ((value[end] >= '0' && value[end] <= '9') || value[end] == '.')) {
            ++end;
        }
        value.replace(begin, end - begin, "0");
        pos = begin + 1;
    }
    return value;
}

std::string normalizeTiming(std::string value) {
    value = normalizeField(std::move(value), "\"wallMs\":");
    value = normalizeField(std::move(value), "\"thinkMicros\":");
    value = normalizeField(std::move(value), "\"totalThinkMicros\":");
    return value;
}

int countOccurrences(const std::string& value, const std::string& needle) {
    int count = 0;
    std::size_t pos = 0;
    while ((pos = value.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

} // namespace

int main() {
    puyo::ChainBenchmarkConfig config;
    config.games = 2;
    config.turns = 3;
    config.seed = 20260908;
    config.depth = 2;
    config.beamWidth = 4;
    config.progress = false;
    config.recordDecisionLog = true;

    const std::string first = puyo::runChainBenchmarkGame(config, 0);
    const std::string second = puyo::runChainBenchmarkGame(config, 0);
    assert(!first.empty());
    assert(normalizeTiming(first) == normalizeTiming(second));
    assert(first.find("\"benchmarkVersion\":8") != std::string::npos);
    assert(first.find("\"game\":0") != std::string::npos);
    assert(first.find("\"turnLogs\":[") != std::string::npos);
    assert(first.find("\"gameOverReason\":") != std::string::npos);
    assert(countOccurrences(first, "\"turns\":") == 1);

    config.recordDecisionLog = false;
    const std::string compact = puyo::runChainBenchmarkGame(config, 1);
    assert(!compact.empty());
    assert(compact.find("\"recordDecisionLog\":false") != std::string::npos);
    assert(compact.find("\"turnLogs\":[]") != std::string::npos);
    assert(compact.find("\"game\":1") != std::string::npos);

    assert(puyo::runChainBenchmarkGame(config, -1).empty());
    assert(puyo::runChainBenchmarkGame(config, 2).empty());
    return 0;
}
