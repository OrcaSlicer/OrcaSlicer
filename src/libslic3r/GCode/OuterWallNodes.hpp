#pragma once

#include <unordered_map>

namespace Slic3r {

class ExtrusionEntity;
class Print;

// Chains of outer walls that continue each other from layer to layer (the outer surface of a wall, a column, ...).
// Every chain gets an id that is unique over the whole print; the id is shared by all the instances of an object.
class OuterWallNodes
{
public:
    // Find the chains of the outer walls of all objects of the print. The outer walls of the first layer are not part of any chain.
    static OuterWallNodes build(const Print &print);

    // Id of the chain the outer wall entity belongs to, -1 when it is not in any chain.
    int  node_of(const ExtrusionEntity *entity) const
    {
        auto it = m_node_of_entity.find(entity);
        return it == m_node_of_entity.end() ? -1 : it->second;
    }
    bool empty() const { return m_node_of_entity.empty(); }

private:
    std::unordered_map<const ExtrusionEntity*, int> m_node_of_entity;
};

} // namespace Slic3r
