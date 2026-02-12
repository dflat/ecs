#pragma once

#include "component.hpp"
#include "entity.hpp"

#include <algorithm>
#include <cstdint>
#include <new>
#include <type_traits>
#include <vector>

namespace ecs {

class World; // forward declaration

/**
 * @brief A template for creating entities with a predefined set of components.
 *
 * @details Prefabs store a copy of component data. When instantiated, this data is copied
 * into the new entity. This is useful for spawning multiple similar objects (e.g., bullets, enemies).
 *
 * All components in a Prefab MUST be copy-constructible.
 */
class Prefab {
public:
    struct Entry {
        ComponentTypeID cid;
        size_t elem_size;
        size_t buf_offset; // offset of data in buf_
        ComponentColumn::MoveFunc move_fn;
        ComponentColumn::DestroyFunc destroy_fn;
        void (*copy_fn)(void* dst, const void* src);
    };

    Prefab() = default;

    ~Prefab() {
        for (auto& entry : entries_) {
            entry.destroy_fn(buf_.data() + entry.buf_offset);
        }
    }

    Prefab(const Prefab& o) : entries_(o.entries_), buf_(o.buf_.size()) {
        // Copy-construct each component from o's buffer into ours
        for (auto& entry : entries_) {
            entry.copy_fn(buf_.data() + entry.buf_offset, o.buf_.data() + entry.buf_offset);
        }
    }

    Prefab& operator=(const Prefab& o) {
        if (this != &o) {
            // Destroy current
            for (auto& entry : entries_)
                entry.destroy_fn(buf_.data() + entry.buf_offset);

            entries_ = o.entries_;
            buf_.resize(o.buf_.size());
            for (auto& entry : entries_) {
                entry.copy_fn(buf_.data() + entry.buf_offset, o.buf_.data() + entry.buf_offset);
            }
        }
        return *this;
    }

    Prefab(Prefab&& o) noexcept : entries_(std::move(o.entries_)), buf_(std::move(o.buf_)) {
        o.entries_.clear();
    }

    Prefab& operator=(Prefab&& o) noexcept {
        if (this != &o) {
            for (auto& entry : entries_)
                entry.destroy_fn(buf_.data() + entry.buf_offset);
            entries_ = std::move(o.entries_);
            buf_ = std::move(o.buf_);
            o.entries_.clear();
        }
        return *this;
    }

    /**
     * @brief Creates a new Prefab from a list of components.
     * @tparam Ts Component types.
     * @param components The component values to store as defaults.
     * @return The new Prefab.
     */
    template <typename... Ts>
    static Prefab create(Ts&&... components) {
        static_assert(sizeof...(Ts) > 0, "Prefab::create requires at least one component");
        (static_assert_copyable<std::decay_t<Ts>>(), ...);

        Prefab p;
        (p.add_component<std::decay_t<Ts>>(std::forward<Ts>(components)), ...);
        return p;
    }

    template <typename T>
    bool has() const {
        auto cid = component_id<T>();
        for (auto& entry : entries_) {
            if (entry.cid == cid)
                return true;
        }
        return false;
    }

    size_t component_count() const { return entries_.size(); }

    const std::vector<Entry>& entries() const { return entries_; }
    const uint8_t* data() const { return buf_.data(); }

private:
    std::vector<Entry> entries_;
    std::vector<uint8_t> buf_;

    template <typename T>
    static void static_assert_copyable() {
        static_assert(std::is_copy_constructible_v<T>,
                      "Prefab components must be copy-constructible");
    }

    static size_t align_up(size_t offset, size_t align) {
        return (offset + align - 1) & ~(align - 1);
    }

    template <typename T>
    void add_component(T&& comp) {
        using U = std::decay_t<T>;
        ensure_column_factory<U>();

        size_t offset = align_up(buf_.size(), alignof(std::max_align_t));
        buf_.resize(offset + sizeof(U));

        new (buf_.data() + offset) U(std::forward<T>(comp));

        auto col = make_column<U>();
        entries_.push_back(Entry{
            component_id<U>(),
            sizeof(U),
            offset,
            col.move_fn,
            col.destroy_fn,
            [](void* dst, const void* src) { new (dst) U(*static_cast<const U*>(src)); },
        });
    }
};

// Free function declarations — defined after World in world.hpp

/**
 * @brief Creates a new entity in the world using the prefab's components.
 * @param world The target world.
 * @param prefab The source prefab.
 * @return The new entity handle.
 */
Entity instantiate(World& world, const Prefab& prefab);

/**
 * @brief Creates a new entity from a prefab, overriding specific components.
 * @tparam Overrides Types of components to override or add.
 * @param world The target world.
 * @param prefab The source prefab.
 * @param overrides Component values to use instead of the prefab defaults.
 * @return The new entity handle.
 */
template <typename... Overrides>
Entity instantiate(World& world, const Prefab& prefab, Overrides&&... overrides);

} // namespace ecs
