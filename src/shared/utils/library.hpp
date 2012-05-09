#pragma once

#include <cstdint>
#include <cassert>
#include <vector>
#include <unordered_map>
#include <string>
#include <stdexcept>
#include <functional>

#include "utils/uncopyable.hpp"
#include "utils/tout.hpp"

namespace fr {

struct Config;
struct Camera;
class Image;
class LightList;
struct Shader;
struct Texture;
struct Material;
struct Mesh;
class NetNode;
struct FatRay;

class Library : private Uncopyable {
public:
    explicit Library();
    ~Library();

    // Configs...
    void StoreConfig(Config* config);

    inline Config* LookupConfig() const { return _config; }

    // Cameras...
    void StoreCamera(Camera* camera);

    inline Camera* LookupCamera() const { return _camera; }

    // Images...
    void StoreImage(Image* image);

    inline Image* LookupImage() const { return _image; }

    // Light lists...
    void StoreLightList(LightList* lights);

    inline LightList* LookupLightList() const { return _lights; }

    // Shaders...
    inline uint64_t NextShaderID() const { return _shaders.size(); }

    void StoreShader(uint64_t id, Shader* shader);

    inline Shader* LookupShader(uint64_t id) const {
        assert(id > 0);
        assert(id < _shaders.size());
        return _shaders[id];    
    };

    // Textures...
    inline uint64_t NextTextureID() const { return _textures.size(); }

    void StoreTexture(uint64_t id, Texture* texture);

    inline Texture* LookupTexture(uint64_t id) const {
        assert(id > 0);
        assert(id < _textures.size());
        return _textures[id];
    }

    // Materials...
    inline uint64_t NextMaterialID() const { return _materials.size(); }

    void StoreMaterial(uint64_t id, Material* material, const std::string& name);

    inline Material* LookupMaterial(uint64_t id) const {
        assert(id > 0);
        assert(id < _materials.size());
        return _materials[id];
    }

    inline uint64_t LookupMaterial(const std::string& name) const {
        uint64_t id = 0;
        try {
            id = _material_name_index.at(name);
        } catch (std::out_of_range& e) {
            id = 0;
        }
        return id;
    }

    // Meshes...
    inline uint64_t NextMeshID() const { return _meshes.size(); }

    void StoreMesh(uint64_t id, Mesh* mesh);

    inline Mesh* LookupMesh(uint64_t id) const {
        assert(id > 0);
        assert(id < _meshes.size());
        return _meshes[id];
    }

    void ForEachEmissiveMesh(std::function<void (uint64_t id, Mesh* mesh)> func);

    void NaiveIntersect(FatRay* ray, uint64_t me);

    // Net nodes...
    void StoreNetNode(uint64_t id, NetNode* node);

    inline NetNode* LookupNetNode(uint64_t id) const {
        assert(id > 0);
        assert(id < _nodes.size());
        return _nodes[id];
    }

    void ForEachNetNode(std::function<void (uint64_t id, NetNode* node)> func);

    template <typename RetType>
    std::vector<RetType> ForEachNetNode(std::function<RetType (uint64_t id, NetNode* node)> func) {
        std::vector<RetType> results(_nodes.size());
        for (uint64_t id = 1; id < _nodes.size(); id++) {
            NetNode* node = _nodes[id];
            if (node == nullptr) continue;
            RetType result = func(id, node);
            results.insert(results.begin() + id, result);
        }
        return results;
    }

    // Spatial index access for net nodes...
    void BuildSpatialIndex();

    inline uint64_t LookupNetNodeBySpaceCode(uint64_t spacecode) const {
#ifndef NDEBUG
        if (_chunk_size == 0) {
            TERRLN("Attempted to lookup net node by space code without first building the spatial index!");
            exit(EXIT_FAILURE);
        }
#endif
        return _spatial_index.at(spacecode / _chunk_size);
    }

private:
    Config *_config;
    Camera* _camera;
    Image* _image;
    LightList* _lights;
    std::vector<Shader*> _shaders;
    std::vector<Texture*> _textures;
    std::vector<Material*> _materials;
    std::vector<Mesh*> _meshes;
    std::vector<NetNode*> _nodes;
    std::unordered_map<std::string, uint64_t> _material_name_index;
    std::vector<uint64_t> _spatial_index;
    std::vector<uint64_t> _emissive_index;
    uint64_t _chunk_size;
};

} // namespace fr
