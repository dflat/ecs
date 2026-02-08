#pragma once
#include "world.hpp"

#include <functional>
#include <string>
#include <vector>

namespace ecs {

class SystemRegistry {
public:
    using SystemFunc = std::function<void(World&)>;

    void add(std::string name, SystemFunc fn) {
        systems_.push_back({std::move(name), std::move(fn)});
    }

    void run_all(World& world) {
        for (auto& [name, fn] : systems_)
            fn(world);
    }

private:
    std::vector<std::pair<std::string, SystemFunc>> systems_;
};

} // namespace ecs
