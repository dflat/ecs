#pragma once
#include "world.hpp"

#include <functional>
#include <string>
#include <vector>

namespace ecs {

/**
 * @brief Manages a collection of systems (logic functions) to be executed sequentially.
 * @details Systems are simple functions that operate on the World. This registry provides
 * a way to organize and run them in a loop.
 */
class SystemRegistry {
public:
    using SystemFunc = std::function<void(World&)>;

    /**
     * @brief Registers a new system.
     * @param name Diagnostic name for the system.
     * @param fn The system function `void(World&)`.
     */
    void add(std::string name, SystemFunc fn) {
        systems_.push_back({std::move(name), std::move(fn)});
    }

    /**
     * @brief Executes all registered systems in order.
     * @details Flushes deferred commands after each system to ensure consistency.
     * @param world The world to update.
     */
    void run_all(World& world) {
        for (auto& [name, fn] : systems_) {
            fn(world);
            world.flush_deferred();
        }
    }

private:
    std::vector<std::pair<std::string, SystemFunc>> systems_;
};

} // namespace ecs
