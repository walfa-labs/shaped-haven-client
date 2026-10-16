#include "mesh_factory.h"

#include <OgreHardwareBufferManager.h>
#include <OgreHardwareIndexBuffer.h>
#include <OgreMaterialManager.h>
#include <OgreMeshManager.h>
#include <OgrePass.h>
#include <OgreSubMesh.h>
#include <OgreTechnique.h>
#include <OgreVertexIndexData.h>

#include <cmath>
#include <vector>

namespace haven {

namespace {

constexpr const char* kResourceGroup = "ShapedHaven";

/// Interleaved vertex layout shared by every primitive: position then normal.
constexpr size_t kFloatsPerVertex = 6;
constexpr size_t kVertexSize = kFloatsPerVertex * sizeof(float);

struct Geometry {
    std::vector<float> vertices;
    std::vector<uint16_t> indices;
    float extent = 1.0f;
};

void add_quad(Geometry& g, float hx, float hy)
{
    const uint16_t base = static_cast<uint16_t>(g.vertices.size() / kFloatsPerVertex);
    const float corners[4][2] = {{-hx, -hy}, {hx, -hy}, {hx, hy}, {-hx, hy}};
    for (const auto& corner : corners)
    {
        g.vertices.push_back(corner[0]);
        g.vertices.push_back(corner[1]);
        g.vertices.push_back(0.0f);
        g.vertices.push_back(0.0f);
        g.vertices.push_back(0.0f);
        g.vertices.push_back(1.0f);
    }
    g.indices.push_back(base + 0);
    g.indices.push_back(base + 1);
    g.indices.push_back(base + 2);
    g.indices.push_back(base + 0);
    g.indices.push_back(base + 2);
    g.indices.push_back(base + 3);
}

void add_fan(Geometry& g, const std::vector<Ogre::Vector3>& ring, bool close_ring)
{
    const uint16_t base = static_cast<uint16_t>(g.vertices.size() / kFloatsPerVertex);
    for (const Ogre::Vector3& v : ring)
    {
        g.vertices.push_back(v.x);
        g.vertices.push_back(v.y);
        g.vertices.push_back(0.0f);
        g.vertices.push_back(0.0f);
        g.vertices.push_back(0.0f);
        g.vertices.push_back(1.0f);
    }
    const size_t n = ring.size();
    for (size_t i = 1; i + 1 < n; ++i)
    {
        g.indices.push_back(static_cast<uint16_t>(base));
        g.indices.push_back(static_cast<uint16_t>(base + i));
        g.indices.push_back(static_cast<uint16_t>(base + i + 1));
    }
    if (close_ring && n >= 3)
    {
        g.indices.push_back(static_cast<uint16_t>(base));
        g.indices.push_back(static_cast<uint16_t>(base + n - 1));
        g.indices.push_back(static_cast<uint16_t>(base + 1));
    }
}

}  // namespace

MeshFactory::MeshFactory(Ogre::SceneManager* scene_manager) : scene_manager_(scene_manager) {}

Ogre::MeshPtr MeshFactory::build(Shape shape, float radius, const std::string& name)
{
    Ogre::MeshPtr mesh = Ogre::MeshManager::getSingleton().createManual(name, kResourceGroup);
    Ogre::SubMesh* sub = mesh->createSubMesh();

    Geometry g;
    switch (shape)
    {
        case Shape::Square:
            add_quad(g, radius, radius);
            g.extent = radius;
            break;
        case Shape::Circle:
        {
            constexpr uint32_t kSegments = 32;
            std::vector<Ogre::Vector3> ring;
            ring.reserve(kSegments + 1);
            for (uint32_t i = 0; i <= kSegments; ++i)
            {
                const float a = static_cast<float>(i) * (Ogre::Math::TWO_PI / static_cast<float>(kSegments));
                ring.emplace_back(radius * std::cos(a), radius * std::sin(a), 0.0f);
            }
            add_fan(g, ring, false);
            g.extent = radius;
            break;
        }
        case Shape::Triangle:
        {
            std::vector<Ogre::Vector3> ring;
            ring.reserve(3);
            for (uint32_t i = 0; i < 3; ++i)
            {
                const float a = Ogre::Math::PI * 0.5f + static_cast<float>(i) * (Ogre::Math::TWO_PI / 3.0f);
                ring.emplace_back(radius * std::cos(a), radius * std::sin(a), 0.0f);
            }
            add_fan(g, ring, true);
            g.extent = radius;
            break;
        }
    }

    Ogre::VertexData* vertex_data = new Ogre::VertexData();
    vertex_data->vertexStart = 0;
    vertex_data->vertexCount = g.vertices.size() / kFloatsPerVertex;

    Ogre::VertexDeclaration* decl = vertex_data->vertexDeclaration;
    decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    decl->addElement(0, 3 * sizeof(float), Ogre::VET_FLOAT3, Ogre::VES_NORMAL);

    Ogre::HardwareVertexBufferSharedPtr vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        kVertexSize, vertex_data->vertexCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    vertex_data->vertexBufferBinding->setBinding(0, vbuf);
    vbuf->writeData(0, vbuf->getSizeInBytes(), g.vertices.data(), true);

    Ogre::HardwareIndexBufferSharedPtr ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, g.indices.size(), Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    ibuf->writeData(0, ibuf->getSizeInBytes(), g.indices.data(), true);

    sub->indexData->indexStart = 0;
    sub->indexData->indexCount = g.indices.size();
    sub->indexData->indexBuffer = ibuf;
    sub->useSharedVertices = true;
    mesh->sharedVertexData = vertex_data;

    const float e = g.extent;
    mesh->_setBounds(Ogre::AxisAlignedBox(-e, -e, -0.01f, e, e, 0.01f));
    mesh->_setBoundingSphereRadius(e);
    mesh->load();

    return mesh;
}

Ogre::MaterialPtr MeshFactory::flat_material(const std::string& name, const Ogre::ColourValue& colour)
{
    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create(name, kResourceGroup);
    Ogre::Pass* pass = material->getTechnique(0)->getPass(0);
    pass->setAmbient(colour);
    pass->setDiffuse(colour);
    pass->setSelfIllumination(colour);
    material->load();
    return material;
}

}  // namespace haven
