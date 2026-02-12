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

/**
 * @brief Unique identifier type for component types.
 */
using ComponentTypeID = uint32_t;

/**
 * @brief Generates a new unique component type ID.
 * @details This function is internal and thread-unsafe if called concurrently during initialization.
 * @return A new ComponentTypeID.
 */
inline ComponentTypeID next_component_id() {
    static ComponentTypeID counter = 0;
    return counter++;
}

/**
 * @brief Retrieves the unique ComponentTypeID for a specific type T.
 * @tparam T The component type.
 * @return The unique ID for type T.
 */
template <typename T>
ComponentTypeID component_id() {
    static ComponentTypeID id = next_component_id();
    return id;
}

/**
 * @brief Type-erased column storage for a single component type within an archetype.
 *
 * @details This struct manages the operations (move, destroy, swap, serialize) for a column of components
 * without knowing the concrete type at compile time. It acts as a vtable and metadata holder
 * for a contiguous array of components.
 *
 * @warning The actual memory buffer (`data`) is NOT owned by this struct. It is typically a pointer
 * into a larger memory block managed by an `Archetype`.
 */
struct ComponentColumn {
    /** @brief Pointer to the start of the component data array. */
    uint8_t* data = nullptr;
    /** @brief Size of a single component element in bytes. */
    size_t elem_size = 0;
    /** @brief Number of active elements in the column. */
    size_t count = 0;
    /** @brief Allocated capacity of the column (in elements). */
    size_t capacity = 0;
    /** @brief Alignment requirement of the component type. */
    size_t alignment = 1;

    using MoveFunc = void (*)(void* dst, void* src);
    using DestroyFunc = void (*)(void* ptr);
    using SwapFunc = void (*)(void* a, void* b);
    using SerializeFunc = void (*)(const void* elem, std::ostream& out);
    using DeserializeFunc = void (*)(void* elem, std::istream& in);

    /** @brief Function pointer to move-construct an element. */
    MoveFunc move_fn = nullptr;
    /** @brief Function pointer to destroy (call destructor) an element. */
    DestroyFunc destroy_fn = nullptr;
    /** @brief Function pointer to swap two elements. */
    SwapFunc swap_fn = nullptr;
    /** @brief Function pointer to serialize an element. */
    SerializeFunc serialize_fn = nullptr;
    /** @brief Function pointer to deserialize an element. */
    DeserializeFunc deserialize_fn = nullptr;

    ComponentColumn() = default;

    /**
     * @brief Move constructor.
     * @details Transfers ownership of metadata. Data pointer is copied, but ownership remains external.
     */
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

    /**
     * @brief Move assignment operator.
     * @details Destroys existing elements via `destroy_all` before taking over new metadata.
     */
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

    /**
     * @brief Destructor.
     * @details Calls destructors on all active elements but does not free `data`.
     */
    ~ComponentColumn() {
        destroy_all();
        // data is owned by Archetype's block_ — do NOT free here
    }

    ComponentColumn(const ComponentColumn&) = delete;
    ComponentColumn& operator=(const ComponentColumn&) = delete;

    /**
     * @brief Pushes a new element into the column via move construction.
     * @param src Pointer to the source object.
     * @warning Assumes capacity is sufficient. The caller must ensure space exists.
     */
    void push_raw(void* src) {
        ECS_ASSERT(count < capacity, "push_raw: column at capacity (archetype should have grown)");
        move_fn(data + count * elem_size, src);
        ++count;
    }

    /**
     * @brief Removes an element at a specific index using the "swap and pop" idiom.
     * @details The element at `row` is destroyed, and the last element is moved into its place.
     * @param row Index of the element to remove.
     */
    void swap_remove(size_t row) {
        if (row < count - 1) {
            destroy_fn(data + row * elem_size);
            move_fn(data + row * elem_size, data + (count - 1) * elem_size);
        } else {
            destroy_fn(data + row * elem_size);
        }
        --count;
    }

    /**
     * @brief Gets a type-erased pointer to the element at the specified row.
     * @param row Index of the element.
     * @return void* pointer to the element data.
     */
    void* get(size_t row) { return data + row * elem_size; }

    /**
     * @brief Destroys all elements in the column.
     * @details Resets count to 0. Does not free memory.
     */
    void destroy_all() {
        if (data && destroy_fn) {
            for (size_t i = 0; i < count; ++i)
                destroy_fn(data + i * elem_size);
        }
        count = 0;
    }
};

/**
 * @brief Creates a ComponentColumn configured for a specific type T.
 * @tparam T The component type.
 * @return A configured ComponentColumn with appropriate lifecycle functions.
 */
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

/**
 * @brief Global registry of column factories so we can create columns for types
 * during archetype migration without knowing the concrete type.
 */
using ColumnFactory = std::function<ComponentColumn()>;

/**
 * @brief Accesses the global registry mapping component IDs to their column factories.
 * @return Reference to the registry map.
 */
inline std::map<ComponentTypeID, ColumnFactory>& column_factory_registry() {
    static std::map<ComponentTypeID, ColumnFactory> reg;
    return reg;
}

/**
 * @brief Ensures a column factory exists for type T.
 * @tparam T The component type.
 */
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

/**
 * @brief Accesses the registry mapping string names to component IDs.
 * @return Reference to the name-to-ID map.
 */
inline std::map<std::string, ComponentTypeID>& name_to_id_registry() {
    static std::map<std::string, ComponentTypeID> reg;
    return reg;
}

/**
 * @brief Accesses the registry mapping component IDs to string names.
 * @return Reference to the ID-to-name map.
 */
inline std::map<ComponentTypeID, std::string>& id_to_name_registry() {
    static std::map<ComponentTypeID, std::string> reg;
    return reg;
}

/**
 * @brief Registers a component type with a stable name for serialization.
 * @tparam T The component type.
 * @param name The unique string name for the component.
 * @param ser Optional custom serialization function.
 * @param deser Optional custom deserialization function.
 */
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

/**
 * @brief lookups a component ID by its registered name.
 * @param name The registered name.
 * @return The ComponentTypeID.
 * @warning Asserts if the name is not registered.
 */
inline ComponentTypeID component_id_by_name(const std::string& name) {
    auto& n2i = name_to_id_registry();
    auto it = n2i.find(name);
    ECS_ASSERT(it != n2i.end(), "component_id_by_name: name not registered");
    return it->second;
}

/**
 * @brief Lookups a registered name by component ID.
 * @param id The component ID.
 * @return The registered name.
 * @warning Asserts if the ID is not registered.
 */
inline const std::string& component_name(ComponentTypeID id) {
    auto& i2n = id_to_name_registry();
    auto it = i2n.find(id);
    ECS_ASSERT(it != i2n.end(), "component_name: id not registered");
    return it->second;
}

/**
 * @brief Checks if a component ID has been registered.
 * @param id The component ID.
 * @return true if registered, false otherwise.
 */
inline bool component_registered(ComponentTypeID id) {
    auto& i2n = id_to_name_registry();
    return i2n.find(id) != i2n.end();
}

} // namespace ecs
