#pragma once

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

#ifndef ECS_ASSERT
#define ECS_ASSERT(expr, msg) assert((expr) && (msg))
#endif

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
    size_t alignment = 1;

    using MoveFunc = void (*)(void* dst, void* src);
    using DestroyFunc = void (*)(void* ptr);
    using SwapFunc = void (*)(void* a, void* b);
    using SerializeFunc = void (*)(const void* elem, std::ostream& out);
    using DeserializeFunc = void (*)(void* elem, std::istream& in);

    MoveFunc move_fn = nullptr;
    DestroyFunc destroy_fn = nullptr;
    SwapFunc swap_fn = nullptr;
    SerializeFunc serialize_fn = nullptr;
    DeserializeFunc deserialize_fn = nullptr;

    ComponentColumn() = default;

    ComponentColumn(ComponentColumn&& o) noexcept
        : data(o.data),
          elem_size(o.elem_size),
          count(o.count),
          capacity(o.capacity),
          alignment(o.alignment),
          move_fn(o.move_fn),
          destroy_fn(o.destroy_fn),
          swap_fn(o.swap_fn),
          serialize_fn(o.serialize_fn),
          deserialize_fn(o.deserialize_fn) {
        o.data = nullptr;
        o.count = 0;
        o.capacity = 0;
    }

    ComponentColumn& operator=(ComponentColumn&& o) noexcept {
        if (this != &o) {
            destroy_all();
            // data is owned by Archetype's block_ — do NOT free here
            data = o.data;
            elem_size = o.elem_size;
            count = o.count;
            capacity = o.capacity;
            alignment = o.alignment;
            move_fn = o.move_fn;
            destroy_fn = o.destroy_fn;
            swap_fn = o.swap_fn;
            serialize_fn = o.serialize_fn;
            deserialize_fn = o.deserialize_fn;
            o.data = nullptr;
            o.count = 0;
            o.capacity = 0;
        }
        return *this;
    }

    ~ComponentColumn() {
        destroy_all();
        // data is owned by Archetype's block_ — do NOT free here
    }

    ComponentColumn(const ComponentColumn&) = delete;
    ComponentColumn& operator=(const ComponentColumn&) = delete;

    void push_raw(void* src) {
        ECS_ASSERT(count < capacity, "push_raw: column at capacity (archetype should have grown)");
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
    col.alignment = alignof(T);
    col.move_fn = [](void* dst, void* src) {
        new (dst) T(std::move(*static_cast<T*>(src)));
        static_cast<T*>(src)->~T();
    };
    col.destroy_fn = [](void* ptr) {
        static_cast<T*>(ptr)->~T();
    };
    col.swap_fn = [](void* a, void* b) {
        using std::swap;
        swap(*static_cast<T*>(a), *static_cast<T*>(b));
    };
    if constexpr (std::is_trivially_copyable_v<T>) {
        col.serialize_fn = [](const void* elem, std::ostream& out) {
            out.write(static_cast<const char*>(elem), sizeof(T));
        };
        col.deserialize_fn = [](void* elem, std::istream& in) {
            in.read(static_cast<char*>(elem), sizeof(T));
        };
    }
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

// -- Stable name registry for serialization --

inline std::map<std::string, ComponentTypeID>& name_to_id_registry() {
    static std::map<std::string, ComponentTypeID> reg;
    return reg;
}

inline std::map<ComponentTypeID, std::string>& id_to_name_registry() {
    static std::map<ComponentTypeID, std::string> reg;
    return reg;
}

template <typename T>
void register_component(const char* name, ComponentColumn::SerializeFunc ser = nullptr,
                        ComponentColumn::DeserializeFunc deser = nullptr) {
    auto& n2i = name_to_id_registry();
    auto& i2n = id_to_name_registry();
    ComponentTypeID id = component_id<T>();

    auto it = n2i.find(name);
    if (it != n2i.end()) {
        ECS_ASSERT(it->second == id, "component name already registered to different type");
        return;
    }
    auto it2 = i2n.find(id);
    if (it2 != i2n.end()) {
        ECS_ASSERT(it2->second == std::string(name),
                   "component type already registered with different name");
        return;
    }
    n2i[name] = id;
    i2n[id] = name;

    // Determine serialize/deserialize functions
    ComponentColumn::SerializeFunc actual_ser = ser;
    ComponentColumn::DeserializeFunc actual_deser = deser;
    if constexpr (std::is_trivially_copyable_v<T>) {
        if (!actual_ser) {
            actual_ser = [](const void* elem, std::ostream& out) {
                out.write(static_cast<const char*>(elem), sizeof(T));
            };
        }
        if (!actual_deser) {
            actual_deser = [](void* elem, std::istream& in) {
                in.read(static_cast<char*>(elem), sizeof(T));
            };
        }
    }

    // Update the column factory to capture serialize/deserialize functions
    auto& reg = column_factory_registry();
    reg[id] = [actual_ser, actual_deser]() -> ComponentColumn {
        auto col = make_column<T>();
        col.serialize_fn = actual_ser;
        col.deserialize_fn = actual_deser;
        return col;
    };
}

inline ComponentTypeID component_id_by_name(const std::string& name) {
    auto& n2i = name_to_id_registry();
    auto it = n2i.find(name);
    ECS_ASSERT(it != n2i.end(), "component_id_by_name: name not registered");
    return it->second;
}

inline const std::string& component_name(ComponentTypeID id) {
    auto& i2n = id_to_name_registry();
    auto it = i2n.find(id);
    ECS_ASSERT(it != i2n.end(), "component_name: id not registered");
    return it->second;
}

inline bool component_registered(ComponentTypeID id) {
    auto& i2n = id_to_name_registry();
    return i2n.find(id) != i2n.end();
}

} // namespace ecs
