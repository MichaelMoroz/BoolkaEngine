#include "stdafx.h"

#include "Device.h"

#include "BoolkaCommon/DebugHelpers/DebugProfileTimer.h"

// D3D12 Agility SDK parameters
extern "C"
{
    __declspec(dllexport) extern const UINT D3D12SDKVersion;
    __declspec(dllexport) extern const char* D3D12SDKPath;
    const UINT D3D12SDKVersion = 4;
    const char* D3D12SDKPath = ".\\D3D12\\";
}

namespace Boolka
{

    Device::Device()
        : m_Device(nullptr)
    {
    }

    Device::~Device()
    {
        BLK_ASSERT(m_Device == nullptr);
    }

    ID3D12Device6* Device::Get()
    {
        BLK_ASSERT(m_Device != nullptr);
        return m_Device;
    }

    ID3D12Device6* Device::operator->()
    {
        return Get();
    }

    GraphicQueue& Device::GetGraphicQueue()
    {
        return m_GraphicQueue;
    }

    ComputeQueue& Device::GetComputeQueue()
    {
        return m_ComputeQueue;
    }

    CopyQueue& Device::GetCopyQueue()
    {
        return m_CopyQueue;
    }

    DStorageQueue& Device::GetDStorageQueue()
    {
        return m_DStorageQueue;
    }

    DStorageFactory& Device::GetDStorageFactory()
    {
        return m_DStorageFactory;
    }

    bool Device::Initialize(Factory& factory)
    {
        BLK_ASSERT(m_Device == nullptr);

        BLK_CPU_SCOPE("Device::Initialize");

        DebugProfileTimer timer;
        timer.Start();
        HRESULT hr = ::D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_Device));
        timer.Stop(L"D3D12CreateDevice");

        if (FAILED(hr))
        {
            ::MessageBoxW(0, L"DirectX 12.0 Feature Level 12_0 required",
                          L"GPU and/or driver unsupported", MB_OK | MB_ICONERROR);
            BLK_CRITICAL_DEBUG_BREAK();
            return false;
        }

        BLK_RENDER_PROFILING_ONLY(InitializeProfiling());
        BLK_RENDER_DEBUG_ONLY(InitializeDebug());

        if (!m_FeatureSupportHelper.Initialize(*this))
            return false;
        if (!m_DStorageFactory.Initialize())
            return false;
        if (!m_GraphicQueue.Initialize(*this))
            return false;
        if (!m_ComputeQueue.Initialize(*this))
            return false;
        if (!m_CopyQueue.Initialize(*this))
            return false;
        if (!m_DStorageQueue.Initialize(*this))
            return false;

        return true;
    }

    void Device::Unload()
    {
        BLK_ASSERT(m_Device != nullptr);

        m_DStorageQueue.Unload();
        m_CopyQueue.Unload();
        m_ComputeQueue.Unload();
        m_GraphicQueue.Unload();
        m_DStorageFactory.Unload();
        m_FeatureSupportHelper.Unload();

        BLK_RENDER_DEBUG_ONLY(ReportObjectLeaks());

        m_Device->Release();
        m_Device = nullptr;
    }

    void Device::Flush()
    {
        Fence* fences[3] = {&m_GraphicQueue.GetFence(), &m_ComputeQueue.GetFence(),
                            &m_CopyQueue.GetFence()};

        UINT64 values[3] = {m_GraphicQueue.SignalGPU(), m_ComputeQueue.SignalGPU(),
                            m_CopyQueue.SignalGPU()};

        Fence::WaitCPUMultiple(3, fences, values);
    }

    void Device::CheckIsDeviceAlive()
    {
        HRESULT hr = m_Device->GetDeviceRemovedReason();
        if (FAILED(hr))
        {
            wchar_t errorMessage[1024] = L"";
            DWORD res = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, hr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                       errorMessage, ARRAYSIZE(errorMessage), nullptr);
            if (res == 0)
            {
                g_WDebugOutput << L"Encountered error while processing error: 0x" << std::hex << hr
                               << L" GetLastError() == 0x" << GetLastError() << std::dec
                               << std::endl;
                BLK_CRITICAL_ASSERT(0);
            }
            g_WDebugOutput << L"Device Removed. Error code: 0x" << std::hex << hr << std::dec
                           << L". " << errorMessage << std::endl;
            BLK_CRITICAL_ASSERT(0);
        }
    }

#ifdef BLK_RENDER_PROFILING
    void Device::InitializeProfiling()
    {
        m_Device->SetStablePowerState(TRUE);
    }
#endif

#ifdef BLK_RENDER_DEBUG
    void Device::InitializeDebug()
    {
        SetDebugBreakSeverity(D3D12_MESSAGE_SEVERITY_WARNING);
    }

    void Device::SetDebugBreakSeverity(D3D12_MESSAGE_SEVERITY severity)
    {
        ID3D12InfoQueue* debugInfoQueue = nullptr;
        HRESULT hr = m_Device->QueryInterface(IID_PPV_ARGS(&debugInfoQueue));
        if (FAILED(hr))
        {
            return;
        }

        for (int i = 0; i <= D3D12_MESSAGE_SEVERITY_MESSAGE; i++)
        {
            // lower enum value corresponds to higher severity
            bool needBreak = (i <= severity);
            debugInfoQueue->SetBreakOnSeverity(static_cast<D3D12_MESSAGE_SEVERITY>(i), needBreak);
        }
        debugInfoQueue->Release();
    }

    void Device::ReportObjectLeaks()
    {
        ID3D12DebugDevice2* debugDevice = nullptr;
        HRESULT hr = m_Device->QueryInterface(IID_PPV_ARGS(&debugDevice));
        if (FAILED(hr))
        {
            return;
        }

        FilterMessage(D3D12_MESSAGE_ID_LIVE_DEVICE);
        // debugDevice->ReportLiveDeviceObjects always report reference to
        // device, since debugDevice itself is reference to device
        debugDevice->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
        RemoveLastMessageFilter();
        ULONG refCount = debugDevice->Release();
        // Instead of relying on debug layer warnings we check ref count
        // manually After releasing debugDevice there should only be single
        // reference left that we'll release right after
        // Device::ReportObjectLeaks call
        BLK_ASSERT(refCount == 1);
    }

    void Device::FilterMessage(D3D12_MESSAGE_ID id)
    {
        FilterMessage(&id, 1);
    }

    void Device::FilterMessage(D3D12_MESSAGE_ID* idArray, UINT idCount)
    {
        ID3D12InfoQueue* debugInfoQueue = nullptr;
        HRESULT hr = m_Device->QueryInterface(IID_PPV_ARGS(&debugInfoQueue));
        if (FAILED(hr))
        {
            return;
        }

        D3D12_INFO_QUEUE_FILTER filter = {};
        filter.DenyList.NumIDs = idCount;
        filter.DenyList.pIDList = idArray;
        hr = debugInfoQueue->PushStorageFilter(&filter);
        BLK_ASSERT(SUCCEEDED(hr));

        debugInfoQueue->Release();
    }

    void Device::RemoveLastMessageFilter()
    {
        ID3D12InfoQueue* debugInfoQueue = nullptr;
        HRESULT hr = m_Device->QueryInterface(IID_PPV_ARGS(&debugInfoQueue));
        if (FAILED(hr))
        {
            return;
        }
        debugInfoQueue->PopStorageFilter();

        debugInfoQueue->Release();
    }

#endif

} // namespace Boolka
