#pragma once

#include <cstdint>
#include <vector>
#include <functional>
#include <utility>

#include "glm/glm.hpp"
#include "msgpack.hpp"

#include "types/linear_node.hpp"
#include "types/traversal_state.hpp"
#include "utils/tostring.hpp"

namespace fr {

struct Mesh;
struct SlimRay;
struct LinkedNode;
struct PrimitiveInfo;
struct HitRecord;

/**
 * The construction implementation is based on the one presented in Physically
 * Based Rendering, Section 4.4, pages 208-227, with some modifications to
 * support stackless traversal. The stackless traversal algorithm is as
 * described in Hapala et al [2011], with modifications for suspension on one
 * worker and resuming on another without a restart.
 */

class BVH {
public:
    /**
     * Constructs a BVH for traversing the given mesh.
     */
    explicit BVH(const Mesh* mesh);

    /**
     * Constructs a BVH for traversing a set of things, where those things
     * are pairs of resource IDs and their bounding boxes.
     */
    explicit BVH(const std::vector<std::pair<uint32_t, BoundingBox>>& things);

    /// MSGPACK ONLY!
    explicit BVH();

    /**
     * Traverses the BVH by testing the given SlimRay against the bounding
     * volumes. If a leaf node is hit, the passed primitive intersector
     * function will be called. Returns the current traversal state when the
     * function exits.
     */
    TraversalState Traverse(const SlimRay& ray, HitRecord* nearest,
     std::function<bool (uint32_t index, const SlimRay& ray, HitRecord* hit, bool* request_suspend)> intersector);

    /**
     * Also traverses the BVH, but resumes traversal where we left off using
     * the given TraversalState packet. Returns the current traversal state
     * when the function exits.
     */
    TraversalState Traverse(TraversalState state, const SlimRay& ray, HitRecord* nearest,
     std::function<bool (uint32_t index, const SlimRay& ray, HitRecord* hit, bool* request_suspend)> intersector, bool resume = true);

    /**
     * Returns the extents of the area contained by the BVH.
     */
    inline BoundingBox Extents() const {
        return _nodes[0].bounds;
    }

    inline uint64_t GetSizeInBytes() const { return _nodes.size() * sizeof(LinearNode); }
    inline float GetSizeInMB() const { return (_nodes.size() * sizeof(LinearNode)) / (1024.0f * 1024.0f); }

    MSGPACK_DEFINE(_nodes);

    TOSTRINGABLEBYPTR(BVH);

private:
    struct BucketInfo {
        uint32_t count;
        BoundingBox bounds; 
    };

    static const uint32_t NUM_BUCKETS;

    std::vector<LinearNode> _nodes;

    /**
     * Constructs the BVH from the given initialized build data.
     */
    void Build(std::vector<PrimitiveInfo>& build_data);

    /**
     * Recursively partitions and builds the BVH for the given build data
     * between the start and end indexes. Returns the total nodes created
     * through the passed pointer.
     */
    LinkedNode* RecursiveBuild(std::vector<PrimitiveInfo>& build_data,
     size_t start, size_t end, size_t* total_nodes);

    /**
     * Computes the minimum cost split for the build_data given num_buckets
     * possible candidate splits and the centroid bounding box.
     */
    uint32_t ComputeSAH(std::vector<PrimitiveInfo>& build_data, size_t start,
     size_t end, float min, float max, float surface_area, BoundingBox::Axis axis);

    /**
     * Flattens the linked tree structure into a linear structure with offsets
     * for faster/portable traversal at runtime.
     */
    size_t FlattenTree(LinkedNode* current, size_t parent, size_t* offset);

    /**
     * Recursively deletes a LinkedNode tree structure rooted at node.
     */
    void DeleteLinked(LinkedNode* node);

    /// Special case constructor for building a tree with nothing in it.
    void ZeroThings();

    /// Special case constructor for building a tree with one thing in it.
    void OneThing(uint32_t id, const BoundingBox& bounds);

    /// Returns the index of the sibling of the current node.
    inline size_t Sibling(size_t current) {
        size_t parent = _nodes[current].parent;
        size_t right = _nodes[parent].right;
        return (right == current) ? parent + 1 : right;
    }

    /// The near child is defined to be the left-hand child.
    inline size_t NearChild(size_t current, glm::vec3 direction) {
        float axis_component = AxisComponent(direction, _nodes[current].axis);
        return axis_component < 0.0f ? _nodes[current].right : current + 1;
    }

    /// The far child is defined to be the right-hand child.
    inline size_t FarChild(size_t current, glm::vec3 direction) {
        float axis_component = AxisComponent(direction, _nodes[current].axis);
        return axis_component < 0.0f ? current + 1 : _nodes[current].right;
    }

    /// Performs a quick bounding box check against the given bounds and ray.
    inline bool BoundingHit(const BoundingBox& bounds, const SlimRay& ray,
     glm::vec3 inv_dir, float max) {
        if (!bounds.IsValid()) return false;
        float t = -1.0f;
        return bounds.Intersect(ray, inv_dir, &t) && t < max;
    }
};

std::string ToString(const BVH* bvh, const std::string& indent = "");

} // namespace fr
