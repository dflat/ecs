#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <new>
#include <utility>

namespace ecs {

using ComponentTypeID = uint32_t;

inline ComponentTypeID next_component_id() {
    static ComponentTypeID counter = 0;
    return counter++;
}

template <typename T>
ComponentTypeID component_id() {
    static ComponentTypeID id = next_component_id();
    return id;
}

// Type-erased column storage for a single component type within an archetype.
struct ComponentColumn {
    uint8_t* data = nullptr;
    size_t elem_size = 0;
    size_t count = 0;
    size_t capacity = 0;

    using MoveFunc = void (*)(void* dst, void* src);
    using DestroyFunc = void (*)(void* ptr);

    MoveFunc move_fn = nullptr;
    DestroyFunc destroy_fn = nullptr;

    ComponentColumn() = default;

    ComponentColumn(ComponentColumn&& o) noexcept
        : data(o.data),
          elem_size(o.elem_size),
          count(o.count),
          capacity(o.capacity),
          move_fn(o.move_fn),
          destroy_fn(o.destroy_fn) {
        o.data = nullptr;
        o.count = 0;
        o.capacity = 0;
    }

    ComponentColumn& operator=(ComponentColumn&& o) noexcept {
        if (this != &o) {
            destroy_all();
            std::free(data);
            data = o.data;
            elem_size = o.elem_size;
            count = o.count;
            capacity = o.capacity;
            move_fn = o.move_fn;
            destroy_fn = o.destroy_fn;
            o.data = nullptr;
            o.count = 0;
            o.capacity = 0;
        }
        return *this;
    }

    ~ComponentColumn() {
        destroy_all();
        std::free(data);
    }

    ComponentColumn(const ComponentColumn&) = delete;
    ComponentColumn& operator=(const ComponentColumn&) = delete;

    void grow() {
        size_t new_cap = capacity == 0 ? 16 : capacity * 2;
        uint8_t* new_data = static_cast<uint8_t*>(std::malloc(new_cap * elem_size));
        if (data) {
            for (size_t i = 0; i < count; ++i)
                move_fn(new_data + i * elem_size, data + i * elem_size);
            std::free(data);
        }
        data = new_data;
        capacity = new_cap;
    }

    void push_raw(void* src) {
        if (count == capacity)
            grow();
        move_fn(data + count * elem_size, src);
        ++count;
    }

    void swap_remove(size_t row) {
        if (row < count - 1) {
            destroy_fn(data + row * elem_size);
            move_fn(data + row * elem_size, data + (count - 1) * elem_size);
        } else {
            destroy_fn(data + row * elem_size);
        }
        --count;
    }

    void* get(size_t row) { return data + row * elem_size; }

    void destroy_all() {
        if (data && destroy_fn) {
            for (size_t i = 0; i < count; ++i)
                destroy_fn(data + i * elem_size);
        }
        count = 0;
    }
};

template <typename T>
ComponentColumn make_column() {
    ComponentColumn col;
    col.elem_size = sizeof(T);
    col.move_fn = [](void* dst, void* src) {
        new (dst) T(std::move(*static_cast<T*>(src)));
        static_cast<T*>(src)->~T();
    };
    col.destroy_fn = [](void* ptr) {
        static_cast<T*>(ptr)->~T();
    };
    return col;
}

// Global registry of column factories so we can create columns for types
// during archetype migration without knowing the concrete type.
using ColumnFactory = std::function<ComponentColumn()>;

inline std::map<ComponentTypeID, ColumnFactory>& column_factory_registry() {
    static std::map<ComponentTypeID, ColumnFactory> reg;
    return reg;
}

template <typename T>
void ensure_column_factory() {
    auto& reg = column_factory_registry();
    ComponentTypeID id = component_id<T>();
    if (reg.find(id) == reg.end()) {
        reg[id] = []() -> ComponentColumn {
            return make_column<T>();
        };
    }
}

} // namespace ecs
