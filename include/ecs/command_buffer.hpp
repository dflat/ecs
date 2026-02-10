#pragma once

#include "world.hpp"

#include <functional>
#include <memory>
#include <tuple>
#include <vector>

namespace ecs {

class CommandBuffer {
public:
    void destroy(Entity e) {
        commands_.push_back([e](World& w) { w.destroy(e); });
    }

    template <typename T>
    void add(Entity e, T&& comp) {
        auto ptr = std::make_shared<std::decay_t<T>>(std::forward<T>(comp));
        commands_.push_back([e, ptr](World& w) { w.add<std::decay_t<T>>(e, std::move(*ptr)); });
    }

    template <typename T>
    void remove(Entity e) {
        commands_.push_back([e](World& w) { w.remove<std::decay_t<T>>(e); });
    }

    template <typename... Ts>
    void create_with(Ts&&... comps) {
        auto data = std::make_shared<std::tuple<std::decay_t<Ts>...>>(std::forward<Ts>(comps)...);
        commands_.push_back([data](World& w) {
            std::apply([&w](auto&&... args) { w.create_with(std::move(args)...); },
                       std::move(*data));
        });
    }

    void flush(World& w) {
        auto cmds = std::move(commands_);
        for (auto& cmd : cmds)
            cmd(w);
    }

    bool empty() const { return commands_.empty(); }

private:
    std::vector<std::function<void(World&)>> commands_;
};

} // namespace ecs
