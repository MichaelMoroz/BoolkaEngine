#include "stdafx.h"

#include "OBJConverter.h"

#include <DirectXMath.h>
#include <d3d12.h>

#include "BoolkaCommon/DebugHelpers/DebugFileWriter.h"
#include "BoolkaCommon/DebugHelpers/DebugTimer.h"
#include "BoolkaCommon/Structures/MemoryBlock.h"
#include "BoolkaCommon/Structures/Sphere.h"
#include "D3D12Backend/Containers/Streaming/SceneData.h"
#include "D3D12Backend/HLSLShared.h"
#include "ThirdParty/DirectXMesh/DirectXMesh/DirectXMesh.h"

namespace Boolka
{
    static const size_t gs_ResourceAlignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    static const size_t gs_PitchAlignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    static const size_t gs_CubeMapFaces = 6;

    struct [[nodiscard]] BoolkaMaterial
    {
        BoolkaMaterial()
            : gpuMatData{} {};
        BoolkaMaterial(const tinyobj::material_t& tinyObjMat);

        std::string diffuseTexName;
        HLSLShared::MaterialData gpuMatData;

        bool operator==(const BoolkaMaterial& other) const;
    };

    BoolkaMaterial::BoolkaMaterial(const tinyobj::material_t& tinyObjMat)
        : diffuseTexName{tinyObjMat.diffuse_texname}
    {
        gpuMatData.diffuse =
            Vector3{tinyObjMat.diffuse[0], tinyObjMat.diffuse[1], tinyObjMat.diffuse[2]};
        gpuMatData.transparency = tinyObjMat.dissolve;
        gpuMatData.specular =
            Vector3{tinyObjMat.specular[0], tinyObjMat.specular[1], tinyObjMat.specular[2]};
        gpuMatData.specularExp = tinyObjMat.shininess;
        gpuMatData.indexOfRefraction = tinyObjMat.ior;
    }

    bool BoolkaMaterial::operator==(const BoolkaMaterial& other) const
    {
        return diffuseTexName == other.diffuseTexName &&
               memcmp(&gpuMatData, &other.gpuMatData, sizeof(gpuMatData)) == 0;
    }

    struct [[nodiscard]] BoolkaMaterialHash
    {
        size_t operator()(const Boolka::BoolkaMaterial& k) const
        {
            size_t currentHash = std::hash<std::string>()(k.diffuseTexName);

            const unsigned char* gpuMatData = ptr_static_cast<const unsigned char*>(&k.gpuMatData);
            for (size_t i = 0; i < sizeof(Boolka::HLSLShared::MaterialData); ++i)
            {
                currentHash = BLK_SHIFT_LEFT_WITH_CARRY(size_t, currentHash);
                currentHash ^= std::hash<unsigned char>()(gpuMatData[i]);
            }

            return currentHash;
        }
    };

    class [[nodiscard]] ObjConverterImpl
    {
    public:
        ObjConverterImpl();
        ~ObjConverterImpl() = default;

        bool Convert(const std::wstring& inFile, const std::wstring& outFolder);

    private:
        bool Build();
        bool WriteScene(const std::wstring& outFile);

        void Reset();

        struct UniqueVertexKey
        {
            int vertexIndex;
            int normalIndex;
            int texcoordIndex;

            bool operator<(const UniqueVertexKey& other) const;
        };

        // Loads OBJ
        bool Load(std::wstring inFile);

        // Geometry
        void ProcessGeometry();
        void ProcessVerticesIndices();
        void RemapVertices(std::map<UniqueVertexKey, uint32_t>& verticesMap);

        // Parses textures
        void RemapMaterials();
        [[nodiscard]] bool IsTransparent(const tinyobj::material_t& material);

        // SkyBox
        void PrepareSkyBox();

        // Serialization
        void WriteHeader(DebugFileWriter& fileWriter);
        void WriteTextureHeaders(DebugFileWriter& fileWriter);
        void WriteSkyBoxTextures(DebugFileWriter& fileWriter);
        void WriteSceneTextures(DebugFileWriter& fileWriter);

        template <typename T>
        void WriteVector(DebugFileWriter& fileWriter, const std::vector<T>& vertexDataVector,
                         size_t alignment);
        template <typename pixelType, typename sumType = pixelType>
        void WriteMIPChain(DebugFileWriter& fileWriter, const unsigned char* textureData, int width,
                           int height);

        static const char* const ms_SkyBoxTexNames[gs_CubeMapFaces];

        // Loaded OBJ
        tinyobj::attrib_t m_Attrib;
        std::vector<tinyobj::shape_t> m_Shapes;
        std::vector<tinyobj::material_t> m_Materials;

        // Geometry
        std::vector<HLSLShared::VertexData1> m_VertexData1;
        std::vector<HLSLShared::VertexData2> m_VertexData2;
        // Contains uint32_t data, but declared as uint8_t since DirectXMesh requires uint8_t vector
        std::vector<uint8_t> m_VertexIndirection;
        std::vector<DirectX::MeshletTriangle> m_IndexData;
        std::vector<HLSLShared::MeshletData> m_Meshlets;
        std::vector<HLSLShared::MeshletCullData> m_MeshletsCull;
        std::vector<HLSLShared::ObjectData> m_Objects;
        size_t m_OpaqueObjectCount;

        // Materials
        std::vector<BoolkaMaterial> m_RemappedMaterials;
        std::vector<HLSLShared::MaterialData> m_MaterialData;
        std::unordered_map<BoolkaMaterial, int, BoolkaMaterialHash> m_MaterialsMap;

        // SkyBox
        UINT m_SkyBoxTextureResolution;
        UINT m_SkyBoxMipCount;

        // Raytracing data
        std::vector<uint32_t> m_RTIndexData;
        std::vector<uint32_t> m_RTOjbectIndexOffsetData;
        std::vector<SceneData::CPUObjectHeader> m_CpuObjects;
    };

    const char* const ObjConverterImpl::ms_SkyBoxTexNames[gs_CubeMapFaces] = {
        "skybox\\px.hdr", "skybox\\nx.hdr", "skybox\\py.hdr",
        "skybox\\ny.hdr", "skybox\\pz.hdr", "skybox\\nz.hdr"};

    ObjConverterImpl::ObjConverterImpl()
    {
        Reset();
    }

    bool ObjConverterImpl::Build()
    {
        std::wcout << "Building data" << std::endl;

        std::wcout << "Processing geometry" << std::endl;
        ProcessGeometry();
        std::wcout << "Processing skybox" << std::endl;
        PrepareSkyBox();

        return true;
    }

    bool ObjConverterImpl::WriteScene(const std::wstring& outFolder)
    {
        std::wstring outHeaderFilePath;
        CombinePath(outFolder, BLK_SCENE_HEADER_FILENAME, outHeaderFilePath);
        std::wstring outDataFilePath;
        CombinePath(outFolder, BLK_SCENE_DATA_FILENAME, outDataFilePath);

        DebugFileWriter headerFileWriter;
        bool res = headerFileWriter.OpenFile(outHeaderFilePath.c_str());
        if (!res)
        {
            std::wcout << "Failed to open file " << outHeaderFilePath << " for writing"
                       << std::endl;
            return false;
        }
        DebugFileWriter dataFileWriter;
        res = dataFileWriter.OpenFile(outDataFilePath.c_str());
        if (!res)
        {
            std::wcout << "Failed to open file " << outDataFilePath << " for writing" << std::endl;
            return false;
        }

        WriteHeader(headerFileWriter);

        WriteTextureHeaders(headerFileWriter);

        WriteVector(headerFileWriter, m_CpuObjects, 0);
        std::cout << "Written rt index buffer 1" << std::endl;

        headerFileWriter.Close(BLK_FILE_BLOCK_SIZE);

        WriteVector(dataFileWriter, m_VertexData1, gs_ResourceAlignment);
        std::cout << "Written vertex buffer 1" << std::endl;

        WriteVector(dataFileWriter, m_VertexData2, gs_ResourceAlignment);
        std::cout << "Written vertex buffer 2" << std::endl;

        WriteVector(dataFileWriter, m_VertexIndirection, gs_ResourceAlignment);
        std::cout << "Written vertex indirection buffer" << std::endl;

        WriteVector(dataFileWriter, m_IndexData, gs_ResourceAlignment);
        std::cout << "Written index buffer" << std::endl;

        WriteVector(dataFileWriter, m_Meshlets, gs_ResourceAlignment);
        std::cout << "Written meshlets buffer" << std::endl;

        WriteVector(dataFileWriter, m_MeshletsCull, gs_ResourceAlignment);
        std::cout << "Written meshlets cull buffer" << std::endl;

        WriteVector(dataFileWriter, m_Objects, gs_ResourceAlignment);
        std::cout << "Written objects buffer" << std::endl;

        WriteVector(dataFileWriter, m_MaterialData, gs_ResourceAlignment);
        std::cout << "Written material buffer" << std::endl;

        WriteVector(dataFileWriter, m_RTIndexData, gs_ResourceAlignment);
        std::cout << "Written RT index buffer" << std::endl;

        WriteVector(dataFileWriter, m_RTOjbectIndexOffsetData, gs_ResourceAlignment);
        std::cout << "Written RT object index offset buffer" << std::endl;

        WriteSkyBoxTextures(dataFileWriter);
        std::cout << "Written skybox textures" << std::endl;

        WriteSceneTextures(dataFileWriter);
        std::cout << "Written scene textures" << std::endl;

        res = dataFileWriter.Close(BLK_FILE_BLOCK_SIZE);
        BLK_CRITICAL_ASSERT(res);

        if (!res)
        {
            std::cout << "Failed to close file" << std::endl;
            return false;
        }

        std::wcout << "Successfully written scene to " << outFolder << std::endl;

        return true;
    }

    void ObjConverterImpl::Reset()
    {
        m_Attrib = {};
        m_Shapes.clear();
        m_Materials.clear();

        m_VertexData1.clear();
        m_VertexData2.clear();
        m_VertexIndirection.clear();
        m_IndexData.clear();
        m_Meshlets.clear();
        m_Objects.clear();

        m_OpaqueObjectCount = 0;

        m_RemappedMaterials.clear();

        m_SkyBoxTextureResolution = 0;

        m_MaterialsMap.clear();
    }

#ifdef BLK_DEBUG
    void ValidateMeshlet(const DirectX::Meshlet& meshlet, const DirectX::CullData& cullData,
                         const DirectX::XMFLOAT3* verticies, const uint32_t* vertexIndirection,
                         const DirectX::MeshletTriangle* meshletTriangles)
    {
        BLK_ASSERT(meshlet.PrimCount <= BLK_MESHLET_MAX_PRIMS &&
                   meshlet.VertCount <= BLK_MESHLET_MAX_VERTS);

        for (size_t j = 0; j < meshlet.PrimCount; ++j)
        {
            size_t primIndex = meshlet.PrimOffset + j;
            const auto& triangle = meshletTriangles[primIndex];

            BLK_ASSERT(triangle.i0 < BLK_MESHLET_MAX_VERTS);
            BLK_ASSERT(triangle.i1 < BLK_MESHLET_MAX_VERTS);
            BLK_ASSERT(triangle.i2 < BLK_MESHLET_MAX_VERTS);
        }

        if (((cullData.NormalCone.v >> 24) & 0xFF) == 0xFF)
            return;

        Vector4 center = Vector4(cullData.BoundingSphere.Center.x, cullData.BoundingSphere.Center.y,
                                 cullData.BoundingSphere.Center.z, 1.0f);

        Vector4 axis;
        float angle;
        axis.x() = float((cullData.NormalCone.v >> 0) & 0xFF);
        axis.y() = float((cullData.NormalCone.v >> 8) & 0xFF);
        axis.z() = float((cullData.NormalCone.v >> 16) & 0xFF);
        angle = float(((cullData.NormalCone.v >> 24) & 0xFF));

        axis /= 255.0f;
        angle /= 255.0f;

        axis = axis * 2.0f - Vector4(1.0f, 1.0f, 1.0f, 0.0f);

        Vector4 apex = center - axis * cullData.ApexOffset;

        DirectX::XMFLOAT3 localVertices[BLK_MESHLET_MAX_VERTS]{};

        for (size_t i = 0; i < meshlet.VertCount; ++i)
        {
            size_t index = vertexIndirection[meshlet.VertOffset + i];
            localVertices[i] = verticies[index];
        }

        for (size_t j = 0; j < meshlet.PrimCount; ++j)
        {
            size_t primIndex = meshlet.PrimOffset + j;
            const auto& triangle = meshletTriangles[primIndex];

            DirectX::XMFLOAT3 verts[3] = {
                localVertices[triangle.i0],
                localVertices[triangle.i1],
                localVertices[triangle.i2],
            };

            for (const auto& vert : verts)
            {
                Vector4 vertex = Vector4(vert.x, vert.y, vert.z, 1.0f);
                float dist = (center - vertex).LengthSlow() - 1e-5f;
                BLK_ASSERT(dist < cullData.BoundingSphere.Radius);
            }

            Vector4 blkVerticies[3] = {
                {verts[0].x, verts[0].y, verts[0].z, 0.0f},
                {verts[1].x, verts[1].y, verts[1].z, 0.0f},
                {verts[2].x, verts[2].y, verts[2].z, 0.0f},
            };

            Vector4 vectors[2] = {blkVerticies[0] - blkVerticies[1],
                                  blkVerticies[0] - blkVerticies[2]};

            Vector4 normal = vectors[0].Cross(vectors[1]).Normalize();
            Vector4 viewDir = normal;

            float calculatedAngle = viewDir.Dot(-axis);
            BLK_ASSERT(calculatedAngle <= angle);
        }
    }
#else

    void ValidateMeshlet(const DirectX::Meshlet& meshlet, const DirectX::CullData& cullData,
                         const DirectX::XMFLOAT3* verticies, const uint32_t* vertexIndirection,
                         const DirectX::MeshletTriangle* meshletTriangles){};
#endif

    bool ObjConverterImpl::Convert(const std::wstring& inFile, const std::wstring& outFolder)
    {
        std::wcout << "Loading file:" << inFile << std::endl;

        if (!Load(inFile))
        {
            std::wcout << "Failed to load file:" << inFile << std::endl;
            return false;
        }

        std::wcout << inFile << " Loaded successfully" << std::endl;
        if (!Build())
        {
            std::wcout << "Failed to build" << std::endl;
            return false;
        }

        if (!WriteScene(outFolder))
        {
            std::wcout << "Failed to build" << std::endl;
            return false;
        }

        Reset();

        return true;
    }

    bool ObjConverterImpl::Load(std::wstring inFile)
    {
        std::string warn;
        std::string err;

        std::string inFileA = UTF8encode(inFile);

        DebugTimer timer;
        timer.Start();
        bool ret = tinyobj::LoadObj(&m_Attrib, &m_Shapes, &m_Materials, &warn, &err,
                                    inFileA.c_str(), NULL, true);
        float seconds = timer.Stop();
        std::cout << "tinyobj::LoadObj tooks " << seconds << "s" << std::endl;
        if (!warn.empty())
        {
            std::cerr << "tinyobj warning:\n" << warn;
        }

        if (!err.empty())
        {
            std::cerr << "tinyobj error:\n" << err;
        }

        return ret;
    }

    void ObjConverterImpl::ProcessGeometry()
    {
        RemapMaterials();
        ProcessVerticesIndices();
    }

    void ObjConverterImpl::RemapMaterials()
    {
        m_MaterialsMap.reserve(m_Materials.size());
        int currentMaterialIndex = 0;

        for (const auto& material : m_Materials)
        {
            if (m_MaterialsMap.find(material) == m_MaterialsMap.end())
            {
                m_MaterialsMap[material] = currentMaterialIndex++;
            }
        }

        m_RemappedMaterials.resize(m_MaterialsMap.size());
        m_MaterialData.resize(m_MaterialsMap.size());
        for (const auto& [boolkaMaterial, materialIndex] : m_MaterialsMap)
        {
            m_RemappedMaterials[materialIndex] = boolkaMaterial;
            m_MaterialData[materialIndex] = boolkaMaterial.gpuMatData;
        }

        std::cout << "Remapped materials" << std::endl;
    }

    // TODO find better way to determine whether material have transparency
    bool ObjConverterImpl::IsTransparent(const tinyobj::material_t& material)
    {
        if (material.illum > 2)
            return true;

        const auto& filename = material.diffuse_texname;

        if (filename.empty())
        {
            return false;
        }

        static std::unordered_map<std::string, bool> s_calculated;

        auto iter = s_calculated.find(filename);
        if (iter != s_calculated.end())
        {
            return iter->second;
        }

        int width, height, bitsPerPixel;
        unsigned char* textureData = stbi_load(filename.c_str(), &width, &height, &bitsPerPixel, 4);

        bool result = false;

        for (size_t pixel = 0; pixel < static_cast<size_t>(width) * height; ++pixel)
        {
            if (textureData[pixel * 4 + 3] != 255)
            {
                result = true;
                break;
            }
        }

        stbi_image_free(textureData);

        s_calculated[filename] = result;
        return result;
    }

    void ObjConverterImpl::PrepareSkyBox()
    {
        int width, height, dummy;
        int result = stbi_info(ms_SkyBoxTexNames[0], &width, &height, &dummy);

        BLK_CRITICAL_ASSERT(width == height);

        UINT mipCount = 0;
        int dimension = width;
        while (dimension)
        {
            mipCount++;
            dimension >>= 1;
        }

        m_SkyBoxTextureResolution = width;
        m_SkyBoxMipCount = mipCount;
    }

    void ObjConverterImpl::ProcessVerticesIndices()
    {
        uint32_t currentIndex = 0;

        std::map<UniqueVertexKey, uint32_t> verticesMap;
        RemapVertices(verticesMap);

        const auto& vertices = m_Attrib.vertices;
        std::vector<DirectX::XMFLOAT3> dxVertices;

        dxVertices.resize(verticesMap.size());

        for (auto& [uniqueVertex, arrayIndex] : verticesMap)
        {
            int posIndex = uniqueVertex.vertexIndex;
            dxVertices[arrayIndex] = DirectX::XMFLOAT3{
                vertices[3 * posIndex], vertices[3 * posIndex + 2], vertices[3 * posIndex + 1]};
        }

        const size_t nVerts = dxVertices.size();

        std::vector<std::vector<HLSLShared::MeshletData>> processedMeshlets(m_Shapes.size());
        std::vector<std::vector<HLSLShared::MeshletCullData>> processedMeshletsCull(
            m_Shapes.size());
        std::vector<std::vector<uint8_t>> processedMeshletVertexIndirection(m_Shapes.size());
        std::vector<std::vector<DirectX::MeshletTriangle>> processedMeshletTriangles(
            m_Shapes.size());
        std::vector<HLSLShared::ObjectData> processedObjects(m_Shapes.size());
        std::vector<std::vector<uint32_t>> processedRtIndicies(m_Shapes.size());

        // Calculating all required data
        std::for_each(
            std::execution::par_unseq, std::begin(m_Shapes), std::end(m_Shapes),
            [&](tinyobj::shape_t& shape) {
                size_t shapeIndex = &shape - &m_Shapes[0];

                HLSLShared::ObjectData& object = processedObjects[shapeIndex];

                const auto& material = m_Materials[shape.mesh.material_ids[0]];

                int materialIndex = m_MaterialsMap[material];

                const auto& indices = shape.mesh.indices;
                BLK_CRITICAL_ASSERT(indices.size() % 3 == 0);
                size_t nFaces = indices.size() / 3;

                std::vector<uint32_t> dxIndices(indices.size());
                for (size_t i = 0; i < dxIndices.size(); ++i)
                {
                    const auto& index = indices[i];
                    UniqueVertexKey remappedVertexKey = {index.vertex_index, index.normal_index,
                                                         index.texcoord_index};
                    dxIndices[i] = verticesMap[remappedVertexKey];
                }

                object.boundingBox.GetMax() = {-FLT_MAX, -FLT_MAX, -FLT_MAX, 1.0f};
                object.boundingBox.GetMin() = {FLT_MAX, FLT_MAX, FLT_MAX, 1.0f};

                auto& positions = m_Attrib.vertices;

                for (size_t i = 0; i < indices.size(); i++)
                {
                    auto& index = indices[i];
                    int vertexIndex = index.vertex_index;
                    Vector4 xyz = {positions[3 * vertexIndex], positions[3 * vertexIndex + 2],
                                   positions[3 * vertexIndex + 1], 1.0f};
                    object.boundingBox.GetMax() = Max(object.boundingBox.GetMax(), xyz);
                    object.boundingBox.GetMin() = Min(object.boundingBox.GetMin(), xyz);
                }

                std::vector<uint32_t> adjacency(nFaces * 3);
                HRESULT hr = DirectX::GenerateAdjacencyAndPointReps(dxIndices.data(), nFaces,
                                                                    dxVertices.data(), nVerts, 0.0f,
                                                                    nullptr, adjacency.data());

                BLK_ASSERT_VAR2(SUCCEEDED(hr), hr);

                {
                    std::vector<DirectX::Meshlet> meshlets;

                    HRESULT hr = DirectX::ComputeMeshlets(
                        dxIndices.data(), nFaces, dxVertices.data(), nVerts, adjacency.data(),
                        meshlets, processedMeshletVertexIndirection[shapeIndex],
                        processedMeshletTriangles[shapeIndex], BLK_MESHLET_MAX_VERTS,
                        BLK_MESHLET_MAX_PRIMS);

                    BLK_ASSERT(SUCCEEDED(hr));

                    std::vector<DirectX::CullData> cullDataVector(meshlets.size());

                    hr = DirectX::ComputeCullData(
                        dxVertices.data(), nVerts, meshlets.data(), meshlets.size(),
                        ptr_static_cast<uint32_t*>(
                            processedMeshletVertexIndirection[shapeIndex].data()),
                        processedMeshletVertexIndirection[shapeIndex].size() /
                            (sizeof(uint32_t) / sizeof(uint8_t)),
                        processedMeshletTriangles[shapeIndex].data(),
                        processedMeshletTriangles[shapeIndex].size(), cullDataVector.data());

                    uint32_t* vertexIndirection2 = reinterpret_cast<uint32_t*>(
                        processedMeshletVertexIndirection[shapeIndex].data());

                    for (size_t i = 0; i < meshlets.size(); i++)
                    {
                        const auto& meshlet = meshlets[i];
                        const auto& cullData = cullDataVector[i];

                        ValidateMeshlet(meshlet, cullData, dxVertices.data(),
                                        ptr_static_cast<uint32_t*>(
                                            processedMeshletVertexIndirection[shapeIndex].data()),
                                        processedMeshletTriangles[shapeIndex].data());
                    }

                    size_t roundedSize = BLK_CEIL_TO_POWER_OF_TWO(meshlets.size(), 32);
                    meshlets.resize(roundedSize);
                    cullDataVector.resize(roundedSize);

                    BLK_ASSERT_VAR2(SUCCEEDED(hr), hr);

                    object.meshletCount = static_cast<uint32_t>(meshlets.size());

                    std::vector<HLSLShared::MeshletData>& processedMeshletVector =
                        processedMeshlets[shapeIndex];
                    std::vector<HLSLShared::MeshletCullData>& processedMeshletCullVector =
                        processedMeshletsCull[shapeIndex];

                    processedMeshletVector.resize(meshlets.size());
                    processedMeshletCullVector.resize(meshlets.size());

                    for (size_t i = 0; i < meshlets.size(); ++i)
                    {
                        HLSLShared::MeshletData& processedMeshlet = processedMeshletVector[i];
                        const auto& dxMeshlet = meshlets[i];

                        processedMeshlet.MaterialID =
                            checked_narrowing_cast<uint16_t>(materialIndex);
                        processedMeshlet.VertCount =
                            checked_narrowing_cast<uint16_t>(dxMeshlet.VertCount);
                        processedMeshlet.VertOffset = dxMeshlet.VertOffset;
                        processedMeshlet.PrimCount =
                            checked_narrowing_cast<uint16_t>(dxMeshlet.PrimCount);
                        processedMeshlet.PrimOffset = dxMeshlet.PrimOffset;

                        HLSLShared::MeshletCullData& processedMeshletCull =
                            processedMeshletCullVector[i];
                        const DirectX::CullData& cullData = cullDataVector[i];

                        processedMeshletCull.BoundingSphere = Vector4(
                            cullData.BoundingSphere.Center.x, cullData.BoundingSphere.Center.y,
                            cullData.BoundingSphere.Center.z, cullData.BoundingSphere.Radius);
                        processedMeshletCull.NormalCone = cullData.NormalCone.v;
                        processedMeshletCull.ApexOffset = cullData.ApexOffset;
                    }
                }

                {
                    std::vector<uint32_t> faceReorder(nFaces);
                    HRESULT hr = DirectX::OptimizeFaces(dxIndices.data(), nFaces, adjacency.data(),
                                                        faceReorder.data());

                    BLK_ASSERT_VAR2(SUCCEEDED(hr), hr);

                    auto& processedRtIndiciesVector = processedRtIndicies[shapeIndex];
                    processedRtIndiciesVector.resize(nFaces * 3);

                    for (size_t i = 0; i < nFaces; ++i)
                    {
                        uint32_t face = faceReorder[i];
                        processedRtIndiciesVector[3 * i] = dxIndices[3 * face];
                        processedRtIndiciesVector[3 * i + 1] = dxIndices[3 * face + 1];
                        processedRtIndiciesVector[3 * i + 2] = dxIndices[3 * face + 2];
                    }
                }
            });

        std::cout << "Processed meshlets" << std::endl;

        // Flattening data to prepare it for writing to disk
        const size_t totalMeshletCount = NestedVectorSize(processedMeshlets);
        m_Meshlets.reserve(totalMeshletCount);
        m_MeshletsCull.reserve(totalMeshletCount);
        m_VertexIndirection.reserve(NestedVectorSize(processedMeshletVertexIndirection));
        m_IndexData.reserve(NestedVectorSize(processedMeshletTriangles));
        m_Objects.reserve(m_Shapes.size());
        m_RTIndexData.reserve(NestedVectorSize(processedRtIndicies));
        m_RTOjbectIndexOffsetData.reserve(m_Shapes.size());
        m_CpuObjects.reserve(m_Shapes.size());

        size_t flattenedObjects = 0;
        for (size_t processTransparent = 0; processTransparent < 2; ++processTransparent)
        {
            for (size_t i = 0; i < m_Shapes.size(); ++i)
            {
                const auto& shape = m_Shapes[i];
                const auto& material = m_Materials[shape.mesh.material_ids[0]];
                bool isCurrentTransparent = IsTransparent(material);

                if (isCurrentTransparent != bool(processTransparent))
                    continue;

                ++flattenedObjects;

                HLSLShared::ObjectData currentObject = processedObjects[i];
                currentObject.meshletOffset = checked_narrowing_cast<uint32_t>(m_Meshlets.size());
                m_Objects.push_back(currentObject);

                size_t additionalVertOffset =
                    m_VertexIndirection.size() * sizeof(uint8_t) / sizeof(uint32_t);
                size_t additionalPrimOffset = m_IndexData.size();
                for (const auto& meshlet : processedMeshlets[i])
                {
                    HLSLShared::MeshletData currentMeshlet = meshlet;
                    currentMeshlet.VertOffset += static_cast<uint32_t>(additionalVertOffset);
                    currentMeshlet.PrimOffset += static_cast<uint32_t>(additionalPrimOffset);

                    m_Meshlets.push_back(currentMeshlet);
                }

                for (const auto& meshletCull : processedMeshletsCull[i])
                {
                    m_MeshletsCull.push_back(meshletCull);
                }

                SceneData::CPUObjectHeader currentCPUObject{};
                currentCPUObject.rtIndexOffset =
                    checked_narrowing_cast<uint32_t>(m_RTIndexData.size());
                currentCPUObject.rtIndexCount =
                    checked_narrowing_cast<uint32_t>(processedRtIndicies[i].size());
                int materialIndex = m_MaterialsMap[material];
                currentCPUObject.materialIndex = materialIndex;
                m_CpuObjects.push_back(currentCPUObject);
                m_RTOjbectIndexOffsetData.push_back(currentCPUObject.rtIndexOffset);

                m_VertexIndirection.insert(std::end(m_VertexIndirection),
                                           std::begin(processedMeshletVertexIndirection[i]),
                                           std::end(processedMeshletVertexIndirection[i]));
                m_IndexData.insert(std::end(m_IndexData), std::begin(processedMeshletTriangles[i]),
                                   std::end(processedMeshletTriangles[i]));
                m_RTIndexData.insert(std::end(m_RTIndexData), std::begin(processedRtIndicies[i]),
                                     std::end(processedRtIndicies[i]));
            }

            if (processTransparent == 0)
            {
                m_OpaqueObjectCount = flattenedObjects;
            }
        }

        std::cout << "Flattened geometry data" << std::endl;
    }

    void ObjConverterImpl::RemapVertices(std::map<UniqueVertexKey, uint32_t>& verticesMap)
    {
        for (const auto& shape : m_Shapes)
        {
            for (unsigned char vertexesPerFace : shape.mesh.num_face_vertices)
            {
                // Further code assume that there are only triangles
                BLK_ASSERT(vertexesPerFace == 3);
            }

            auto& mesh = shape.mesh;
            auto& indices = mesh.indices;
            for (size_t i = 0; i < indices.size(); i++)
            {
                auto& index = indices[i];
                size_t triangleIndex = i / 3;
                verticesMap.insert(std::pair(
                    UniqueVertexKey{index.vertex_index, index.normal_index, index.texcoord_index},
                    0));
            }

            for (unsigned char vertexesPerFace : shape.mesh.num_face_vertices)
            {
                // Further code assume that there are only triangles
                BLK_ASSERT_VAR2(vertexesPerFace == 3, vertexesPerFace);
            }

            std::cout << "Added vertices from shape: " << shape.name << std::endl;
        }

        const auto& positions = m_Attrib.vertices;
        const auto& normals = m_Attrib.normals;
        const auto& texcoords = m_Attrib.texcoords;

        uint32_t vertexIndex = 0;
        m_VertexData1.reserve(verticesMap.size());
        m_VertexData2.reserve(verticesMap.size());
        for (auto& [uniqueVertex, arrayIndex] : verticesMap)
        {
            HLSLShared::VertexData1 vertexData1{};
            HLSLShared::VertexData2 vertexData2{};

            if (uniqueVertex.vertexIndex >= 0)
            {
                // Swap y and z
                // In OBJ y is up, and in Boolka Engine z is up
                vertexData1.position = {positions[3ll * uniqueVertex.vertexIndex],
                                        positions[3ll * uniqueVertex.vertexIndex + 2],
                                        positions[3ll * uniqueVertex.vertexIndex + 1]};
            }
            else
            {
                vertexData1.position = Vector3{};
            }

            if (uniqueVertex.normalIndex >= 0)
            {
                // Swap y and z
                // In OBJ y is up, and in Boolka Engine z is up
                vertexData2.normal[0] = normals[3ll * uniqueVertex.normalIndex];
                vertexData2.normal[1] = normals[3ll * uniqueVertex.normalIndex + 2];
                vertexData2.normal[2] = normals[3ll * uniqueVertex.normalIndex + 1];
            }
            else
            {
                vertexData2.normal = Vector3{};
            }

            if (uniqueVertex.texcoordIndex >= 0)
            {
                // Flip y coordinate
                // In obj, y = 0 is bottom and in DirectX y = 0 is top
                vertexData1.texCoordX = texcoords[2ll * uniqueVertex.texcoordIndex];
                vertexData2.texCoordY = 1 - texcoords[2ll * uniqueVertex.texcoordIndex + 1];
            }
            else
            {
                vertexData1.texCoordX = 0.0f;
                vertexData2.texCoordY = 0.0f;
            }

            m_VertexData1.push_back(vertexData1);
            m_VertexData2.push_back(vertexData2);
            arrayIndex = vertexIndex++;
        }

        std::cout << "Processed vertices" << std::endl;
    }

    void ObjConverterImpl::WriteHeader(DebugFileWriter& fileWriter)
    {
        SceneData::FormatHeader formatHeader{};
        bool res = fileWriter.Write(&formatHeader, sizeof(formatHeader));

        std::srand((uint)std::time(nullptr)); // use current time as seed for random generator
        UINT sceneIdentifier = std::rand();

        SceneData::SceneHeader sceneHeader{
            .sceneIdentifier = sceneIdentifier,
            .vertex1Size = checked_narrowing_cast<UINT>(BLK_CEIL_TO_POWER_OF_TWO(
                m_VertexData1.size() * sizeof(m_VertexData1[0]), gs_ResourceAlignment)),
            .vertex2Size = checked_narrowing_cast<UINT>(BLK_CEIL_TO_POWER_OF_TWO(
                m_VertexData2.size() * sizeof(m_VertexData2[0]), gs_ResourceAlignment)),
            .vertexIndirectionSize = checked_narrowing_cast<UINT>(BLK_CEIL_TO_POWER_OF_TWO(
                m_VertexIndirection.size() * sizeof(m_VertexIndirection[0]), gs_ResourceAlignment)),
            .indexSize = checked_narrowing_cast<UINT>(BLK_CEIL_TO_POWER_OF_TWO(
                m_IndexData.size() * sizeof(m_IndexData[0]), gs_ResourceAlignment)),
            .meshletsSize = checked_narrowing_cast<UINT>(BLK_CEIL_TO_POWER_OF_TWO(
                m_Meshlets.size() * sizeof(m_Meshlets[0]), gs_ResourceAlignment)),
            .meshletsCullSize = checked_narrowing_cast<UINT>(BLK_CEIL_TO_POWER_OF_TWO(
                m_MeshletsCull.size() * sizeof(m_MeshletsCull[0]), gs_ResourceAlignment)),
            .objectsSize = checked_narrowing_cast<UINT>(BLK_CEIL_TO_POWER_OF_TWO(
                m_Objects.size() * sizeof(m_Objects[0]), gs_ResourceAlignment)),
            .materialsSize = checked_narrowing_cast<UINT>(BLK_CEIL_TO_POWER_OF_TWO(
                m_MaterialData.size() * sizeof(m_MaterialData[0]), gs_ResourceAlignment)),
            .rtIndiciesSize = checked_narrowing_cast<UINT>(BLK_CEIL_TO_POWER_OF_TWO(
                m_RTIndexData.size() * sizeof(m_RTIndexData[0]), gs_ResourceAlignment)),
            .rtObjectIndexOffsetSize = checked_narrowing_cast<UINT>(BLK_CEIL_TO_POWER_OF_TWO(
                m_RTOjbectIndexOffsetData.size() * sizeof(m_RTOjbectIndexOffsetData[0]),
                gs_ResourceAlignment)),
            .objectCount = checked_narrowing_cast<UINT>(m_Objects.size()),
            .opaqueCount = checked_narrowing_cast<UINT>(m_OpaqueObjectCount),
            .skyBoxResolution = m_SkyBoxTextureResolution,
            .skyBoxMipCount = m_SkyBoxMipCount,
            .textureCount = checked_narrowing_cast<UINT>(m_RemappedMaterials.size())};

        BLK_CRITICAL_ASSERT(sceneHeader.vertex1Size != 0);
        BLK_CRITICAL_ASSERT(sceneHeader.vertex2Size != 0);
        BLK_CRITICAL_ASSERT(sceneHeader.vertexIndirectionSize != 0);
        BLK_CRITICAL_ASSERT(sceneHeader.indexSize != 0);
        BLK_CRITICAL_ASSERT(sceneHeader.meshletsSize != 0);
        BLK_CRITICAL_ASSERT(sceneHeader.objectsSize != 0);
        BLK_CRITICAL_ASSERT(sceneHeader.materialsSize != 0);
        BLK_CRITICAL_ASSERT(sceneHeader.rtIndiciesSize != 0);
        BLK_CRITICAL_ASSERT(sceneHeader.rtObjectIndexOffsetSize != 0);
        BLK_CRITICAL_ASSERT(sceneHeader.objectCount != 0);
        BLK_CRITICAL_ASSERT(sceneHeader.opaqueCount != 0);
        BLK_CRITICAL_ASSERT(sceneHeader.skyBoxResolution != 0);
        BLK_CRITICAL_ASSERT(sceneHeader.skyBoxMipCount != 0);
        BLK_CRITICAL_ASSERT(sceneHeader.textureCount != 0);

        res = fileWriter.Write(&sceneHeader, sizeof(sceneHeader));
        BLK_ASSERT_VAR(res);

        std::cout << "Written header" << std::endl;
    }

    void ObjConverterImpl::WriteTextureHeaders(DebugFileWriter& fileWriter)
    {
        for (size_t i = 0; i < m_RemappedMaterials.size(); ++i)
        {
            const auto& material = m_RemappedMaterials[i];

            int width, height, dummy;

            UINT mipCount = 0;

            if (!material.diffuseTexName.empty())
            {
                int result = stbi_info(material.diffuseTexName.c_str(), &width, &height, &dummy);
                BLK_CRITICAL_ASSERT(result != 0);

                int dimension = std::min(width, height);
                while (dimension)
                {
                    mipCount++;
                    dimension >>= 1;
                }
            }
            else
            {
                width = 1;
                height = 1;
                mipCount = 1;
            }

            SceneData::TextureHeader textureHeader{checked_narrowing_cast<UINT>(width),
                                                   checked_narrowing_cast<UINT>(height), mipCount};

            fileWriter.Write(&textureHeader, sizeof(textureHeader));

            std::cout << "Scene texture " << i << " header processed\n";
        }

        std::cout << "Written texture headers" << std::endl;
    }

    template <typename T>
    void ObjConverterImpl::WriteVector(DebugFileWriter& fileWriter,
                                       const std::vector<T>& dataVector, size_t alignment)
    {
        size_t size = dataVector.size() * sizeof(T);
        bool res = fileWriter.Write(dataVector.data(), size);
        BLK_ASSERT_VAR(res);
        if (alignment > 0)
        {
            size_t modulo = size % alignment;
            if (modulo > 0)
            {
                fileWriter.AddPadding(alignment - modulo);
            }
        }
    }

    void ObjConverterImpl::WriteSkyBoxTextures(DebugFileWriter& fileWriter)
    {
        for (size_t i = 0; i < gs_CubeMapFaces; ++i)
        {
            const char* texName = ms_SkyBoxTexNames[i];

            int width, height, dummy;

            void* textureData = stbi_loadf(texName, &width, &height, &dummy, 4);

            BLK_CRITICAL_ASSERT(width == height && width == m_SkyBoxTextureResolution);

            BLK_CRITICAL_ASSERT(textureData);

            WriteMIPChain<Vector4>(fileWriter, static_cast<unsigned char*>(textureData), width,
                                   height);

            stbi_image_free(textureData);

            std::cout << "SkyBox texture " << i << " written" << std::endl;
        }
    }

    void ObjConverterImpl::WriteSceneTextures(DebugFileWriter& fileWriter)
    {
        for (size_t i = 0; i < m_RemappedMaterials.size(); ++i)
        {
            const auto& material = m_RemappedMaterials[i];

            if (!material.diffuseTexName.empty())
            {
                int width, height, bitsPerPixel;

                unsigned char* textureData =
                    stbi_load(material.diffuseTexName.c_str(), &width, &height, &bitsPerPixel, 4);

                BLK_CRITICAL_ASSERT(textureData);

                WriteMIPChain<Vector<4, unsigned char>, Vector<4, unsigned int>>(
                    fileWriter, textureData, width, height);

                stbi_image_free(textureData);
            }
            else
            {
                Vector<4, unsigned char> pixel{1, 1, 1, 1};
                WriteMIPChain<Vector<4, unsigned char>, Vector<4, unsigned int>>(
                    fileWriter, pixel.GetBuffer(), 1, 1);
            }

            std::cout << "Scene texture " << i << ":" << material.diffuseTexName << " written"
                      << std::endl;
        }
    }

    template <typename pixelType, typename sumType>
    void ObjConverterImpl::WriteMIPChain(DebugFileWriter& fileWriter,
                                         const unsigned char* textureData, int width, int height)
    {
        BLK_CRITICAL_ASSERT(width > 0);
        BLK_CRITICAL_ASSERT(height > 0);

        size_t srcRowPitch = sizeof(pixelType) * width;
        size_t mip0RowPitch =
            BLK_CEIL_TO_POWER_OF_TWO(sizeof(pixelType) * width, gs_PitchAlignment);
        size_t mip0Size = BLK_CEIL_TO_POWER_OF_TWO(mip0RowPitch * height, gs_ResourceAlignment);

        unsigned char* mip0Data = new unsigned char[mip0Size];

        MemcpyStrided(mip0Data, mip0RowPitch, textureData, srcRowPitch, height);

        fileWriter.Write(mip0Data, mip0Size);

        const unsigned char* prevMipData = mip0Data;
        size_t prevMipRowPitch = mip0RowPitch;

        for (size_t mipWidth = width / 2, mipHeight = height / 2; mipWidth > 0 && mipHeight > 0;
             mipWidth /= 2, mipHeight /= 2)
        {
            size_t rowPitch =
                BLK_CEIL_TO_POWER_OF_TWO(sizeof(pixelType) * mipWidth, gs_PitchAlignment);
            size_t mipSize = BLK_CEIL_TO_POWER_OF_TWO(rowPitch * mipHeight, gs_ResourceAlignment);
            unsigned char* mipData = new unsigned char[mipSize];

            for (size_t y = 0; y < mipHeight; ++y)
            {
                for (size_t x = 0; x < mipWidth; ++x)
                {
                    pixelType pixel{};
                    pixelType prevMipPixels[4]{};

                    for (size_t i = 0; i < 2; ++i)
                    {
                        for (size_t j = 0; j < 2; ++j)
                        {
                            memcpy(&prevMipPixels[2 * i + j],
                                   &prevMipData[prevMipRowPitch * (y * 2 + i) +
                                                sizeof(pixelType) * (x * 2 + j)],
                                   sizeof(pixelType));
                        }
                    }

                    sumType sum{};
                    for (const auto& data : prevMipPixels)
                    {
                        sum += static_cast<sumType>(data);
                    }
                    pixel = static_cast<pixelType>(sum / 4);

                    memcpy(&mipData[rowPitch * y + sizeof(pixelType) * x], &pixel,
                           sizeof(pixelType));
                }
            }

            delete[] prevMipData;
            prevMipData = mipData;
            prevMipRowPitch = rowPitch;

            fileWriter.Write(mipData, mipSize);
        }
    }

    bool ObjConverterImpl::UniqueVertexKey::operator<(const UniqueVertexKey& other) const
    {
        if (vertexIndex != other.vertexIndex)
        {
            return vertexIndex < other.vertexIndex;
        }

        if (normalIndex != other.normalIndex)
        {
            return normalIndex < other.normalIndex;
        }

        return texcoordIndex < other.texcoordIndex;
    }

    bool OBJConverter::Convert(std::wstring inFile, std::wstring outFolder)
    {
        ObjConverterImpl converter;
        return converter.Convert(inFile, outFolder);
    }

} // namespace Boolka
