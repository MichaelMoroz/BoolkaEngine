#include "stdafx.h"

#include "Scene.h"

#include "APIWrappers/Resources/Buffers/UploadBuffer.h"
#include "APIWrappers/Resources/ResourceTransition.h"
#include "BoolkaCommon/DebugHelpers/DebugProfileTimer.h"
#include "Contexts/RenderEngineContext.h"

namespace Boolka
{

    const DXGI_FORMAT Scene::ms_SkyBoxTextureFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
    const DXGI_FORMAT Scene::ms_SceneTexturesFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    Scene::Scene()
        : m_ObjectCount(0)
        , m_OpaqueObjectCount(0)
    {
    }

    Scene::~Scene()
    {
        BLK_ASSERT(m_ObjectCount == 0);
        BLK_ASSERT(m_OpaqueObjectCount == 0);
    }

    bool Scene::Initialize(Device& device, SceneData& sceneData, RenderEngineContext& engineContext)
    {
        BLK_ASSERT(m_ObjectCount == 0);
        BLK_ASSERT(m_OpaqueObjectCount == 0);

        const auto dataWrapper = sceneData.GetSceneWrapper();
        const auto sceneHeader = dataWrapper.header;
        bool res;

        res = m_VertexBuffer1.Initialize(device, sceneHeader.vertex1Size, D3D12_HEAP_TYPE_DEFAULT,
                                         D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
        BLK_ASSERT_VAR(res);

        res = m_VertexBuffer2.Initialize(device, sceneHeader.vertex2Size, D3D12_HEAP_TYPE_DEFAULT,
                                         D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
        BLK_ASSERT_VAR(res);

        res = m_VertexIndirectionBuffer.Initialize(
            device, sceneHeader.vertexIndirectionSize, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
        BLK_ASSERT_VAR(res);

        res = m_IndexBuffer.Initialize(device, sceneHeader.indexSize, D3D12_HEAP_TYPE_DEFAULT,
                                       D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
        BLK_ASSERT_VAR(res);

        res = m_MeshletBuffer.Initialize(device, sceneHeader.meshletsSize, D3D12_HEAP_TYPE_DEFAULT,
                                         D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
        BLK_ASSERT_VAR(res);

        res = m_ObjectBuffer.Initialize(device, sceneHeader.objectsSize, D3D12_HEAP_TYPE_DEFAULT,
                                        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
        BLK_ASSERT_VAR(res);

        res = m_SRVDescriptorHeap.Initialize(device, SceneSRVOffset + sceneHeader.textureCount,
                                             D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                             D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
        BLK_ASSERT_VAR(res);

        m_SceneTextures.resize(sceneHeader.textureCount);

        sceneData.PrepareTextureHeaders();

        size_t uploadSize = static_cast<size_t>(sceneHeader.vertex1Size) + sceneHeader.vertex2Size +
                            sceneHeader.vertexIndirectionSize + sceneHeader.indexSize +
                            sceneHeader.meshletsSize + sceneHeader.objectsSize;
        std::vector<size_t> textureOffsets;
        textureOffsets.reserve(sceneHeader.textureCount);

        size_t lastOffset = 0;

        // Skybox texture
        {
            UINT skyBoxResolution = sceneHeader.skyBoxResolution;
            UINT skyBoxMipCount = sceneHeader.skyBoxMipCount;

            size_t alignment;
            size_t size;
            Texture2D::GetRequiredSize(alignment, size, device, skyBoxResolution, skyBoxResolution,
                                       skyBoxMipCount, ms_SkyBoxTextureFormat,
                                       D3D12_RESOURCE_FLAG_NONE, BLK_TEXCUBE_FACE_COUNT);

            lastOffset += size;

            uploadSize += Texture2D::GetUploadSize(
                skyBoxResolution, skyBoxResolution, skyBoxMipCount, ms_SkyBoxTextureFormat,
                D3D12_RESOURCE_FLAG_NONE, BLK_TEXCUBE_FACE_COUNT);
        }

        // Scene textures
        for (UINT i = 0; i < sceneHeader.textureCount; ++i)
        {
            const auto& textureHeader = dataWrapper.textureHeaders[i];
            UINT width = textureHeader.width;
            UINT height = textureHeader.height;
            UINT mipCount = textureHeader.mipCount;
            BLK_ASSERT(width != 0);
            BLK_ASSERT(height != 0);

            size_t alignment;
            size_t size;
            Texture2D::GetRequiredSize(alignment, size, device, textureHeader.width,
                                       textureHeader.height, mipCount, ms_SceneTexturesFormat,
                                       D3D12_RESOURCE_FLAG_NONE);

            lastOffset = BLK_CEIL_TO_POWER_OF_TWO(lastOffset, alignment);
            textureOffsets.push_back(lastOffset);
            lastOffset += size;

            uploadSize += Texture2D::GetUploadSize(width, height, mipCount, ms_SceneTexturesFormat,
                                                   D3D12_RESOURCE_FLAG_NONE);
        }
        size_t heapSize = lastOffset;

        m_ResourceHeap.Initialize(device, heapSize, D3D12_HEAP_TYPE_DEFAULT,
                                  D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES);

        // Skybox texture
        {
            UINT skyBoxResolution = sceneHeader.skyBoxResolution;
            UINT skyBoxMipCount = sceneHeader.skyBoxMipCount;
            m_SkyBoxCubemap.Initialize(device, m_ResourceHeap, 0, skyBoxResolution,
                                       skyBoxResolution, skyBoxMipCount, ms_SkyBoxTextureFormat,
                                       D3D12_RESOURCE_FLAG_NONE, nullptr,
                                       D3D12_RESOURCE_STATE_COMMON, BLK_TEXCUBE_FACE_COUNT);

            BLK_RENDER_DEBUG_ONLY(m_SkyBoxCubemap.SetDebugName(L"Scene::m_SkyBoxCubemap"));
        }

        // Scene textures
        for (UINT i = 0; i < sceneHeader.textureCount; ++i)
        {
            auto& texture = m_SceneTextures[i];
            const auto& textureHeader = dataWrapper.textureHeaders[i];

            texture.Initialize(device, m_ResourceHeap, textureOffsets[i], textureHeader.width,
                               textureHeader.height, textureHeader.mipCount, ms_SceneTexturesFormat,
                               D3D12_RESOURCE_FLAG_NONE, nullptr, D3D12_RESOURCE_STATE_COMMON);
            BLK_RENDER_DEBUG_ONLY(texture.SetDebugName(L"Scene::m_Textures[%d]", i));
        }

        UINT slot = MeshletSRVOffset;

        ShaderResourceView::CreateSRV(
            device, m_VertexBuffer1, sceneHeader.vertex1Size / sizeof(SceneData::VertexData1),
            sizeof(SceneData::VertexData1), m_SRVDescriptorHeap.GetCPUHandle(slot++));

        ShaderResourceView::CreateSRV(
            device, m_VertexBuffer2, sceneHeader.vertex1Size / sizeof(SceneData::VertexData1),
            sizeof(SceneData::VertexData1), m_SRVDescriptorHeap.GetCPUHandle(slot++));

        ShaderResourceView::CreateSRV(device, m_VertexIndirectionBuffer,
                                      sceneHeader.vertexIndirectionSize / sizeof(uint32_t),
                                      sizeof(uint32_t), m_SRVDescriptorHeap.GetCPUHandle(slot++));

        ShaderResourceView::CreateSRV(device, m_IndexBuffer,
                                      sceneHeader.indexSize / sizeof(uint32_t), sizeof(uint32_t),
                                      m_SRVDescriptorHeap.GetCPUHandle(slot++));

        ShaderResourceView::CreateSRV(
            device, m_MeshletBuffer, sceneHeader.meshletsSize / sizeof(SceneData::MeshletData),
            sizeof(SceneData::MeshletData), m_SRVDescriptorHeap.GetCPUHandle(slot++));

        ShaderResourceView::CreateSRV(
            device, m_ObjectBuffer, sceneHeader.objectsSize / sizeof(SceneData::ObjectHeader),
            sizeof(SceneData::ObjectHeader), m_SRVDescriptorHeap.GetCPUHandle(slot++));

        BLK_ASSERT(slot == SkyBoxSRVOffset);

        ShaderResourceView::CreateSRVCube(device, m_SkyBoxCubemap,
                                          m_SRVDescriptorHeap.GetCPUHandle(slot++),
                                          ms_SkyBoxTextureFormat);

        BLK_ASSERT(slot == SceneSRVOffset);

        for (UINT i = 0; i < sceneHeader.textureCount; ++i)
        {
            ShaderResourceView::CreateSRV(device, m_SceneTextures[i],
                                          m_SRVDescriptorHeap.GetCPUHandle(SceneSRVOffset + i));
            BLK_ASSERT_VAR(res);
        }

        UploadBuffer uploadBuffer;
        uploadBuffer.Initialize(device, std::max(size_t(64), uploadSize));

        DebugProfileTimer streamingWait;
        streamingWait.Start();
        sceneData.PrepareBinaryData();
        streamingWait.Stop(L"Streaming wait");

        uploadBuffer.Upload(dataWrapper.binaryData, uploadSize);

        auto& initCommandList = engineContext.GetInitializationCommandList();

        UINT64 uploadBufferOffset = 0;

        initCommandList->CopyBufferRegion(m_VertexBuffer1.Get(), 0, uploadBuffer.Get(),
                                          uploadBufferOffset, sceneHeader.vertex1Size);
        uploadBufferOffset += sceneHeader.vertex1Size;

        initCommandList->CopyBufferRegion(m_VertexBuffer2.Get(), 0, uploadBuffer.Get(),
                                          uploadBufferOffset, sceneHeader.vertex2Size);
        uploadBufferOffset += sceneHeader.vertex2Size;

        initCommandList->CopyBufferRegion(m_VertexIndirectionBuffer.Get(), 0, uploadBuffer.Get(),
                                          uploadBufferOffset, sceneHeader.vertexIndirectionSize);
        uploadBufferOffset += sceneHeader.vertexIndirectionSize;

        initCommandList->CopyBufferRegion(m_IndexBuffer.Get(), 0, uploadBuffer.Get(),
                                          uploadBufferOffset, sceneHeader.indexSize);
        uploadBufferOffset += sceneHeader.indexSize;

        initCommandList->CopyBufferRegion(m_MeshletBuffer.Get(), 0, uploadBuffer.Get(),
                                          uploadBufferOffset, sceneHeader.meshletsSize);
        uploadBufferOffset += sceneHeader.meshletsSize;

        initCommandList->CopyBufferRegion(m_ObjectBuffer.Get(), 0, uploadBuffer.Get(),
                                          uploadBufferOffset, sceneHeader.objectsSize);
        uploadBufferOffset += sceneHeader.objectsSize;

        {
            UINT skyBoxResolution = sceneHeader.skyBoxResolution;
            UINT skyBoxMipCount = sceneHeader.skyBoxMipCount;
            UINT bytesPerPixel = Texture2D::GetBPP(ms_SkyBoxTextureFormat) / 8;
            BLK_ASSERT(bytesPerPixel != 0);

            BLK_ASSERT(bytesPerPixel == sizeof(Vector4));

            UINT16 subresource = 0;

            for (UINT face = 0; face < BLK_TEXCUBE_FACE_COUNT; ++face)
            {
                UINT resolution = skyBoxResolution;
                for (UINT16 mipNumber = 0; mipNumber < skyBoxMipCount; ++mipNumber)
                {
                    BLK_ASSERT(resolution != 0);

                    size_t rowPitch = BLK_CEIL_TO_POWER_OF_TWO(resolution * bytesPerPixel,
                                                               D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
                    size_t textureSize = BLK_CEIL_TO_POWER_OF_TWO(
                        rowPitch * resolution, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

                    D3D12_TEXTURE_COPY_LOCATION copyDest;
                    copyDest.pResource = m_SkyBoxCubemap.Get();
                    copyDest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    copyDest.SubresourceIndex = face * skyBoxMipCount + mipNumber;

                    D3D12_TEXTURE_COPY_LOCATION copySource;
                    copySource.pResource = uploadBuffer.Get();
                    copySource.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    copySource.PlacedFootprint.Offset = uploadBufferOffset;
                    copySource.PlacedFootprint.Footprint.Format = ms_SkyBoxTextureFormat;
                    copySource.PlacedFootprint.Footprint.Width = resolution;
                    copySource.PlacedFootprint.Footprint.Height = resolution;
                    copySource.PlacedFootprint.Footprint.Depth = 1;
                    copySource.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);

                    initCommandList->CopyTextureRegion(&copyDest, 0, 0, 0, &copySource, nullptr);

                    resolution >>= 1;
                    uploadBufferOffset += textureSize;
                }
            }
        }

        {
            UINT bytesPerPixel = Texture2D::GetBPP(ms_SceneTexturesFormat) / 8;
            BLK_ASSERT(bytesPerPixel != 0);

            for (UINT i = 0; i < sceneHeader.textureCount; ++i)
            {
                auto& texture = m_SceneTextures[i];
                auto& textureHeader = dataWrapper.textureHeaders[i];

                UINT width = textureHeader.width;
                UINT height = textureHeader.height;
                UINT16 mipCount = textureHeader.mipCount;
                for (UINT16 mipNumber = 0; mipNumber < mipCount; ++mipNumber)
                {
                    BLK_ASSERT(width != 0);
                    BLK_ASSERT(height != 0);

                    size_t rowPitch = BLK_CEIL_TO_POWER_OF_TWO(width * bytesPerPixel,
                                                               D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
                    size_t textureSize = BLK_CEIL_TO_POWER_OF_TWO(
                        rowPitch * height, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

                    D3D12_TEXTURE_COPY_LOCATION copyDest;
                    copyDest.pResource = texture.Get();
                    copyDest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    copyDest.SubresourceIndex = mipNumber;

                    D3D12_TEXTURE_COPY_LOCATION copySource;
                    copySource.pResource = uploadBuffer.Get();
                    copySource.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    copySource.PlacedFootprint.Offset = uploadBufferOffset;
                    copySource.PlacedFootprint.Footprint.Format = ms_SceneTexturesFormat;
                    copySource.PlacedFootprint.Footprint.Width = width;
                    copySource.PlacedFootprint.Footprint.Height = height;
                    copySource.PlacedFootprint.Footprint.Depth = 1;
                    copySource.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);

                    initCommandList->CopyTextureRegion(&copyDest, 0, 0, 0, &copySource, nullptr);

                    width >>= 1;
                    height >>= 1;
                    uploadBufferOffset += textureSize;
                }
            }
        }

        BLK_ASSERT(uploadBufferOffset == uploadSize);

        engineContext.FinishInitializationCommandList(device);

        uploadBuffer.Unload();

        m_ObjectCount = sceneHeader.objectCount;
        m_OpaqueObjectCount = sceneHeader.opaqueCount;

        m_BatchManager.Initialize(*this);

        return true;
    }

    void Scene::Unload()
    {
        BLK_UNLOAD_ARRAY(m_SceneTextures);
        m_SceneTextures.clear();

        m_SkyBoxCubemap.Unload();

        m_ResourceHeap.Unload();

        m_SRVDescriptorHeap.Unload();

        m_VertexBuffer1.Unload();
        m_VertexBuffer2.Unload();
        m_VertexIndirectionBuffer.Unload();
        m_IndexBuffer.Unload();
        m_MeshletBuffer.Unload();
        m_ObjectBuffer.Unload();

        m_ObjectCount = 0;
        m_OpaqueObjectCount = 0;
        m_BatchManager.Unload();
    }

    UINT Scene::GetObjectCount() const
    {
        return m_ObjectCount;
    }

    UINT Scene::GetOpaqueObjectCount() const
    {
        return m_OpaqueObjectCount;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE Scene::GetMeshletsTable() const
    {
        return m_SRVDescriptorHeap.GetGPUHandle(MeshletSRVOffset);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE Scene::GetSkyBoxTable() const
    {
        return m_SRVDescriptorHeap.GetGPUHandle(SkyBoxSRVOffset);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE Scene::GetSceneTexturesTable() const
    {
        return m_SRVDescriptorHeap.GetGPUHandle(SceneSRVOffset);
    }

    DescriptorHeap& Scene::GetSRVDescriptorHeap()
    {
        return m_SRVDescriptorHeap;
    }

    BatchManager& Scene::GetBatchManager()
    {
        return m_BatchManager;
    }

    void Scene::BindResources(CommandList& commandList)
    {
        ID3D12DescriptorHeap* descriptorHeaps[] = {GetSRVDescriptorHeap().Get()};
        commandList->SetDescriptorHeaps(ARRAYSIZE(descriptorHeaps), descriptorHeaps);
        commandList->SetGraphicsRootDescriptorTable(
            static_cast<UINT>(ResourceContainer::DefaultRootSigBindPoints::SceneSRV),
            GetSceneTexturesTable());
        commandList->SetGraphicsRootDescriptorTable(
            static_cast<UINT>(ResourceContainer::DefaultRootSigBindPoints::SceneMeshletsSRV),
            GetMeshletsTable());
    }

} // namespace Boolka
