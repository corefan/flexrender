#pragma once

#include <cstring>

#include "utils/uncopyable.hpp"

namespace fr {

struct FatRay;
struct Camera;
struct RenderStats;

class RayQueue : private Uncopyable {
public:
    explicit RayQueue(Camera* camera, RenderStats* stats);

    ~RayQueue();

    /// Pushes the given ray into the queue and assumes ownership of its memory.
    void Push(FatRay* ray);

    /// Pops a ray out of the queue and relinquishes control of its memory.
    FatRay* Pop();

    /// Returns the size of the internal intersection ray queue.
    inline size_t IntersectSize() const { return _intersect_size; }

    /// Returns the size of the internal illumination ray queue.
    inline size_t IlluminateSize() const { return _illuminate_size; }

    /// Returns the size of the internal light ray queue.
    inline size_t LightSize() const { return _light_size; }

    /// Pauses primary ray generation.
    void Pause() { _paused = true; }

    /// Resumes primary ray generation.
    void Resume() { _paused = false; }

private:
    Camera* _camera;
    RenderStats* _stats;
    FatRay* _intersect_front;
    FatRay* _intersect_back;
    size_t _intersect_size;
    FatRay* _illuminate_front;
    FatRay* _illuminate_back;
    size_t _illuminate_size;
    FatRay* _light_front;
    FatRay* _light_back;
    size_t _light_size;
    bool _paused;
};

} // namespace fr
