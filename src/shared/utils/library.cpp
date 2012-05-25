#include "utils/library.hpp"

#include <limits>

#include "glm/glm.hpp"

#include "types.hpp"
#include "utils.hpp"

using std::string;
using std::function;
using std::numeric_limits;
using glm::vec3;
using glm::vec4;
using glm::normalize;

namespace fr {

Library::Library() :
 _config(nullptr),
 _camera(nullptr),
 _image(nullptr),
 _lights(nullptr),
 _shaders(),
 _textures(),
 _materials(),
 _meshes(),
 _nodes(),
 _material_name_index(),
 _spatial_index(),
 _emissive_index(),
 _chunk_size(0) {
    // ID #0 is always reserved.
    _shaders.push_back(nullptr);
    _textures.push_back(nullptr);
    _materials.push_back(nullptr);
    _meshes.push_back(nullptr);
    _nodes.push_back(nullptr);
}

Library::~Library() {
    if (_config != nullptr) delete _config;
    if (_camera != nullptr) delete _camera;
    if (_image != nullptr) delete _image;
    if (_lights != nullptr) delete _lights;

    for (size_t i = 0; i < _shaders.size(); i++) {
        if (_shaders[i] != nullptr) delete _shaders[i];
    }
    for (size_t i = 0; i < _textures.size(); i++) {
        if (_textures[i] != nullptr) delete _textures[i];
    }
    for (size_t i = 0; i < _materials.size(); i++) {
        if (_materials[i] != nullptr) delete _materials[i];
    }
    for (size_t i = 0; i < _meshes.size(); i++) {
        if (_meshes[i] != nullptr) delete _meshes[i];
    }
    for (size_t i = 0; i < _nodes.size(); i++) {
        if (_nodes[i] != nullptr) delete _nodes[i];
    }
}

void Library::StoreConfig(Config* config) {
    if (_config != nullptr) delete _config;
    _config = config;
}

void Library::StoreCamera(Camera* camera) {
    if (_camera != nullptr) delete _camera;
    _camera = camera;
}

void Library::StoreImage(Image* image) {
    if (_image != nullptr) delete _image;
    _image = image;
}

void Library::StoreLightList(LightList* lights) {
    if (_lights != nullptr) delete _lights;
    _lights = lights;
}

void Library::StoreShader(uint32_t id, Shader* shader) {
    if (id < _shaders.size()) {
        if (_shaders[id] != nullptr) delete _shaders[id];
    } else {
        _shaders.resize(id + 1, nullptr);
    }
    _shaders[id] = shader;
}

void Library::StoreTexture(uint32_t id, Texture* texture) {
    if (id < _textures.size()) {
        if (_textures[id] != nullptr) delete _textures[id];
    } else {
        _textures.resize(id + 1, nullptr);
    }
    _textures[id] = texture;
}

void Library::StoreMaterial(uint32_t id, Material* material, const string& name) {
    if (id < _materials.size()) {
        if (_materials[id] != nullptr) delete _materials[id];
    } else {
        _materials.resize(id + 1, nullptr);
    }
    _materials[id] = material;
    _material_name_index[name] = id;
}

void Library::StoreMesh(uint32_t id, Mesh* mesh) {
    if (id < _meshes.size()) {
        if (_meshes[id] != nullptr) delete _meshes[id];
    } else {
        _meshes.resize(id + 1, nullptr);
    }
    _meshes[id] = mesh;

    if (mesh != nullptr) {
        Material* mat = _materials[mesh->material];
        assert(mat != nullptr);
        if (mat->emissive) {
            _emissive_index.push_back(id);
        }
    }
}

void Library::ForEachMesh(function<void (uint32_t, Mesh*)> func) {
    for (uint32_t id = 1; id < _meshes.size(); id++) {
        Mesh* mesh = _meshes[id];
        if (mesh == nullptr) continue;
        func(id, mesh);
    }
}

void Library::ForEachEmissiveMesh(function<void (uint32_t, Mesh*)> func) {
    for (uint32_t id : _emissive_index) {
        Mesh* mesh = _meshes[id];
        func(id, mesh);
    }
}

void Library::StoreNetNode(uint32_t id, NetNode* node) {
    if (id < _nodes.size()) {
        if (_nodes[id] != nullptr) delete _nodes[id];
    } else {
        _nodes.resize(id + 1, nullptr);
    }
    _nodes[id] = node;
}

void Library::ForEachNetNode(function<void (uint32_t, NetNode*)> func) {
    for (uint32_t id = 1; id < _nodes.size(); id++) {
        NetNode* node = _nodes[id];
        if (node == nullptr) continue;
        func(id, node);
    }
}

void Library::BuildSpatialIndex() {
    _spatial_index.clear();

    for (uint32_t id = 1; id < _nodes.size(); id++) {
        _spatial_index.push_back(id);
    }

    _chunk_size = ((SPACECODE_MAX + 1) / (_nodes.size() - 1)) + 1;
}

void Library::NaiveIntersect(FatRay* ray, uint32_t me) {
    HitRecord nearest(0, 0, numeric_limits<float>::infinity());

    for (uint32_t id = 1; id < _meshes.size(); id++) {
        Mesh* mesh = _meshes[id];
        if (mesh == nullptr) continue;

        // Get a skinny ray in the mesh's object space.
        SlimRay xformed_ray = ray->TransformTo(mesh);

        for (const auto& tri : mesh->tris) {
            float t = numeric_limits<float>::quiet_NaN();
            LocalGeometry local;

            if (tri.Intersect(xformed_ray, &t, &local) && t < nearest.t) {
                nearest.worker = me;
                nearest.mesh = id;
                nearest.t = t;
                nearest.geom = local;
            }
        }
    }

    if (nearest.worker > 0 && nearest.t < ray->hit.t) {
        ray->hit = nearest;

        // Correct the interpolated normal.
        vec4 n(ray->hit.geom.n, 0.0f);
        ray->hit.geom.n = normalize(
         vec3(_meshes[ray->hit.mesh]->xform_inv_tr * n));
    }
}

void Library::Intersect(FatRay* ray, uint32_t me) {
    HitRecord nearest(0, 0, numeric_limits<float>::infinity());

    for (uint32_t id = 1; id < _meshes.size(); id++) {
        Mesh* mesh = _meshes[id];
        if (mesh == nullptr) continue;

        // Get a skinny ray in the mesh's object space.
        SlimRay xformed_ray = ray->TransformTo(mesh);

        mesh->bvh->Traverse(xformed_ray, &nearest,
         [me, id, mesh](uint32_t index, const SlimRay& r, HitRecord* hit) {
            float t = numeric_limits<float>::quiet_NaN();
            LocalGeometry local;

            const Triangle& tri = mesh->tris[index];
            if (tri.Intersect(r, &t, &local) && t < hit->t) {
                hit->worker = me;
                hit->mesh = id;
                hit->t = t;
                hit->geom = local;
                return true;
            }

            return false;
        });
    }

    if (nearest.worker > 0 && nearest.t < ray->hit.t) {
        ray->hit = nearest;

        // Correct the interpolated normal.
        vec4 n(ray->hit.geom.n, 0.0f);
        ray->hit.geom.n = normalize(
         vec3(_meshes[ray->hit.mesh]->xform_inv_tr * n));
    }
}

}
