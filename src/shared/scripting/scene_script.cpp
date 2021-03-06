#include "scripting/scene_script.hpp"

#include <cstdint>
#include <limits>

#include "types.hpp"
#include "utils.hpp"

using std::string;
using std::numeric_limits;
using glm::vec2;
using glm::vec3;
using glm::vec4;
using glm::normalize;

namespace fr {

SceneScript::SceneScript(SyncCallback syncer) :
 Script(),
 _lib(nullptr),
 _active_mesh(nullptr),
 _centroid_num(0.0f, 0.0f, 0.0f),
 _centroid_denom(0.0f),
 _syncer(syncer),
 _total_verts(0),
 _total_faces(0),
 _total_bytes(0) {
    // Scene scripts should have access to the entire standard library.
    FR_SCRIPT_INIT(SceneScript, ScriptLibs::STANDARD_LIBS);

    // Register function handlers with the interpreter.
    FR_SCRIPT_REGISTER("camera", SceneScript, Camera);
    FR_SCRIPT_REGISTER("texture", SceneScript, Texture);
    FR_SCRIPT_REGISTER("shader", SceneScript, Shader);
    FR_SCRIPT_REGISTER("material", SceneScript, Material);
    FR_SCRIPT_REGISTER("mesh", SceneScript, Mesh);
    FR_SCRIPT_REGISTER("vertex", SceneScript, Vertex);
    FR_SCRIPT_REGISTER("triangle", SceneScript, Triangle);
}

bool SceneScript::Parse(const string& filename, Library *lib) {
    _lib = lib;

    // Create a new light list and save it in the library.
    LightList* lights = new LightList;
    _lib->StoreLightList(lights);

    // Evaluate the file.
    if (luaL_dofile(_state, filename.c_str())) {
        TERRLN(lua_tostring(_state, -1));
        return false;
    }

    return true;
}

FR_SCRIPT_FUNCTION(SceneScript, Camera) {
    BeginTableCall();

    // Used for creating a sensible default aspect ratio based on the output
    // image dimensions.
    Camera* cam = new Camera(_lib->LookupConfig());

    // "camera.eye" is a required float3.
    if (!PushField("eye", LUA_TTABLE)) {
        ScriptError("camera.eye is required");
    }
    cam->eye = FetchFloat3();
    PopField();

    // "camera.look" is a required float3.
    if (!PushField("look", LUA_TTABLE)) {
        ScriptError("camera.look is required");
    }
    cam->look = FetchFloat3();
    PopField();

    // "camera.up" is an optional float3.
    if (PushField("up", LUA_TTABLE)) {
        cam->up = normalize(FetchFloat3());
    }
    PopField();

    // "camera.rotation" is an optional float.
    if (PushField("rotation", LUA_TNUMBER)) {
        cam->rotation = FetchFloat();
    }
    PopField();

    // "camera.ratio" is an optional float.
    if (PushField("ratio", LUA_TNUMBER)) {
        cam->ratio = FetchFloat();
    }
    PopField();

    _lib->StoreCamera(cam);

    EndTableCall();
    return 0;
}

FR_SCRIPT_FUNCTION(SceneScript, Texture) {
    BeginTableCall();

    uint32_t id = _lib->NextTextureID();
    Texture *tex = new Texture(id);

    // "texture.kind" is a required string.
    if (!PushField("kind", LUA_TSTRING)) {
        ScriptError("texture.kind is required");
    }
    string kind = FetchString();
    PopField();

    if (kind == "procedural") {
        tex->kind = Texture::Kind::PROCEDURAL;

        // "texture.code" is a required string for procedural textures.
        if (!PushField("code", LUA_TSTRING)) {
            ScriptError("texture.code is required for procedural textures");
        }
        tex->code = FetchString();
        PopField();
    } else if (kind == "image") {
        tex->kind = Texture::Kind::IMAGE;

        // "texture.size" is a required vec2 of int16 for image textures.
        if (!PushField("size", LUA_TTABLE)) {
            ScriptError("texture.size is required for image textures");
        }
        vec2 size = FetchFloat2();
        tex->width = static_cast<int16_t>(size.x);
        tex->height = static_cast<int16_t>(size.y);
        PopField();

        // Raw image data is in the numerically indexed part of the table.
        ForEachIndex([this, tex](size_t index) {
            PushIndex(index, LUA_TNUMBER);
            tex->image.push_back(FetchFloat());
            PopIndex();
        });
    } else {
        ScriptError("texture.kind must be 'procedural' or 'image'");
    }

    _lib->StoreTexture(id, tex);

    EndTableCall();
    return ReturnResourceID(id);
}

FR_SCRIPT_FUNCTION(SceneScript, Shader) {
    BeginTableCall();

    uint32_t id = _lib->NextShaderID();
    Shader *shader = new Shader(id);

    // "shader.code" is a required string.
    if (!PushField("code", LUA_TSTRING)) {
        ScriptError("shader.code is required");
    }
    shader->code = FetchString();
    PopField();

    _lib->StoreShader(id, shader);

    EndTableCall();
    return ReturnResourceID(id);
}

FR_SCRIPT_FUNCTION(SceneScript, Material) {
    BeginTableCall();

    uint32_t id = _lib->NextMaterialID();
    Material *mat = new Material(id);

    // "material.name" is a required string.
    if (!PushField("name", LUA_TSTRING)) {
        ScriptError("material.name is required");
    }
    string name = FetchString();
    PopField();

    // "mat.emissive" is an optional boolean.
    if (PushField("emissive", LUA_TBOOLEAN)) {
        mat->emissive = FetchBool();
    }
    PopField();

    // "material.shader" is a required string.
    if (!PushField("shader", LUA_TSTRING)) {
        ScriptError("material.shader is required");
    }
    mat->shader = DecodeResourceID(FetchString());
    PopField();

    // "material.textures" is an optional table.
    if (PushField("textures", LUA_TTABLE)) {
        ForEachKeyVal([this, mat](const string &key) {
            TypeCheck(LUA_TSTRING, "texture resource ID");
            mat->textures[key] = DecodeResourceID(FetchString());
        });
    }
    PopField();

    _lib->StoreMaterial(id, mat, name);

    EndTableCall();
    return ReturnResourceID(id);
}

FR_SCRIPT_FUNCTION(SceneScript, Mesh) {
    BeginTableCall();

    Mesh *mesh = new Mesh;

    _active_mesh = mesh;
    _centroid_num = vec3(0.0f, 0.0f, 0.0f);
    _centroid_denom = 0.0f;

    // "mesh.material" is a required string.
    if (!PushField("material", LUA_TSTRING)) {
        ScriptError("mesh.material is required");
    }
    mesh->material = _lib->LookupMaterial(FetchString());
    PopField();

    // "mesh.transforms" is an 4x4 array of floats.
    if (PushField("transform", LUA_TTABLE)) {
        vec4 rows[4];
        uint32_t count = 0;
        ForEachIndex([this, &rows, &count, mesh](size_t index) {
            if (count > 3) {
                ScriptError("expected 4 columns in a matrix");
            }

            PushIndex(index, LUA_TTABLE);
            rows[index - 1] = FetchFloat4();
            PopIndex();
            count++;
        });
        mesh->xform_cols[0] = rows[0];
        mesh->xform_cols[1] = rows[1];
        mesh->xform_cols[2] = rows[2];
        mesh->xform_cols[3] = rows[3];
    }
    PopField();

    // "mesh.data" is a required function
    if (!PushField("data", LUA_TFUNCTION)) {
        ScriptError("mesh.data is required");
    }
    CallFunc(0, 0);
    // no need to pop, 0 return values
    
    // Compute transformation matrices.
    mesh->ComputeMatrices();

    // Transform the centroid into world space.
    vec4 centroid(_centroid_num.x / _centroid_denom,
                  _centroid_num.y / _centroid_denom,
                  _centroid_num.z / _centroid_denom,
                  1.0f);
    mesh->centroid = vec3(mesh->xform * centroid);

    uint64_t num_verts = mesh->vertices.size();
    uint64_t num_faces = mesh->faces.size();
    uint64_t num_bytes = num_verts * sizeof(Vertex) + num_faces * sizeof(Triangle);
    _total_verts += num_verts;
    _total_faces += num_faces;
    _total_bytes += num_bytes;
    float total_kb = _total_bytes / 1024.0f;

    TOUTLN("Loaded " << num_verts << "v, " << num_faces << "f, " << num_bytes << " bytes (" << _total_verts << "v, " << _total_faces << "f, " << total_kb << " KB total)");

    // Sync the mesh.
    uint32_t id = _syncer(mesh);

    EndTableCall();
    return ReturnResourceID(id);
}

FR_SCRIPT_FUNCTION(SceneScript, Vertex) {
    BeginTableCall();

    vec3 v(numeric_limits<float>::quiet_NaN(),
           numeric_limits<float>::quiet_NaN(),
           numeric_limits<float>::quiet_NaN());
    vec3 n(numeric_limits<float>::quiet_NaN(),
           numeric_limits<float>::quiet_NaN(),
           numeric_limits<float>::quiet_NaN());
    vec2 t(numeric_limits<float>::quiet_NaN(),
           numeric_limits<float>::quiet_NaN());

    // vertex.v is a required float3.
    if (!PushField("v", LUA_TTABLE)) {
        ScriptError("vertex.v is required");
    }
    v = FetchFloat3();
    PopField();

    // vertex.n is a required float3.
    if (!PushField("n", LUA_TTABLE)) {
        ScriptError("vertex.n is required");
    }
    n = normalize(FetchFloat3());
    PopField();

    // vertex.t is an optional float2.
    if (PushField("t", LUA_TTABLE)) {
        t = FetchFloat2();
    }
    PopField();

    // Track the centroid of the mesh.
    _centroid_num += v;
    _centroid_denom++;

    _active_mesh->vertices.emplace_back(v, n, t);

    EndTableCall();
    return 0;
}

FR_SCRIPT_FUNCTION(SceneScript, Triangle) {
    BeginTableCall();

    uint32_t v1 = numeric_limits<uint32_t>::max();
    uint32_t v2 = numeric_limits<uint32_t>::max();
    uint32_t v3 = numeric_limits<uint32_t>::max();

    // "triangle[1]" is a required uint32.
    PushIndex(1, LUA_TNUMBER);
    v1 = static_cast<uint32_t>(FetchFloat());
    PopIndex();

    // "triangle[2]" is a required uint32.
    PushIndex(2, LUA_TNUMBER);
    v2 = static_cast<uint32_t>(FetchFloat());
    PopIndex();

    // "triangle[3]" is a required uint32.
    PushIndex(3, LUA_TNUMBER);
    v3 = static_cast<uint32_t>(FetchFloat());
    PopIndex();

    _active_mesh->faces.emplace_back(v1, v2, v3);
    EndTableCall();
    return 0;
}

} // namespace fr
