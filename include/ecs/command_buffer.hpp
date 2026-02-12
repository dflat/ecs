#pragma once

#include "component.hpp"
#include "entity.hpp"

#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <vector>

namespace ecs {

class World; // forward declaration — flush() defined after World in world.hpp

/**
 * @brief Records structural changes to be executed later.
 *
 * @details The CommandBuffer allows systems to queue operations like entity creation,
 * destruction, or component addition/removal while iterating over the World. This avoids
 * invalidating iterators or modifying the archetype graph during a query.
 *
 * Commands are stored in a linear byte buffer and executed in FIFO order when `flush()` is called.
 */
class CommandBuffer {
public:
    CommandBuffer() = default;

    /**
     * @brief Destructor.
     * @details Destroys any unflushed components stored in the buffer to prevent memory leaks.
     */
    ~CommandBuffer() { destroy_unflushed(); }

    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;
    CommandBuffer(CommandBuffer&&) = default;
    CommandBuffer& operator=(CommandBuffer&&) = default;

    /**
     * @brief Queues an entity for destruction.
     * @param e The entity to destroy.
     */
    void destroy(Entity e) {
        auto* hdr = write_header(CmdTag::Destroy, e, 0, 0, nullptr, nullptr, nullptr);
        (void)hdr;
    }

    /**
     * @brief Queues a component addition/update.
     * @tparam T The component type.
     * @param e The target entity.
     * @param comp The component value (moved into the buffer).
     */
    template <typename T>
    void add(Entity e, T&& comp) {
        using U = std::decay_t<T>;
        ensure_column_factory<U>();
        auto col = make_column<U>();
        write_header(CmdTag::Add, e, component_id<U>(), sizeof(U), col.move_fn, col.destroy_fn,
                     nullptr);
        void* dst = alloc_inline(sizeof(U), alignof(std::max_align_t));
        new (dst) U(std::forward<T>(comp));
    }

    /**
     * @brief Queues a component removal.
     * @tparam T The component type to remove.
     * @param e The target entity.
     */
    template <typename T>
    void remove(Entity e) {
        write_header(CmdTag::Remove, e, component_id<std::decay_t<T>>(), 0, nullptr, nullptr,
                     nullptr);
    }

    /**
     * @brief Queues the creation of a new entity with multiple components.
     * @tparam Ts Component types to initialize.
     * @param comps Component values (moved into the buffer).
     */
    template <typename... Ts>
    void create_with(Ts&&... comps) {
        static_assert(sizeof...(Ts) > 0, "create_with requires at least one component");
        (ensure_column_factory<std::decay_t<Ts>>(), ...);
        write_header(CmdTag::CreateWith, INVALID_ENTITY, 0, sizeof...(Ts), nullptr, nullptr,
                     nullptr);
        // Write one sub-entry per component
        (write_sub_entry<Ts>(std::forward<Ts>(comps)), ...);
    }

    /**
     * @brief Executes all queued commands on the specified World.
     * @param w The World to update.
     * @details Clears the buffer after execution.
     */
    void flush(World& w); // defined after World class in world.hpp

    /**
     * @brief Checks if the buffer contains any pending commands.
     * @return true if empty, false otherwise.
     */
    bool empty() const { return buf_.empty(); }

private:
    enum class CmdTag : uint8_t { Destroy, Add, Remove, CreateWith };

    struct CmdHeader {
        CmdTag tag;
        Entity entity;
        ComponentTypeID cid;
        size_t payload;
        ComponentColumn::MoveFunc move_fn;
        ComponentColumn::DestroyFunc destroy_fn;
    };

    struct SubEntry {
        ComponentTypeID cid;
        size_t elem_size;
        ComponentColumn::MoveFunc move_fn;
        ComponentColumn::DestroyFunc destroy_fn;
    };

    std::vector<uint8_t> buf_;

    static size_t align_up(size_t offset, size_t align) {
        return (offset + align - 1) & ~(align - 1);
    }

    void* alloc_raw(size_t size, size_t alignment) {
        size_t offset = align_up(buf_.size(), alignment);
        buf_.resize(offset + size);
        return buf_.data() + offset;
    }

    CmdHeader* write_header(CmdTag tag, Entity e, ComponentTypeID cid, size_t payload,
                            ComponentColumn::MoveFunc move_fn,
                            ComponentColumn::DestroyFunc destroy_fn, void* /*unused*/) {
        void* dst = alloc_raw(sizeof(CmdHeader), alignof(CmdHeader));
        auto* hdr = new (dst) CmdHeader{tag, e, cid, payload, move_fn, destroy_fn};
        return hdr;
    }

    void* alloc_inline(size_t size, size_t alignment) { return alloc_raw(size, alignment); }

    template <typename T>
    void write_sub_entry(T&& comp) {
        using U = std::decay_t<T>;
        auto col = make_column<U>();
        void* dst = alloc_raw(sizeof(SubEntry), alignof(SubEntry));
        new (dst) SubEntry{component_id<U>(), sizeof(U), col.move_fn, col.destroy_fn};
        void* data_dst = alloc_inline(sizeof(U), alignof(std::max_align_t));
        new (data_dst) U(std::forward<T>(comp));
    }

    void destroy_unflushed() {
        size_t pos = 0;
        while (pos < buf_.size()) {
            pos = align_up(pos, alignof(CmdHeader));
            if (pos + sizeof(CmdHeader) > buf_.size())
                break;
            auto* hdr = reinterpret_cast<CmdHeader*>(buf_.data() + pos);
            pos += sizeof(CmdHeader);

            switch (hdr->tag) {
            case CmdTag::Destroy:
            case CmdTag::Remove:
                break;
            case CmdTag::Add: {
                pos = align_up(pos, alignof(std::max_align_t));
                if (hdr->destroy_fn && pos + hdr->payload <= buf_.size())
                    hdr->destroy_fn(buf_.data() + pos);
                pos += hdr->payload;
                break;
            }
            case CmdTag::CreateWith: {
                size_t count = hdr->payload;
                for (size_t i = 0; i < count; ++i) {
                    pos = align_up(pos, alignof(SubEntry));
                    if (pos + sizeof(SubEntry) > buf_.size())
                        break;
                    auto* sub = reinterpret_cast<SubEntry*>(buf_.data() + pos);
                    pos += sizeof(SubEntry);
                    pos = align_up(pos, alignof(std::max_align_t));
                    if (sub->destroy_fn && pos + sub->elem_size <= buf_.size())
                        sub->destroy_fn(buf_.data() + pos);
                    pos += sub->elem_size;
                }
                break;
            }
            }
        }
        buf_.clear();
    }
};

} // namespace ecs
