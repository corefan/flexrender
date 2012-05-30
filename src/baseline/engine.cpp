#include "engine.hpp"

#include <ctime>
#include <cstdlib>
#include <iostream>
#include <cassert>
#include <vector>
#include <utility>

#include "uv.h"

#include "scripting.hpp"
#include "types.hpp"
#include "utils.hpp"

using std::string;
using std::flush;
using std::cout;
using std::endl;
using std::vector;
using std::pair;
using std::make_pair;
using glm::vec3;

namespace fr {

/// The library for the current render.
static Library* lib = nullptr;

/// The maximum number of uninteresting stats intervals before we declare the
/// rendering complete.
static uint32_t max_intervals = 0;

/// The maximum number of jobs "in flight" at any given time.
static uint32_t max_jobs = 0;

/// The number of jobs currently available for running on libuv's thread pool.
static uint32_t active_jobs = 0;

/// The timer for reporting stats during rendering.
static uv_timer_t stats_timer;

/// The scene file we're rendering.
static string scene;

/// Render stats for this worker.
static RenderStats stats;

/// Timers for measuring total time.
static time_t load_start;
static time_t load_stop;
static time_t build_start;
static time_t build_stop;
static time_t render_start;
static time_t render_stop;

uint32_t SyncMesh(Mesh* mesh);
void ScheduleJob();
void StopRender();
void ProcessRay(FatRay* ray, WorkResults* results);
void ProcessIntersect(FatRay* ray, WorkResults* results);
void ProcessLight(FatRay* ray, WorkResults* results);
void IlluminateIntersection(FatRay* ray, WorkResults* work);

void OnStatsTimeout(uv_timer_t* timer, int status);
void OnWork(uv_work_t* req);
void AfterWork(uv_work_t* req);

void EngineInit(const string& config_file, const string& scene_file,
 uint32_t intervals, uint32_t jobs) {
    lib = new Library;

    scene = scene_file;
    max_intervals = intervals;
    max_jobs = jobs;

    // Randomize the world.
    srand(time(0));

    // Parse the config file.
    ConfigScript config_script;
    TOUTLN("Loading config from " << config_file << ".");
    if (!config_script.Parse(config_file, lib)) {
        TERRLN("Can't continue with bad config.");
        exit(EXIT_FAILURE);
    }
    TOUTLN("Config loaded.");

    Config* config = lib->LookupConfig();
    assert(config != nullptr);

    // Create the image with all the requested buffers.
    Image* image = new Image(config->width, config->height);
    for (const auto& buffer : config->buffers) {
        image->AddBuffer(buffer);
    }
    lib->StoreImage(image);

    load_start = time(nullptr);

    // Parse the scene.
    SceneScript scene_script(SyncMesh);
    TOUTLN("Loading scene from " << scene << ".");
    if (!scene_script.Parse(scene, lib)) {
        TERRLN("Can't continue with bad scene.");
        exit(EXIT_FAILURE);
    }

    load_stop = time(nullptr);

    TOUT("Building BVH" << flush);
    build_start = time(nullptr);

    // Build triangle BVHs for each mesh.
    vector<pair<uint32_t, BoundingBox>> mesh_bounds;
    lib->ForEachMesh([&mesh_bounds](uint32_t id, Mesh* mesh) {
        mesh->bvh = new BVH(mesh);
        mesh_bounds.emplace_back(make_pair(id, mesh->bvh->Extents()));
        cout << "." << flush;
    });

    // Build the mesh BVH from the mesh extents.
    BVH* mbvh = new BVH(mesh_bounds);
    lib->StoreMBVH(mbvh);
    cout << "." << endl;

    build_stop = time(nullptr);
    TOUTLN("Local BVH ready.");

    // Set the camera range.
    Camera* camera = lib->LookupCamera();
    assert(camera != nullptr);
    camera->SetRange(0, config->width);

    // Start the stats timer.
    int result = 0;
    result = uv_timer_init(uv_default_loop(), &stats_timer);
    CheckUVResult(result, "timer_init");
    result = uv_timer_start(&stats_timer, OnStatsTimeout, 1000, 1000);
    CheckUVResult(result, "timer_start");

    // Queue up some jobs.
    for (uint32_t i = 0; i < max_jobs; i++) {
        ScheduleJob();
    }

    render_start = time(nullptr);
}

void EngineRun() {
    uv_run(uv_default_loop());
}

uint32_t SyncMesh(Mesh* mesh) {
    uint32_t id = 0;
    if (mesh != nullptr) {
        // Store the mesh in the library and get back its ID.
        id = lib->NextMeshID();
        mesh->id = id;
        lib->StoreMesh(id, mesh);

        uint32_t mat_id = mesh->material;
        Material* mat = lib->LookupMaterial(mat_id);
        assert(mat != nullptr);

        uint32_t shader_id = mat->shader;
        Shader* shader = lib->LookupShader(shader_id);
        assert(shader != nullptr);

        // Prep the shader if we haven't already.
        if (shader->script == nullptr) {
            shader->script = new ShaderScript(shader->code, lib);
        }

        // Prep any procedural textures for execution.
        for (const auto& kv : mat->textures) {
            uint32_t tex_id = kv.second;
            Texture* tex = lib->LookupTexture(tex_id);
            assert(mat != nullptr);
            if (tex->kind == Texture::Kind::PROCEDURAL && tex->script == nullptr) {
                tex->script = new TextureScript(tex->code);
            }
        }
    }

    return id;
}

void OnStatsTimeout(uv_timer_t* timer, int status) {
    assert(timer == &stats_timer);
    assert(status == 0);

    Camera* cam = lib->LookupCamera();
    assert(cam != nullptr);
    stats.primary_progress = cam->Progress();

    stats.Reset();

    // TODO
    TOUTLN("Stats!");
}

void ScheduleJob() {
    int result = 0;

    // Don't schedule anything if we're maxed out.
    if (active_jobs >= max_jobs) {
        return;
    }

    // Generate a new primary ray.
    Camera* cam = lib->LookupCamera();
    assert(cam != nullptr);
    FatRay* ray = new FatRay;
    if (cam->GeneratePrimary(ray)) {
        // Queue it for work.
        uv_work_t* req = reinterpret_cast<uv_work_t*>(malloc(sizeof(uv_work_t)));
        req->data = ray;
        result = uv_queue_work(uv_default_loop(), req, OnWork, AfterWork);
        CheckUVResult(result, "queue_work");
        active_jobs++;
    }
}

void OnWork(uv_work_t* req) {
    // !!! WARNING !!!
    // Everything this function does and calls must be thread-safe. This
    // function will NOT run in the main thread, it runs on the thread pool.

    // Pull the ray out of the data baton.
    FatRay *ray = reinterpret_cast<FatRay*>(req->data);

    // Allocate results of this work.
    WorkResults* results = new WorkResults;

    //// Dispatch the ray.
    ProcessRay(ray, results);

    //// Pass the work results back through the data baton.
    req->data = results;
}

void AfterWork(uv_work_t* req) {
    assert(req != nullptr);
    assert(req->data != nullptr);

    // Pull the work result out of the data baton.
    WorkResults* results = reinterpret_cast<WorkResults*>(req->data);

    //// Do buffer operations.
    Image* image = lib->LookupImage();
    assert(image != nullptr);
    for (auto& op : results->ops) {
        switch (op.kind) {
            case BufferOp::Kind::WRITE:
                image->Write(op.buffer, op.x, op.y, op.value);
                break;

            case BufferOp::Kind::ACCUMULATE:
                image->Accumulate(op.buffer, op.x, op.y, op.value);
                break;
        }
    }

    //// Forward rays and kill them off.
    //for (auto& forward : results->forwards) {
    //    if (forward.node == nullptr) {
    //        // We didn't know where to send it, so queue it for processing.
    //        rayq->Push(forward.ray);
    //    } else {
    //        // Send it and kill the local copy.
    //        forward.ray->workers_touched++;
    //        forward.node->SendRay(forward.ray);
    //        delete forward.ray;
    //    }
    //}

    //// Merge render stats.
    stats.intersects_produced += results->intersects_produced;
    stats.illuminates_produced += results->illuminates_produced;
    stats.lights_produced += results->lights_produced;
    stats.intersects_killed += results->intersects_killed;
    stats.illuminates_killed += results->illuminates_killed;
    stats.lights_killed += results->lights_killed;

    delete results;
    free(req);

    // This job is done. Schedule more work.
    active_jobs--;
    if (active_jobs == 0) {
        StopRender();
    } else {
        ScheduleJob();
    }
}

void StopRender() {
    int result = 0;

    Config* config = lib->LookupConfig();
    assert(config != nullptr);

    Image* final = lib->LookupImage();
    assert(final != nullptr);

    render_stop = time(nullptr);

    // Stop the stats timer.
    result = uv_timer_stop(&stats_timer);
    CheckUVResult(result, "timer_stop");
    uv_close(reinterpret_cast<uv_handle_t*>(&stats_timer), nullptr);

    // Write out the final image.
    final->ToEXRFile(config->name + ".exr");
    TOUTLN("Wrote " << config->name << ".exr.");

    // Dump out timers.
    TOUTLN("Time spent loading: " << (load_stop - load_start) << " seconds.");
    TOUTLN("Time spent building: " << (build_stop - build_start) << " seconds.");
    TOUTLN("Time spent rendering: " << (render_stop - render_start) << " seconds.");
}

void ProcessRay(FatRay* ray, WorkResults* results) {
    // !!! WARNING !!!
    // Everything this function does and calls must be thread-safe. This
    // function will NOT run in the main thread, it runs on the thread pool.

    switch (ray->kind) {
        case FatRay::Kind::INTERSECT:
            ProcessIntersect(ray, results);
            break;

        case FatRay::Kind::LIGHT:
            ProcessLight(ray, results);
            break;

        default:
            TERRLN("Unknown ray kind " << ray->kind << ".");
            break;
    }
}

void ProcessIntersect(FatRay* ray, WorkResults* results) {
    // !!! WARNING !!!
    // Everything this function does and calls must be thread-safe. This
    // function will NOT run in the main thread, it runs on the thread pool.

    // Test geometry for intersection.
    ray->traversal.hit = lib->Intersect(ray, 1);

    // Did it hit anything?
    if (ray->hit.worker > 0) {
        // Yes it did. Illuminate the intersection and kill the ray.
        IlluminateIntersection(ray, results);
    }

    // Kill the ray.
    delete ray;
    results->intersects_killed++;
    return;
}

void ProcessLight(FatRay* ray, WorkResults* results) {
    // !!! WARNING !!!
    // Everything this function does and calls must be thread-safe. This
    // function will NOT run in the main thread, it runs on the thread pool.

    // TODO
}

void IlluminateIntersection(FatRay* ray, WorkResults* results) {
    // !!! WARNING !!!
    // Everything this function does and calls must be thread-safe. This
    // function will NOT run in the main thread, it runs on the thread pool.

    // Where did we hit?
    vec3 hit = ray->EvaluateAt(ray->hit.t);

    // Find the shader and run the indirect() function.
    Mesh* mesh = lib->LookupMesh(ray->hit.mesh);
    assert(mesh != nullptr);

    Material* mat = lib->LookupMaterial(mesh->material);
    assert(mat != nullptr);

    Shader* shader = lib->LookupShader(mat->shader);
    assert(shader != nullptr);
    assert(shader->script != nullptr);

    shader->script->Indirect(ray, hit, results);

    // TODO: process traced rays in results immediately (recursive)
    
    // TODO: light this intersection by generating light rays and processing
    // them immediately (recursive)
}

} // namespace fr
