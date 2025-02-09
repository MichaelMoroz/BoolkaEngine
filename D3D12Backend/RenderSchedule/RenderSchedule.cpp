#include "stdafx.h"

#include "RenderSchedule.h"

#include "APIWrappers/Device.h"
#include "Contexts/RenderContext.h"

namespace Boolka
{

    bool RenderSchedule::Initialize(Device& device, const wchar_t* folderPath,
                                    DisplayController& displayController)
    {
        BLK_CPU_SCOPE("RenderSchedule::Initialize");

        m_ResourceTracker.Initialize(device, 20);

        bool res = m_EngineContext.Initialize(device, displayController, m_ResourceTracker);
        BLK_ASSERT_VAR(res);
        res = m_FrameContext.Initialize(device);
        BLK_ASSERT_VAR(res);
        res = m_ThreadContext.Initialize(device);
        BLK_ASSERT_VAR(res);

        m_RenderContext.Initialize(m_EngineContext, m_FrameContext, m_ThreadContext);

        m_EngineContext.StartSceneLoading(device, folderPath);
        m_EngineContext.BuildPSOs(device);

        res = InitializeRenderPasses(device);
        BLK_ASSERT_VAR(res);

        m_EngineContext.FinishSceneLoading(device, folderPath);
        m_EngineContext.FlushInitializationCommandList(device);
        device.GetDStorageQueue().SyncGPU(device.GetGraphicQueue());
        m_EngineContext.FinishInitialization();

        return true;
    }

    void RenderSchedule::Unload()
    {
        m_RenderContext.Unload();
        m_EngineContext.Unload();
        m_FrameContext.Unload();
        m_ThreadContext.Unload();

        UnloadRenderPasses();

        m_ResourceTracker.Unload();
    }

    bool RenderSchedule::Render(Device& device, UINT frameIndex)
    {
        m_FrameContext.FlipFrame(m_EngineContext, frameIndex);
        m_ThreadContext.FlipFrame(frameIndex);

        device.CheckIsDeviceAlive();

        PrepareFrame();
        bool res = RenderFrame(device);
        BLK_ASSERT_VAR(res);

        return true;
    }

    ResourceTracker& RenderSchedule::GetResourceTracker()
    {
        return m_ResourceTracker;
    }

    bool RenderSchedule::InitializeRenderPasses(Device& device)
    {
        BLK_CPU_SCOPE("RenderSchedule::InitializeRenderPasses");

        bool res = m_UpdatePass.Initialize(device, m_RenderContext);
        BLK_ASSERT_VAR(res);
        res = m_GPUCullingPass.Initialize(device, m_RenderContext);
        BLK_ASSERT_VAR(res);
        res = m_ZPass.Initialize(device, m_RenderContext);
        BLK_ASSERT_VAR(res);
        res = m_ShadowMapPass.Initialize(device, m_RenderContext);
        BLK_ASSERT_VAR(res);
        res = m_GbufferPass.Initialize(device, m_RenderContext);
        BLK_ASSERT_VAR(res);
        res = m_RaytraceRenderPass.Initialize(device, m_RenderContext);
        BLK_ASSERT_VAR(res);
        res = m_DeferredLightingPass.Initialize(device, m_RenderContext);
        BLK_ASSERT_VAR(res);
        res = m_SkyBoxPass.Initialize(device, m_RenderContext);
        BLK_ASSERT_VAR(res);
        res = m_TransparentPass.Initialize(device, m_RenderContext);
        BLK_ASSERT_VAR(res);
        res = m_ToneMappingPass.Initialize(device, m_RenderContext);
        BLK_ASSERT_VAR(res);
        res = m_PresentPass.Initialize(device, m_RenderContext);
        BLK_ASSERT_VAR(res);
#ifdef BLK_ENABLE_STATS
        res = m_DebugOverlayPass.Initialize(device, m_RenderContext);
        BLK_ASSERT_VAR(res);
#endif

        return true;
    }

    void RenderSchedule::UnloadRenderPasses()
    {
        m_UpdatePass.Unload();
        m_GPUCullingPass.Unload();
        m_ZPass.Unload();
        m_ShadowMapPass.Unload();
        m_GbufferPass.Unload();
        m_RaytraceRenderPass.Unload();
        m_DeferredLightingPass.Unload();
        m_SkyBoxPass.Unload();
        m_TransparentPass.Unload();
        m_ToneMappingPass.Unload();
        m_PresentPass.Unload();
#ifdef BLK_ENABLE_STATS
        m_DebugOverlayPass.Unload();
#endif
    }

    bool RenderSchedule::PrepareFrame()
    {
        Scene& scene = m_RenderContext.GetRenderEngineContext().GetScene();
        scene.GetBatchManager().PrepareBatches(m_FrameContext, scene);

        return true;
    }

    bool RenderSchedule::RenderFrame(Device& device)
    {
        GraphicCommandListImpl& currentCommandList = m_ThreadContext.GetGraphicCommandList();

        PrepareCommandList(currentCommandList);

        bool res = m_UpdatePass.Render(m_RenderContext, m_ResourceTracker);
        BLK_ASSERT_VAR(res);
        res = m_GPUCullingPass.Render(m_RenderContext, m_ResourceTracker);
        BLK_ASSERT_VAR(res);
        res = m_ZPass.Render(m_RenderContext, m_ResourceTracker);
        BLK_ASSERT_VAR(res);
        res = m_ShadowMapPass.Render(m_RenderContext, m_ResourceTracker);
        BLK_ASSERT_VAR(res);
        res = m_GbufferPass.Render(m_RenderContext, m_ResourceTracker);
        BLK_ASSERT_VAR(res);
        res = m_RaytraceRenderPass.Render(m_RenderContext, m_ResourceTracker);
        BLK_ASSERT_VAR(res);
        res = m_DeferredLightingPass.Render(m_RenderContext, m_ResourceTracker);
        BLK_ASSERT_VAR(res);
        res = m_SkyBoxPass.Render(m_RenderContext, m_ResourceTracker);
        BLK_ASSERT_VAR(res);
        res = m_TransparentPass.Render(m_RenderContext, m_ResourceTracker);
        BLK_ASSERT_VAR(res);
        res = m_ToneMappingPass.Render(m_RenderContext, m_ResourceTracker);
        BLK_ASSERT_VAR(res);
#ifdef BLK_ENABLE_STATS
        res = m_DebugOverlayPass.Render(m_RenderContext, m_ResourceTracker);
        BLK_ASSERT_VAR(res);
#endif
        res = m_PresentPass.Render(m_RenderContext, m_ResourceTracker);
        BLK_ASSERT_VAR(res);

        FinishCommandList(device, currentCommandList);

        return true;
    }

    void RenderSchedule::PrepareCommandList(GraphicCommandListImpl& commandList)
    {
        commandList->SetGraphicsRootSignature(
            m_EngineContext.GetResourceContainer()
                .GetRootSignature(ResourceContainer::RootSig::Default)
                .Get());
    }

    void RenderSchedule::FinishCommandList(Device& device, GraphicCommandListImpl& commandList)
    {
        commandList->Close();
        device.GetGraphicQueue().ExecuteCommandList(commandList);
    }

} // namespace Boolka
