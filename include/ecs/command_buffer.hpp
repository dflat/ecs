#pragma once

#include "world.hpp"

#include <tuple>

namespace ecs {

class CommandBuffer {
public:
    void destroy(Entity e) { arena_.emplace<World::DestroyCmd>(World::DestroyCmd{e}); }

    template <typename T>
    void add(Entity e, T&& comp) {
        using U = std::decay_t<T>;
        arena_.emplace<World::AddCmd<U>>(World::AddCmd<U>{e, std::forward<T>(comp)});
    }

    template <typename T>
    void remove(Entity e) {
        arena_.emplace<World::RemoveCmd<std::decay_t<T>>>(World::RemoveCmd<std::decay_t<T>>{e});
    }

    template <typename... Ts>
    void create_with(Ts&&... comps) {
        using Cmd = World::CreateWithCmd<std::decay_t<Ts>...>;
        arena_.emplace<Cmd>(Cmd{std::tuple<std::decay_t<Ts>...>(std::forward<Ts>(comps)...)});
    }

    void flush(World& w) {
        auto cmds = std::move(arena_);
        cmds.execute_all(w);
    }

    bool empty() const { return arena_.empty(); }

private:
    World::CommandArena arena_;
};

} // namespace ecs
