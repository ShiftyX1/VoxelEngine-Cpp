#include "Entities.hpp"

#include "../debug/Logger.hpp"
#include "../data/dynamic_util.hpp"
#include "../assets/Assets.hpp"
#include "../world/Level.hpp"
#include "../content/Content.hpp"
#include "../physics/Hitbox.hpp"
#include "../physics/PhysicsSolver.hpp"
#include "../graphics/render/ModelBatch.hpp"
#include "../graphics/core/LineBatch.hpp"
#include "../graphics/core/Model.hpp"
#include "../maths/FrustumCulling.hpp"
#include "../objects/EntityDef.hpp"
#include "../objects/rigging.hpp"
#include "../logic/scripting/scripting.hpp"
#include "../engine.hpp"

#include <glm/ext/matrix_transform.hpp>

static debug::Logger logger("entities");

static inline std::string COMP_TRANSFORM = "transform";
static inline std::string COMP_RIGIDBODY = "rigidbody";
static inline std::string COMP_MODELTREE = "modeltree";

void Transform::refresh() {
    combined = glm::mat4(1.0f);
    combined = glm::translate(combined, pos);
    combined = glm::scale(combined, size);
    combined = combined * glm::mat4(rot);
    dirty = false;
}

void Entity::destroy() {
    if (isValid()){
        entities.despawn(id);
    }
}

rigging::Rig& Entity::getModeltree() const {
    return registry.get<rigging::Rig>(entity);
}

void Entity::setRig(const rigging::RigConfig* rigConfig) {
    auto& rig = registry.get<rigging::Rig>(entity);
    rig.config = rigConfig;
    rig.pose.matrices.resize(rigConfig->getNodes().size(), glm::mat4(1.0f));
    rig.calculated.matrices.resize(rigConfig->getNodes().size(), glm::mat4(1.0f));
}

Entities::Entities(Level* level) : level(level) {
}

template<void(*callback)(const Entity&, size_t, entityid_t)>
static sensorcallback create_sensor_callback(Entities* entities) {
    return [=](auto entityid, auto index, auto otherid) {
        if (auto entity = entities->get(entityid)) {
            if (entity->isValid()) {
                callback(*entity, index, otherid);
            }
        }
    };
}

entityid_t Entities::spawn(
    EntityDef& def,
    glm::vec3 position,
    dynamic::Value args,
    dynamic::Map_sptr saved,
    entityid_t uid)
{
    auto rig = level->content->getRig(def.rigName);
    if (rig == nullptr) {
        throw std::runtime_error("rig "+def.rigName+" not found");
    }
    auto entity = registry.create();
    entityid_t id;
    if (uid == 0) {
        id = nextID++;
    } else {
        id = uid;
    }
    registry.emplace<EntityId>(entity, static_cast<entityid_t>(id), def);
    const auto& tsf = registry.emplace<Transform>(
        entity, position, glm::vec3(1.0f), glm::mat3(1.0f), glm::mat4(1.0f), true);
    auto& body = registry.emplace<Rigidbody>(
        entity, true, Hitbox {def.bodyType, position, def.hitbox}, std::vector<Sensor>{});

    body.sensors.resize(def.radialSensors.size() + def.boxSensors.size());
    for (auto& [i, box] : def.boxSensors) {
        SensorParams params {};
        params.aabb = box;
        body.sensors[i] = Sensor {
            true, SensorType::AABB, i, id, params, params, {}, {},
            create_sensor_callback<scripting::on_sensor_enter>(this),
            create_sensor_callback<scripting::on_sensor_exit>(this)};
    }
    for (auto& [i, radius] : def.radialSensors) {
        SensorParams params {};
        params.radial = glm::vec4(radius);
        body.sensors[i] = Sensor {
            true, SensorType::RADIUS, i, id, params, params, {}, {},
            create_sensor_callback<scripting::on_sensor_enter>(this),
            create_sensor_callback<scripting::on_sensor_exit>(this)};
    }
    auto& scripting = registry.emplace<ScriptComponents>(entity);
    entities[id] = entity;
    uids[entity] = id;
    registry.emplace<rigging::Rig>(entity, rig->instance());
    for (auto& componentName : def.components) {
        auto component = std::make_unique<UserComponent>(
            componentName, entity_funcs_set {}, nullptr);
        scripting.components.emplace_back(std::move(component));
    }
    dynamic::Map_sptr componentsMap = nullptr;
    if (saved) {
        componentsMap = saved->map("comps");
        loadEntity(saved, get(id).value());
    }
    body.hitbox.position = tsf.pos;
    scripting::on_entity_spawn(
        def, id, scripting.components, std::move(args), std::move(componentsMap));
    return id;
}

void Entities::despawn(entityid_t id) {
    if (auto entity = get(id)) {
        auto& eid = entity->getID();
        if (!eid.destroyFlag) {
            eid.destroyFlag = true;
            scripting::on_entity_despawn(entity->getDef(), *entity);
        }
    }
}

void Entities::loadEntity(const dynamic::Map_sptr& map) {
    entityid_t uid = 0;
    std::string defname;
    map->num("uid", uid);
    map->str("def", defname);
    if (uid == 0) {
        throw std::runtime_error("could not read entity - invalid UID");
    }
    auto& def = level->content->entities.require(defname);
    spawn(def, {}, dynamic::NONE, map, uid);
}

void Entities::loadEntity(const dynamic::Map_sptr& map, Entity entity) {
    auto& transform = entity.getTransform();
    auto& body = entity.getRigidbody();
    auto& rig = entity.getModeltree();

    if (auto bodymap = map->map(COMP_RIGIDBODY)) {
        dynamic::get_vec(bodymap, "vel", body.hitbox.velocity);
        std::string bodyTypeName;
        bodymap->str("type", bodyTypeName);
        if (auto bodyType = BodyType_from(bodyTypeName)) {
            body.hitbox.type = *bodyType;
        }
        bodymap->flag("crouch", body.hitbox.crouching);
        bodymap->num("damping", body.hitbox.linearDamping);
    }
    if (auto tsfmap = map->map(COMP_TRANSFORM)) {
        dynamic::get_vec(tsfmap, "pos", transform.pos);
        dynamic::get_vec(tsfmap, "size", transform.size);
        dynamic::get_mat(tsfmap, "rot", transform.rot);
    }
    std::string rigName = rig.config->getName();
    map->str("rig", rigName);
    if (rigName != rig.config->getName()) {
        rig.config = level->content->getRig(rigName);
    }
    if (auto rigmap = map->map(COMP_MODELTREE)) {
        if (auto texturesmap = rigmap->map("textures")) {
            for (auto& [slot, _] : texturesmap->values) {
                texturesmap->str(slot, rig.textures[slot]);
            }
        }
        if (auto posearr = rigmap->list("pose")) {
            for (size_t i = 0; i < std::min(rig.pose.matrices.size(), posearr->size()); i++) {
                dynamic::get_mat(posearr, i, rig.pose.matrices[i]);
            }
        }
    }
}

void Entities::loadEntities(dynamic::Map_sptr root) {
    auto list = root->list("data");
    for (size_t i = 0; i < list->size(); i++) {
        try {
            loadEntity(list->map(i));
        } catch (const std::runtime_error& err) {
            logger.error() << "could not read entity: " << err.what();
        }
    }
}

void Entities::onSave(const Entity& entity) {
    scripting::on_entity_save(entity);
}

dynamic::Value Entities::serialize(const Entity& entity) {
    auto root = dynamic::create_map();
    auto& eid = entity.getID();
    auto& def = eid.def;
    root->put("def", def.name);
    root->put("uid", eid.uid);
    {
        auto& transform = entity.getTransform();
        auto& tsfmap = root->putMap(COMP_TRANSFORM);
        tsfmap.put("pos", dynamic::to_value(transform.pos));
        if (transform.size != glm::vec3(1.0f)) {
            tsfmap.put("size", dynamic::to_value(transform.size));
        }
        if (transform.rot != glm::mat3(1.0f)) {
            tsfmap.put("rot", dynamic::to_value(transform.rot));
        }
    }
    {
        auto& rigidbody = entity.getRigidbody();
        auto& hitbox = rigidbody.hitbox;
        auto& bodymap = root->putMap(COMP_RIGIDBODY);
        if (!rigidbody.enabled) {
            bodymap.put("enabled", rigidbody.enabled);
        }
        if (def.save.body.velocity) {
            bodymap.put("vel", dynamic::to_value(rigidbody.hitbox.velocity));
        }
        if (def.save.body.settings) {
            bodymap.put("damping", rigidbody.hitbox.linearDamping);
            if (hitbox.type != def.bodyType) {
                bodymap.put("type", to_string(hitbox.type));
            }
            if (hitbox.crouching) {
                bodymap.put("crouch", hitbox.crouching);
            }
        }
    }
    auto& rig = entity.getModeltree();
    if (rig.config->getName() != def.rigName) {
        root->put("rig", rig.config->getName());
    }
    if (def.save.rig.pose || def.save.rig.textures) {
        auto& rigmap = root->putMap(COMP_MODELTREE);
        if (def.save.rig.textures) {
            auto& map = rigmap.putMap("textures");
            for (auto& [slot, texture] : rig.textures) {
                map.put(slot, texture);
            }
        }
        if (def.save.rig.pose) {
            auto& list = rigmap.putList("pose");
            for (auto& mat : rig.pose.matrices) {
                list.put(dynamic::to_value(mat));
            }
        }
    }
    auto& scripts = entity.getScripting();
    if (!scripts.components.empty()) {
        auto& compsMap = root->putMap("comps");
        for (auto& comp : scripts.components) {
            auto data = scripting::get_component_value(comp->env, "SAVED_DATA");
            compsMap.put(comp->name, data);
        }
    }
    return root;
}

void Entities::clean() {
    for (auto it = entities.begin(); it != entities.end();) {
        if (!registry.get<EntityId>(it->second).destroyFlag) {
            ++it;
        } else {
            uids.erase(it->second);
            registry.destroy(it->second);
            it = entities.erase(it);
        }
    }
}

void Entities::preparePhysics() {
    static uint64_t frameid = 0;
    frameid++;
    auto view = registry.view<EntityId, Transform, Rigidbody>();
    auto physics = level->physics.get();
    std::vector<Sensor*> sensors;
    for (auto [entity, eid, transform, rigidbody] : view.each()) {
        if (!rigidbody.enabled) {
            continue;
        }
        // TODO: temporary optimization until threaded solution
        if ((eid.uid + frameid) % 3 != 0) {
            continue;
        }
        for (size_t i = 0; i < rigidbody.sensors.size(); i++) {
            auto& sensor = rigidbody.sensors[i];
            for (auto oid : sensor.prevEntered) {
                if (sensor.nextEntered.find(oid) == sensor.nextEntered.end()) {
                    sensor.exitCallback(sensor.entity, i, oid);
                }
            }
            sensor.prevEntered = sensor.nextEntered;
            sensor.nextEntered.clear();

            switch (sensor.type) {
                case SensorType::AABB:
                    sensor.calculated.aabb = sensor.params.aabb;
                    sensor.calculated.aabb.transform(transform.combined);
                    break;
                case SensorType::RADIUS:
                    sensor.calculated.radial = glm::vec4(
                        rigidbody.hitbox.position.x,
                        rigidbody.hitbox.position.y,
                        rigidbody.hitbox.position.z,
                        sensor.params.radial.w*
                        sensor.params.radial.w);
                    break;
            }
            sensors.push_back(&sensor);
        }
    }
    physics->setSensors(std::move(sensors));
}

void Entities::updatePhysics(float delta) {
    preparePhysics();

    auto view = registry.view<EntityId, Transform, Rigidbody>();
    auto physics = level->physics.get();
    for (auto [entity, eid, transform, rigidbody] : view.each()) {
        if (!rigidbody.enabled) {
            continue;
        }
        auto& hitbox = rigidbody.hitbox;
        auto prevVel = hitbox.velocity;
        bool grounded = hitbox.grounded;

        float vel = glm::length(prevVel);
        int substeps = static_cast<int>(delta * vel * 20);
        substeps = std::min(100, std::max(2, substeps));
        physics->step(
            level->chunks.get(),
            &hitbox,
            delta,
            substeps,
            eid.uid
        );
        hitbox.linearDamping = hitbox.grounded * 24;
        transform.setPos(hitbox.position);
        if (hitbox.grounded && !grounded) {
            scripting::on_entity_grounded(
                *get(eid.uid), glm::length(prevVel-hitbox.velocity));
        }
        if (!hitbox.grounded && grounded) {
            scripting::on_entity_fall(*get(eid.uid));
        }
    }
}

void Entities::update() {
    scripting::on_entities_update();
}

void Entities::renderDebug(LineBatch& batch, const Frustum& frustum) {
    batch.lineWidth(1.0f);
    auto view = registry.view<Transform, Rigidbody>();
    for (auto [entity, transform, rigidbody] : view.each()) {
        const auto& hitbox = rigidbody.hitbox;
        const auto& pos = transform.pos;
        const auto& size = transform.size;
        if (!frustum.isBoxVisible(pos-size, pos+size)) {
            continue;
        }
        batch.box(hitbox.position, hitbox.halfsize * 2.0f, glm::vec4(1.0f));

        for (auto& sensor : rigidbody.sensors) {
            if (sensor.type != SensorType::AABB)
                continue;
            batch.box(
                sensor.calculated.aabb.center(), 
                sensor.calculated.aabb.size(), 
                glm::vec4(1.0f, 1.0f, 0.0f, 1.0f));
        }
    }
}

void Entities::render(Assets* assets, ModelBatch& batch, const Frustum& frustum, bool pause) {
    if (!pause) {
        scripting::on_entities_render();
    }

    auto view = registry.view<Transform, rigging::Rig>();
    for (auto [entity, transform, rig] : view.each()) {
        if (transform.dirty) {
            transform.refresh();
        }
        const auto& pos = transform.pos;
        const auto& size = transform.size;
        if (frustum.isBoxVisible(pos-size, pos+size)) {
            const auto* rigConfig = rig.config;
            rigConfig->render(assets, batch, rig, transform.combined);
        }
    }
}

std::vector<Entity> Entities::getAllInside(AABB aabb) {
    std::vector<Entity> collected;
    auto view = registry.view<Transform>();
    for (auto [entity, transform] : view.each()) {
        if (aabb.contains(transform.pos)) {
            const auto& found = uids.find(entity);
            if (found == uids.end()) {
                continue;
            }
            if (auto wrapper = get(found->second)) {
                collected.push_back(*wrapper);
            }
        }
    }
    return collected;
}
