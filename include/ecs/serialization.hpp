#pragma once
#include "world.hpp"

#include <iostream>
#include <string>

namespace ecs {

/**
 * @brief Serializes the entire World state to an output stream.
 *
 * @details The binary format is:
 * - Header: "ECS\0" (4 bytes)
 * - Version: uint32_t (currently 1)
 * - Archetype Count: uint32_t
 * - Entity Slot Count: uint32_t
 * - Per Archetype:
 *   - Component Count: uint32_t
 *   - Entity Count: uint32_t
 *   - Per Component Column:
 *     - Name Length: uint32_t
 *     - Name: char[]
 *     - Element Size: uint32_t
 *   - Per Column Data:
 *     - Bytes... (via component's serialize_fn)
 *   - Entity List:
 *     - Index: uint32_t
 *     - Generation: uint32_t
 * - Entity Table:
 *   - Slot Count: uint32_t
 *   - Generations: uint32_t[]
 *   - Free List Count: uint32_t
 *   - Free List: uint32_t[]
 *
 * @param world The world to serialize.
 * @param out The output stream.
 * @warning All components in the world MUST be registered via `register_component` and have
 * valid serialization functions, or this function will assert/fail.
 */
inline void serialize(const World& world, std::ostream& out) {
    // Validate: all component types in all archetypes are registered and serializable
    for (auto& [ts, arch] : world.archetypes_) {
        if (arch->count() == 0)
            continue;
        for (auto& [cid, col] : arch->columns) {
            ECS_ASSERT(component_registered(cid),
                       "serialize: archetype contains unregistered component type");
            ECS_ASSERT(col.serialize_fn != nullptr,
                       "serialize: component type has no serialize function");
        }
    }

    // Header
    out.write("ECS\0", 4);
    uint32_t version = 1;
    out.write(reinterpret_cast<const char*>(&version), sizeof(version));

    // Count non-empty archetypes
    uint32_t archetype_count = 0;
    for (auto& [ts, arch] : world.archetypes_) {
        if (arch->count() > 0)
            ++archetype_count;
    }
    out.write(reinterpret_cast<const char*>(&archetype_count), sizeof(archetype_count));

    uint32_t entity_slot_count = static_cast<uint32_t>(world.generations_.size());
    out.write(reinterpret_cast<const char*>(&entity_slot_count), sizeof(entity_slot_count));

    // Per archetype
    for (auto& [ts, arch] : world.archetypes_) {
        if (arch->count() == 0)
            continue;

        uint32_t component_count = static_cast<uint32_t>(arch->columns.size());
        uint32_t entity_count = static_cast<uint32_t>(arch->count());
        out.write(reinterpret_cast<const char*>(&component_count), sizeof(component_count));
        out.write(reinterpret_cast<const char*>(&entity_count), sizeof(entity_count));

        // Component metadata (names + elem_size)
        for (auto& [cid, col] : arch->columns) {
            const std::string& name = component_name(cid);
            uint32_t name_len = static_cast<uint32_t>(name.size());
            out.write(reinterpret_cast<const char*>(&name_len), sizeof(name_len));
            out.write(name.data(), name_len);
            uint32_t elem_size = static_cast<uint32_t>(col.elem_size);
            out.write(reinterpret_cast<const char*>(&elem_size), sizeof(elem_size));
        }

        // Serialized column data
        for (auto& [cid, col] : arch->columns) {
            for (size_t i = 0; i < entity_count; ++i) {
                col.serialize_fn(col.data + i * col.elem_size, out);
            }
        }

        // Entity list
        for (size_t i = 0; i < entity_count; ++i) {
            out.write(reinterpret_cast<const char*>(&arch->entities[i].index), sizeof(uint32_t));
            out.write(reinterpret_cast<const char*>(&arch->entities[i].generation),
                      sizeof(uint32_t));
        }
    }

    // Entity table
    uint32_t slot_count = static_cast<uint32_t>(world.generations_.size());
    out.write(reinterpret_cast<const char*>(&slot_count), sizeof(slot_count));
    for (auto gen : world.generations_) {
        out.write(reinterpret_cast<const char*>(&gen), sizeof(gen));
    }
    uint32_t free_list_count = static_cast<uint32_t>(world.free_list_.size());
    out.write(reinterpret_cast<const char*>(&free_list_count), sizeof(free_list_count));
    for (auto idx : world.free_list_) {
        out.write(reinterpret_cast<const char*>(&idx), sizeof(idx));
    }
}

/**
 * @brief Restores the World state from an input stream.
 *
 * @details Reconstructs archetypes, entities, and components.
 * Matches components by name using `component_id_by_name`.
 *
 * @param world The target world (must be empty).
 * @param in The input stream.
 * @warning The target world must be empty. All components present in the stream
 * must be registered in the target application.
 */
inline void deserialize(World& world, std::istream& in) {
    ECS_ASSERT(world.count() == 0, "deserialize: world must be empty");

    // Read header
    char magic[4];
    in.read(magic, 4);
    ECS_ASSERT(magic[0] == 'E' && magic[1] == 'C' && magic[2] == 'S' && magic[3] == '\0',
               "deserialize: invalid magic");

    uint32_t version;
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    ECS_ASSERT(version == 1, "deserialize: unsupported version");

    uint32_t archetype_count;
    in.read(reinterpret_cast<char*>(&archetype_count), sizeof(archetype_count));

    uint32_t entity_slot_count;
    in.read(reinterpret_cast<char*>(&entity_slot_count), sizeof(entity_slot_count));

    // Per archetype
    for (uint32_t a = 0; a < archetype_count; ++a) {
        uint32_t component_count;
        in.read(reinterpret_cast<char*>(&component_count), sizeof(component_count));

        uint32_t entity_count;
        in.read(reinterpret_cast<char*>(&entity_count), sizeof(entity_count));

        // Read component metadata
        struct CompMeta {
            ComponentTypeID id;
            uint32_t elem_size;
        };
        std::vector<CompMeta> metas;
        metas.reserve(component_count);
        TypeSet ts;
        ts.reserve(component_count);

        for (uint32_t c = 0; c < component_count; ++c) {
            uint32_t name_len;
            in.read(reinterpret_cast<char*>(&name_len), sizeof(name_len));
            std::string name(name_len, '\0');
            in.read(&name[0], name_len);
            uint32_t elem_size;
            in.read(reinterpret_cast<char*>(&elem_size), sizeof(elem_size));

            ComponentTypeID cid = component_id_by_name(name);
            metas.push_back({cid, elem_size});
            ts.push_back(cid);
        }
        std::sort(ts.begin(), ts.end());

        // Create archetype and ensure capacity
        Archetype* arch = world.get_or_create_archetype(ts);
        arch->ensure_capacity(entity_count);

        // Deserialize column data — iterate in the same order as serialized
        // (arch->columns iteration order = std::map order by ComponentTypeID).
        // But we serialized in arch->columns order too, so this matches.
        // However, the source world and dest world may have different column
        // orders if component IDs differ across builds. So we use metas order.
        for (uint32_t c = 0; c < component_count; ++c) {
            auto& col = *arch->find_column(metas[c].id);
            ECS_ASSERT(col.deserialize_fn != nullptr,
                       "deserialize: component type has no deserialize function");
            ECS_ASSERT(col.elem_size == metas[c].elem_size, "deserialize: component size mismatch");
            for (uint32_t i = 0; i < entity_count; ++i) {
                void* dst = col.data + i * col.elem_size;
                col.deserialize_fn(dst, in);
            }
            col.count = entity_count;
        }

        // Read entity list
        arch->entities.resize(entity_count);
        for (uint32_t i = 0; i < entity_count; ++i) {
            in.read(reinterpret_cast<char*>(&arch->entities[i].index), sizeof(uint32_t));
            in.read(reinterpret_cast<char*>(&arch->entities[i].generation), sizeof(uint32_t));
        }
    }

    // Read entity table
    uint32_t slot_count;
    in.read(reinterpret_cast<char*>(&slot_count), sizeof(slot_count));
    world.generations_.resize(slot_count);
    for (uint32_t i = 0; i < slot_count; ++i) {
        in.read(reinterpret_cast<char*>(&world.generations_[i]), sizeof(uint32_t));
    }

    uint32_t free_list_count;
    in.read(reinterpret_cast<char*>(&free_list_count), sizeof(free_list_count));
    world.free_list_.resize(free_list_count);
    for (uint32_t i = 0; i < free_list_count; ++i) {
        in.read(reinterpret_cast<char*>(&world.free_list_[i]), sizeof(uint32_t));
    }

    // Rebuild records from archetype entity lists
    world.records_.resize(slot_count);
    for (auto& rec : world.records_) {
        rec.archetype = nullptr;
        rec.row = 0;
    }
    for (auto& [ts, arch] : world.archetypes_) {
        for (size_t i = 0; i < arch->count(); ++i) {
            Entity e = arch->entities[i];
            world.records_[e.index] = {arch.get(), i};
        }
    }
}

} // namespace ecs
