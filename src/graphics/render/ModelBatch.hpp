#ifndef GRAPHICS_RENDER_MODEL_BATCH_HPP_
#define GRAPHICS_RENDER_MODEL_BATCH_HPP_

#include "../../maths/UVRegion.hpp"

#include <memory>
#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <unordered_map>

class Mesh;
class Texture;
class Chunks;
class Assets;

namespace model {
    struct Mesh;
    struct Model;
}

using texture_names_map = std::unordered_map<std::string, std::string>;

class ModelBatch {
    std::unique_ptr<float[]> const buffer;
    size_t const capacity;
    size_t index;

    std::unique_ptr<Mesh> mesh;
    std::unique_ptr<Texture> blank;

    glm::mat4 combined;
    std::vector<glm::mat4> matrices;
    glm::mat3 rotation;

    Assets* assets;
    Chunks* chunks;
    Texture* texture = nullptr;
    UVRegion region {0.0f, 0.0f, 1.0f, 1.0f};

    static inline glm::vec3 SUN_VECTOR {0.411934f, 0.863868f, -0.279161f};

    inline void vertex(
        glm::vec3 pos, glm::vec2 uv, glm::vec4 light
    ) {
        float* buffer = this->buffer.get();
        buffer[index++] = pos.x;
        buffer[index++] = pos.y;
        buffer[index++] = pos.z;
        buffer[index++] = uv.x * region.getWidth() + region.u1;
        buffer[index++] = uv.y * region.getHeight() + region.v1;

        union {
            float floating;
            uint32_t integer;
        } compressed;

        compressed.integer  = (static_cast<uint32_t>(light.r * 255) & 0xff) << 24;
        compressed.integer |= (static_cast<uint32_t>(light.g * 255) & 0xff) << 16;
        compressed.integer |= (static_cast<uint32_t>(light.b * 255) & 0xff) << 8;
        compressed.integer |= (static_cast<uint32_t>(light.a * 255) & 0xff);

        buffer[index++] = compressed.floating;
    }

    inline void plane(glm::vec3 pos, glm::vec3 right, glm::vec3 up, glm::vec3 norm, glm::vec4 lights) {
        norm = rotation * norm;
        float d = glm::dot(norm, SUN_VECTOR);
        d = 0.8f + d * 0.2f;
        
        auto color = lights * d;

        vertex(pos-right-up, {0,0}, color);
        vertex(pos+right-up, {1,0}, color);
        vertex(pos+right+up, {1,1}, color);

        vertex(pos-right-up, {0,0}, color);
        vertex(pos+right+up, {1,1}, color);
        vertex(pos-right+up, {0,1}, color);
    }

    void draw(const model::Mesh& mesh, const glm::mat4& matrix, 
              const glm::mat3& rotation, const texture_names_map* varTextures);
    void box(glm::vec3 pos, glm::vec3 size, glm::vec4 lights);
    void setTexture(const std::string& name,
                    const texture_names_map* varTextures);
    void setTexture(Texture* texture);
    void flush();

    struct DrawEntry {
        glm::mat4 matrix;
        glm::mat3 rotation;
        const model::Mesh* mesh;
        const texture_names_map* varTextures;
    };
    std::vector<DrawEntry> entries;
public:
    ModelBatch(size_t capacity, Assets* assets, Chunks* chunks);
    ~ModelBatch();

    void translate(glm::vec3 vec);
    void rotate(glm::vec3 axis, float angle);
    void scale(glm::vec3 vec);

    void pushMatrix(glm::mat4 matrix);
    void popMatrix();
    void draw(const model::Model* model,
              const texture_names_map* varTextures);

    void render();
};

#endif // GRAPHICS_RENDER_MODEL_BATCH_HPP_