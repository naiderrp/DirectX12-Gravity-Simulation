#pragma once

#include "stdafx.h"
#include "camera.hpp"
#include "StepTimer.h"

#include "logging.hpp"


#define InterlockedGetValue(object) InterlockedCompareExchange(object, 0, 0)

using namespace DirectX;

enum graphics_root_parameters : uint32_t
{
    graphics_CBV,
    graphics_SRVtable,
    graphics_parameters_count
};

enum compute_root_parameters : uint32_t
{
    compute_CBV,
    computeSRVtable,
    computeUAVtable,
    compute_parameters_count
};

enum descriptor_heap_index : uint32_t
{
    UAV_particle_buf_0,
    UAV_particle_buf_1,
    SRV_particle_buf_0,
    SRV_particle_buf_1,
    descriptor_count
};

struct particle_t {
    XMFLOAT4 position;
    XMFLOAT4 velocity;
};

struct vertex_data {
    XMFLOAT4 color;
};

struct geometry_data {
    XMFLOAT4X4 mvp;
    XMFLOAT4X4 inverse_view;

    // Constant buffers are 256-byte aligned in GPU memory.
    // Padding is added for convenience when computing the struct's size.
    float padding[32];
};

struct compute_data {
    uint32_t param[4];  // param[0] - particles amount, param[1] - dimx
    float    paramf[4]; // paramf[0] - time interval, paramf[1] - damping
    // 4 variables for alignment
};

// while ComPtr is used to manage the lifetime of resources on the CPU,
// it has no understanding of the lifetime of resources on the GPU. 
// Apps must account for the GPU lifetime of resources to avoid destroying 
// objects that may still be referenced by the GPU.
//
// An example of this can be found in the cleanup() method

using Microsoft::WRL::ComPtr;

class render_system {
    
    struct thread_t {
        render_system* pcontext;
        uint32_t       thread_index;
    };

public:
    render_system(uint32_t width, uint32_t height, const std::wstring& name) :
        width_(width),
        height_(height),
        window_title_(name),
        use_warp_device_(false),
        current_frame_(0),
        viewport_(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
        scissor_rect_(0, 0, static_cast<LONG>(width), static_cast<LONG>(height)),
        RTVdescriptor_size_(0),
        SRV_UAVdescriptor_size_(0),
        geometry_constant_buffer_data_(nullptr),
        render_context_fence_value_(0),
        terminating_(0),
        SRVindex_{},
        frame_fence_values_{}
    {
        WCHAR assetsPath[512];
        GetAssetsPath(assetsPath, _countof(assetsPath));
        assets_path_ = assetsPath;

        aspect_ratio_ = static_cast<float>(width) / static_cast<float>(height);


        for (int i = 0; i < thread_count_; ++i) {
            render_context_fence_values_[i] = 0;
            thread_fence_values_[i] = 0;
        }

        float sqRootNumAsyncContexts = sqrt(static_cast<float>(thread_count_));
        height_instances_ = static_cast<uint32_t>(ceil(sqRootNumAsyncContexts));
        width_instances_ = static_cast<uint32_t>(ceil(sqRootNumAsyncContexts));

        if (width_instances_ * (height_instances_ - 1) >= thread_count_)
            --height_instances_;

        ThrowIfFailed(DXGIDeclareAdapterRemovalSupport());
    }

    void init() {
        camera_.init({ 0.0f, 0.0f, 1500.0f });
        camera_.set_move_speed(250.0f);

        load_pipeline();
        load_assets();
        create_async_contexts();
    }

    // update frame-based values
    void update() {
        // wait for the previous Present to complete
        WaitForSingleObjectEx(swapchain_event_, 100, FALSE);

        timer_.Tick(NULL);
        camera_.update(static_cast<float>(timer_.GetElapsedSeconds()));

        geometry_data constantBufferGS = {};
        XMStoreFloat4x4(&constantBufferGS.mvp, XMMatrixMultiply(camera_.view_matrix(), camera_.projection_matrix(0.8f, aspect_ratio_, 1.0f, 5000.0f)));
        XMStoreFloat4x4(&constantBufferGS.inverse_view, XMMatrixInverse(nullptr, camera_.view_matrix()));

        uint8_t* destination = geometry_constant_buffer_data_ + sizeof(geometry_data) * current_frame_;
        memcpy(destination, &constantBufferGS, sizeof(geometry_data));

        update_FPS();
    }

    void update_FPS() {
        static float FPS = 0.0f;
        static int frame_count = 0;
        static float elapsed_time = 0.0f;

        elapsed_time += timer_.GetElapsedSeconds();

        ++frame_count;

        if (elapsed_time >= 1.0f) {
            FPS = static_cast<float>(frame_count) / elapsed_time;
            
            set_window_title(std::to_wstring(FPS) + L" FPS");
            
            frame_count = 0;
            elapsed_time = 0.0f;
        }
    }

    void render() {
        try {
            // let the compute thread know that a new frame is being rendered.
            for (int n = 0; n < thread_count_; ++n)
                InterlockedExchange(&render_context_fence_values_[n], render_context_fence_value_);
            
            // compute work must be completed before the frame can render or else the SRV 
            // will be in the wrong state.
            for (UINT n = 0; n < thread_count_; ++n) {
                
                uint64_t threadFenceValue = InterlockedGetValue(&thread_fence_values_[n]);
                
                if (thread_fences_[n]->GetCompletedValue() < threadFenceValue) {
                    // instruct the rendering command queue to wait for the current 
                    // compute work to complete.
                    ThrowIfFailed(command_queue_->Wait(thread_fences_[n].Get(), threadFenceValue));
                }
            }

            record_command_list();

            ID3D12CommandList* ppCommandLists[] = { command_list_.Get() };
            command_queue_->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

            ThrowIfFailed(swapchain_->Present(1, 0));

            acquire_next_frame();
        }

        catch (HrException& e) {
            if (e.Error() == DXGI_ERROR_DEVICE_REMOVED || e.Error() == DXGI_ERROR_DEVICE_RESET)
                restore_resources();
            else throw e;
        }
    }

    void cleanup() {
        // notify the compute threads that the app_ is shutting down
        InterlockedExchange(&terminating_, 1);
        WaitForMultipleObjects(thread_count_, thread_handle_, TRUE, INFINITE);

        // ensure that the GPU is no longer referencing resources that are about to be
        // cleaned up by the destructor
        queue_wait_idle();

        CloseHandle(render_context_fence_event_);
        for (int n = 0; n < thread_count_; ++n) {
            CloseHandle(thread_handle_[n]);
            CloseHandle(thread_fence_events_[n]);
        }
    }
    
    void key_down(UINT8 key) {
        camera_.on_keydown(key);
    }

    void key_up(UINT8 key) {
        camera_.on_keyup(key);
    }

    uint32_t width() const { 
        return width_; 
    }

    uint32_t height() const { 
        return height_; 
    }

    const WCHAR* title() const { 
        return window_title_.c_str(); 
    }

private:
    void load_pipeline() {
        uint32_t factory_flags = 0;

#if defined(_DEBUG)
        {
            ComPtr<ID3D12Debug> debugController;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
                debugController->EnableDebugLayer();
                factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
            }
        }
#endif

        ComPtr<IDXGIFactory4> factory;
        ThrowIfFailed(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory)), L"Failed to CreateDXGIFactory2");

        if (use_warp_device_) {
            ComPtr<IDXGIAdapter> warpAdapter;
            ThrowIfFailed(factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)));

            ThrowIfFailed(D3D12CreateDevice(
                warpAdapter.Get(),
                D3D_FEATURE_LEVEL_11_0,
                IID_PPV_ARGS(&device_)
            ));
        }
        else {
            ComPtr<IDXGIAdapter1> hardwareAdapter;
            get_adapter(factory.Get(), &hardwareAdapter, true);

            ThrowIfFailed(D3D12CreateDevice(
                hardwareAdapter.Get(),
                D3D_FEATURE_LEVEL_11_0,
                IID_PPV_ARGS(&device_)
            ));
        }

        D3D12_COMMAND_QUEUE_DESC queue_desc = {};
        queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

        ThrowIfFailed(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue_)), L"Failed to create command queue");
        NAME_D3D12_OBJECT(command_queue_);

        DXGI_SWAP_CHAIN_DESC1 swapchain_desc = {};
        swapchain_desc.BufferCount = max_frames_in_flight_;
        swapchain_desc.Width = width_;
        swapchain_desc.Height = height_;
        swapchain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapchain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapchain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapchain_desc.SampleDesc.Count = 1;
        swapchain_desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

        ComPtr<IDXGISwapChain1> swapchain;
        ThrowIfFailed(factory->CreateSwapChainForCoreWindow(
            command_queue_.Get(),        // Swap chain needs the queue so that it can force a flush on it.
            reinterpret_cast<IUnknown*>(Windows::UI::Core::CoreWindow::GetForCurrentThread()),
            &swapchain_desc,
            nullptr,
            &swapchain
        ), L"Failed to create swapchain for core window");

        ThrowIfFailed(swapchain.As(&swapchain_));

        current_frame_ = swapchain_->GetCurrentBackBufferIndex();
        swapchain_event_ = swapchain_->GetFrameLatencyWaitableObject();

        // descriptor heaps
        {
            // a render target view (RTV) descriptor heap
            D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
            rtvHeapDesc.NumDescriptors = max_frames_in_flight_;
            rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            ThrowIfFailed(device_->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&RTVheap_)));

            // a shader resource view (SRV) and unordered access view (UAV) descriptor heap
            D3D12_DESCRIPTOR_HEAP_DESC srvUavHeapDesc = {};
            srvUavHeapDesc.NumDescriptors = descriptor_count;
            srvUavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            srvUavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            ThrowIfFailed(device_->CreateDescriptorHeap(&srvUavHeapDesc, IID_PPV_ARGS(&SRV_UAVheap_)));
            NAME_D3D12_OBJECT(SRV_UAVheap_);

            RTVdescriptor_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            SRV_UAVdescriptor_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        // frame resources
        {
            CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(RTVheap_->GetCPUDescriptorHandleForHeapStart());

            // a RTV and a command allocator for each frame
            for (UINT n = 0; n < max_frames_in_flight_; ++n) {
                ThrowIfFailed(swapchain_->GetBuffer(n, IID_PPV_ARGS(&rendertargets_[n])));
                device_->CreateRenderTargetView(rendertargets_[n].Get(), nullptr, rtvHandle);
                rtvHandle.Offset(1, RTVdescriptor_size_);

                NAME_D3D12_OBJECT_INDEXED(rendertargets_, n);

                ThrowIfFailed(
                    device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocators_[n])),
                    L"Failed to create command allocator"
                );
            }
        }
    }

    void load_assets() {

        // the root signatures
        {
            D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
            featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

            if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
                featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;

            // graphics root signature
            {
                CD3DX12_DESCRIPTOR_RANGE1 ranges[1];
                ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);

                CD3DX12_ROOT_PARAMETER1 rootParameters[graphics_parameters_count];
                rootParameters[graphics_CBV].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC, D3D12_SHADER_VISIBILITY_ALL);
                rootParameters[graphics_SRVtable].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_VERTEX);

                CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
                rootSignatureDesc.Init_1_1(_countof(rootParameters), rootParameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

                ComPtr<ID3DBlob> signature;
                ComPtr<ID3DBlob> error;
                ThrowIfFailed(D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, featureData.HighestVersion, &signature, &error));
                ThrowIfFailed(device_->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&root_signature_)));
                NAME_D3D12_OBJECT(root_signature_);
            }

            // compute root signature
            {
                CD3DX12_DESCRIPTOR_RANGE1 ranges[2];
                ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
                ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);

                CD3DX12_ROOT_PARAMETER1 rootParameters[compute_parameters_count];
                rootParameters[compute_CBV].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC, D3D12_SHADER_VISIBILITY_ALL);
                rootParameters[computeSRVtable].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_ALL);
                rootParameters[computeUAVtable].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_ALL);

                CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC computeRootSignatureDesc;
                computeRootSignatureDesc.Init_1_1(_countof(rootParameters), rootParameters, 0, nullptr);

                ComPtr<ID3DBlob> signature;
                ComPtr<ID3DBlob> error;
                ThrowIfFailed(D3DX12SerializeVersionedRootSignature(&computeRootSignatureDesc, featureData.HighestVersion, &signature, &error));
                ThrowIfFailed(device_->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&compute_root_signature_)));
                NAME_D3D12_OBJECT(compute_root_signature_);
            }
        }

        // the pipeline states, which includes compiling and loading shaders
        {
            ComPtr<ID3DBlob> vertexShader;
            ComPtr<ID3DBlob> geometryShader;
            ComPtr<ID3DBlob> pixelShader;
            ComPtr<ID3DBlob> computeShader;

#if defined(_DEBUG)
            UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
            UINT compileFlags = 0;
#endif
            ThrowIfFailed(D3DCompileFromFile(asset_full_path(L"VertexGeometryPixelShader.hlsl").c_str(), nullptr, nullptr, "VS_main", "vs_5_0", compileFlags, 0, &vertexShader, nullptr));
            ThrowIfFailed(D3DCompileFromFile(asset_full_path(L"VertexGeometryPixelShader.hlsl").c_str(), nullptr, nullptr, "GS_main", "gs_5_0", compileFlags, 0, &geometryShader, nullptr));
            ThrowIfFailed(D3DCompileFromFile(asset_full_path(L"VertexGeometryPixelShader.hlsl").c_str(), nullptr, nullptr, "PS_main", "ps_5_0", compileFlags, 0, &pixelShader, nullptr));
            ThrowIfFailed(D3DCompileFromFile(asset_full_path(L"ComputeShader.hlsl").c_str(), nullptr, nullptr, "main", "cs_5_0", compileFlags, 0, &computeShader, nullptr));

            D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
                { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            };

            // Describe the blend and depth states.
            CD3DX12_BLEND_DESC blendDesc(D3D12_DEFAULT);
            blendDesc.RenderTarget[0].BlendEnable = TRUE;
            blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
            blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
            blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ZERO;
            blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;

            CD3DX12_DEPTH_STENCIL_DESC depthStencilDesc(D3D12_DEFAULT);
            depthStencilDesc.DepthEnable = FALSE;
            depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

            // the graphics PSO
            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
            psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
            psoDesc.pRootSignature = root_signature_.Get();
            psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.Get());
            psoDesc.GS = CD3DX12_SHADER_BYTECODE(geometryShader.Get());
            psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.Get());
            psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            psoDesc.BlendState = blendDesc;
            psoDesc.DepthStencilState = depthStencilDesc;
            psoDesc.SampleMask = UINT_MAX;
            psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            psoDesc.NumRenderTargets = 1;
            psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
            psoDesc.SampleDesc.Count = 1;

            ThrowIfFailed(device_->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipeline_state_)));
            NAME_D3D12_OBJECT(pipeline_state_);

            // the compute PSO
            D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
            computePsoDesc.pRootSignature = compute_root_signature_.Get();
            computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(computeShader.Get());

            ThrowIfFailed(device_->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&compute_state_)));
            NAME_D3D12_OBJECT(compute_state_);
        }

        // the command list
        ThrowIfFailed(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocators_[current_frame_].Get(), pipeline_state_.Get(), IID_PPV_ARGS(&command_list_)));
        NAME_D3D12_OBJECT(command_list_);

        create_vertex_buffer();
        create_particles_buffer();

        ComPtr<ID3D12Resource> constantBufferCSUpload;

        // the compute shader's constant buffer
        {
            const UINT bufferSize = sizeof(compute_data);

            ThrowIfFailed(device_->CreateCommittedResource(
                &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
                D3D12_HEAP_FLAG_NONE,
                &CD3DX12_RESOURCE_DESC::Buffer(bufferSize),
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&compute_constant_buffer_)));

            ThrowIfFailed(device_->CreateCommittedResource(
                &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
                D3D12_HEAP_FLAG_NONE,
                &CD3DX12_RESOURCE_DESC::Buffer(bufferSize),
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&constantBufferCSUpload)));

            NAME_D3D12_OBJECT(compute_constant_buffer_);

            compute_data constantBufferCS = {};
            constantBufferCS.param[0] = particle_count_;
            constantBufferCS.param[1] = int(ceil(particle_count_ / 128.0f));
            constantBufferCS.paramf[0] = 0.1f;
            constantBufferCS.paramf[1] = 1.0f;

            D3D12_SUBRESOURCE_DATA computeCBData = {};
            computeCBData.pData = reinterpret_cast<UINT8*>(&constantBufferCS);
            computeCBData.RowPitch = bufferSize;
            computeCBData.SlicePitch = computeCBData.RowPitch;

            UpdateSubresources<1>(command_list_.Get(), compute_constant_buffer_.Get(), constantBufferCSUpload.Get(), 0, 0, 1, &computeCBData);
            command_list_->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(compute_constant_buffer_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));
        }

        // the geometry shader's constant buffer
        {
            const UINT constantBufferGSSize = sizeof(geometry_data) * max_frames_in_flight_;

            ThrowIfFailed(device_->CreateCommittedResource(
                &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
                D3D12_HEAP_FLAG_NONE,
                &CD3DX12_RESOURCE_DESC::Buffer(constantBufferGSSize),
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&geometry_constant_buffer_)
            ));

            NAME_D3D12_OBJECT(geometry_constant_buffer_);

            CD3DX12_RANGE readRange(0, 0); // We do not intend to read from this resource on the CPU.
            ThrowIfFailed(geometry_constant_buffer_->Map(0, &readRange, reinterpret_cast<void**>(&geometry_constant_buffer_data_)));
            ZeroMemory(geometry_constant_buffer_data_, constantBufferGSSize);
        }

        ThrowIfFailed(command_list_->Close());
        ID3D12CommandList* ppCommandLists[] = { command_list_.Get() };
        command_queue_->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

        // wait until assets have been uploaded to the GPU
        {
            ThrowIfFailed(device_->CreateFence(render_context_fence_value_, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&render_context_fence_)));
            render_context_fence_value_++;

            render_context_fence_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (render_context_fence_event_ == nullptr)
            {
                ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
            }

            queue_wait_idle();
        }
    }

    void restore_resources() {
        // give GPU a chance to finish its execution in progress.
        try {
            wait_idle();
        }
        catch (HrException&) {
            // do nothing, currently attached adapter is unresponsive.
        }
        release_resources();
        init();
    }

    void release_resources() {
        render_context_fence_.Reset();

        ResetComPtrArray(&rendertargets_);

        command_queue_.Reset();

        swapchain_.Reset();

        device_.Reset();
    }

    // wait for pending GPU work to complete.
    void wait_idle() {
        // schedule a Signal command in the queue
        ThrowIfFailed(command_queue_->Signal(render_context_fence_.Get(), render_context_fence_values_[current_frame_]));

        // wait until the fence has been processed
        ThrowIfFailed(render_context_fence_->SetEventOnCompletion(render_context_fence_values_[current_frame_], render_context_fence_event_));
        WaitForSingleObjectEx(render_context_fence_event_, INFINITE, FALSE);

        // Increment the fence value for the current frame
        ++render_context_fence_values_[current_frame_];
    }

    void create_async_contexts() {
        for (uint32_t thread_index = 0; thread_index < thread_count_; ++thread_index) {
            // compute resources
            D3D12_COMMAND_QUEUE_DESC queueDesc = { D3D12_COMMAND_LIST_TYPE_COMPUTE, 0, D3D12_COMMAND_QUEUE_FLAG_NONE };
            ThrowIfFailed(device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&compute_command_queue_[thread_index])));
            ThrowIfFailed(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&compute_allocator_[thread_index])));
            ThrowIfFailed(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, compute_allocator_[thread_index].Get(), nullptr, IID_PPV_ARGS(&compute_command_list_[thread_index])));
            ThrowIfFailed(device_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&thread_fences_[thread_index])));

            thread_fence_events_[thread_index] = CreateEvent(nullptr, FALSE, FALSE, nullptr);

            if (thread_fence_events_[thread_index] == nullptr)
                ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));

            thread_data_[thread_index].pcontext = this;
            thread_data_[thread_index].thread_index = thread_index;

            thread_handle_[thread_index] = CreateThread(
                nullptr,
                0,
                reinterpret_cast<LPTHREAD_START_ROUTINE>(thread_proc),
                reinterpret_cast<void*>(&thread_data_[thread_index]),
                CREATE_SUSPENDED,
                nullptr);

            ResumeThread(thread_handle_[thread_index]);
        }
    }

    void create_vertex_buffer() {
        std::vector<vertex_data> vertices(particle_count_);

        for (int i = 0; i < particle_count_; ++i)
            vertices[i].color = XMFLOAT4(1.0f, 1.0f, 0.2f, 1.0f);

        const uint32_t bufferSize = particle_count_ * sizeof(vertex_data);

        ThrowIfFailed(device_->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(bufferSize),
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&vertex_buffer_))
        );

        ThrowIfFailed(device_->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(bufferSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&vertex_buffer_upload_))
        );

        NAME_D3D12_OBJECT(vertex_buffer_);

        D3D12_SUBRESOURCE_DATA vertexData = {};
        vertexData.pData = reinterpret_cast<UINT8*>(&vertices[0]);
        vertexData.RowPitch = bufferSize;
        vertexData.SlicePitch = vertexData.RowPitch;

        UpdateSubresources<1>(command_list_.Get(), vertex_buffer_.Get(), vertex_buffer_upload_.Get(), 0, 0, 1, &vertexData);
        command_list_->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(vertex_buffer_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));

        vertex_buffer_view_.BufferLocation = vertex_buffer_->GetGPUVirtualAddress();
        vertex_buffer_view_.SizeInBytes = static_cast<UINT>(bufferSize);
        vertex_buffer_view_.StrideInBytes = sizeof(vertex_data);
    }

    // from -1 to 1
    float random_percent() {
        float ret = static_cast<float>((rand() % 10000) - 5000);
        return ret / 5000.0f;
    }

    void init_particles(particle_t* pParticles, const XMFLOAT3& center, const XMFLOAT4& velocity, float spread, UINT numParticles) {
        srand(0);

        for (UINT i = 0; i < numParticles; ++i) {
            XMFLOAT3 delta(spread, spread, spread);

            while (XMVectorGetX(XMVector3LengthSq(XMLoadFloat3(&delta))) > spread * spread) {
                delta.x = random_percent() * spread;
                delta.y = random_percent() * spread;
                delta.z = random_percent() * spread;
            }

            pParticles[i].position.x = center.x + delta.x;
            pParticles[i].position.y = center.y + delta.y;
            pParticles[i].position.z = center.z + delta.z;
            pParticles[i].position.w = 10000.0f * 10000.0f;

            pParticles[i].velocity = velocity;
        }
    }

    void create_particles_buffer() {
        std::vector<particle_t> data(particle_count_);
        const uint32_t dataSize = particle_count_ * sizeof(particle_t);

        // split the particles into two groups
        float centerSpread = particle_spread_ * 0.50f;
        init_particles(&data[0], XMFLOAT3(centerSpread, 0, 0), XMFLOAT4(0, 0, -20, 1 / 100000000.0f), particle_spread_, particle_count_ / 2);
        init_particles(&data[particle_count_ / 2], XMFLOAT3(-centerSpread, 0, 0), XMFLOAT4(0, 0, 20, 1 / 100000000.0f), particle_spread_, particle_count_ / 2);

        D3D12_HEAP_PROPERTIES defaultHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_HEAP_PROPERTIES uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(dataSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        D3D12_RESOURCE_DESC uploadBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(dataSize);

        for (uint32_t index = 0; index < thread_count_; ++index) {
            // Create two buffers in the GPU, each with a copy of the particles data.
            // The compute shader will update one of them while the rendering thread 
            // renders the other. When rendering completes, the threads will swap 
            // which buffer they work on

            ThrowIfFailed(device_->CreateCommittedResource(
                &defaultHeapProperties,
                D3D12_HEAP_FLAG_NONE,
                &bufferDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&particle_buffer0_[index]))
            );

            ThrowIfFailed(device_->CreateCommittedResource(
                &defaultHeapProperties,
                D3D12_HEAP_FLAG_NONE,
                &bufferDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&particle_buffer1_[index]))
            );

            ThrowIfFailed(device_->CreateCommittedResource(
                &uploadHeapProperties,
                D3D12_HEAP_FLAG_NONE,
                &uploadBufferDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&particle_buffer0_upload_[index]))
            );

            ThrowIfFailed(device_->CreateCommittedResource(
                &uploadHeapProperties,
                D3D12_HEAP_FLAG_NONE,
                &uploadBufferDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&particle_buffer1_upload_[index]))
            );

            NAME_D3D12_OBJECT_INDEXED(particle_buffer0_, index);
            NAME_D3D12_OBJECT_INDEXED(particle_buffer1_, index);

            D3D12_SUBRESOURCE_DATA particleData = {};
            particleData.pData = reinterpret_cast<UINT8*>(&data[0]);
            particleData.RowPitch = dataSize;
            particleData.SlicePitch = particleData.RowPitch;

            UpdateSubresources<1>(command_list_.Get(), particle_buffer0_[index].Get(), particle_buffer0_upload_[index].Get(), 0, 0, 1, &particleData);
            UpdateSubresources<1>(command_list_.Get(), particle_buffer1_[index].Get(), particle_buffer1_upload_[index].Get(), 0, 0, 1, &particleData);
            command_list_->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(particle_buffer0_[index].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
            command_list_->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(particle_buffer1_[index].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = particle_count_;
            srvDesc.Buffer.StructureByteStride = sizeof(particle_t);
            srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

            CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle0(SRV_UAVheap_->GetCPUDescriptorHandleForHeapStart(), SRV_particle_buf_0 + index, SRV_UAVdescriptor_size_);
            CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle1(SRV_UAVheap_->GetCPUDescriptorHandleForHeapStart(), SRV_particle_buf_1 + index, SRV_UAVdescriptor_size_);
            device_->CreateShaderResourceView(particle_buffer0_[index].Get(), &srvDesc, srvHandle0);
            device_->CreateShaderResourceView(particle_buffer1_[index].Get(), &srvDesc, srvHandle1);

            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = DXGI_FORMAT_UNKNOWN;
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uavDesc.Buffer.FirstElement = 0;
            uavDesc.Buffer.NumElements = particle_count_;
            uavDesc.Buffer.StructureByteStride = sizeof(particle_t);
            uavDesc.Buffer.CounterOffsetInBytes = 0;
            uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

            CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandle0(SRV_UAVheap_->GetCPUDescriptorHandleForHeapStart(), UAV_particle_buf_0 + index, SRV_UAVdescriptor_size_);
            CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandle1(SRV_UAVheap_->GetCPUDescriptorHandleForHeapStart(), UAV_particle_buf_1 + index, SRV_UAVdescriptor_size_);
            device_->CreateUnorderedAccessView(particle_buffer0_[index].Get(), nullptr, &uavDesc, uavHandle0);
            device_->CreateUnorderedAccessView(particle_buffer1_[index].Get(), nullptr, &uavDesc, uavHandle1);
        }
    }

    void record_command_list() {
        // Command list allocators can only be reset when the associated
        // command lists have finished execution on the GPU; apps should use
        // fences to determine GPU execution progress.
        ThrowIfFailed(command_allocators_[current_frame_]->Reset());

        // However, when ExecuteCommandList() is called on a particular command
        // list, that command list can then be reset at any time and must be before
        // re-recording.
        ThrowIfFailed(command_list_->Reset(command_allocators_[current_frame_].Get(), pipeline_state_.Get()));

        command_list_->SetPipelineState(pipeline_state_.Get());
        command_list_->SetGraphicsRootSignature(root_signature_.Get());

        command_list_->SetGraphicsRootConstantBufferView(graphics_CBV, geometry_constant_buffer_->GetGPUVirtualAddress() + current_frame_ * sizeof(geometry_data));

        ID3D12DescriptorHeap* ppHeaps[] = { SRV_UAVheap_.Get() };
        command_list_->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

        command_list_->IASetVertexBuffers(0, 1, &vertex_buffer_view_);
        command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
        command_list_->RSSetScissorRects(1, &scissor_rect_);

        // the back buffer will be used as a render target
        command_list_->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(rendertargets_[current_frame_].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(RTVheap_->GetCPUDescriptorHandleForHeapStart(), current_frame_, RTVdescriptor_size_);
        command_list_->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        const float clearColor[] = { 0.0f, 0.0f, 0.1f, 0.0f };
        command_list_->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

        // Render the particles.
        float viewportHeight = static_cast<float>(viewport_.Height / height_instances_);
        float viewportWidth = static_cast<float>(viewport_.Width / width_instances_);
        for (uint32_t n = 0; n < thread_count_; ++n) {
            const uint32_t srvIndex = n + (SRVindex_[n] == 0 ? SRV_particle_buf_0 : SRV_particle_buf_1);

            CD3DX12_VIEWPORT viewport(
                (n % width_instances_) * viewportWidth,
                (n / width_instances_) * viewportHeight,
                viewportWidth,
                viewportHeight);

            command_list_->RSSetViewports(1, &viewport);

            CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(SRV_UAVheap_->GetGPUDescriptorHandleForHeapStart(), srvIndex, SRV_UAVdescriptor_size_);
            command_list_->SetGraphicsRootDescriptorTable(graphics_SRVtable, srvHandle);

            command_list_->DrawInstanced(particle_count_, 1, 0, 0);
        }

        command_list_->RSSetViewports(1, &viewport_);

        // the back buffer will now be used to present
        command_list_->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(rendertargets_[current_frame_].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

        ThrowIfFailed(command_list_->Close());
    }

    static DWORD WINAPI thread_proc(thread_t* pData) {
        return pData->pcontext->async_compute_thread_proc(pData->thread_index);
    }

    DWORD async_compute_thread_proc(int thread_index) {

        ID3D12CommandQueue* pCommandQueue = compute_command_queue_[thread_index].Get();
        ID3D12CommandAllocator* pCommandAllocator = compute_allocator_[thread_index].Get();
        ID3D12GraphicsCommandList* pCommandList = compute_command_list_[thread_index].Get();
        ID3D12Fence* pFence = thread_fences_[thread_index].Get();

        while (InterlockedGetValue(&terminating_) == 0) {
            run_simulation(thread_index);

            ThrowIfFailed(pCommandList->Close());
            ID3D12CommandList* ppCommandLists[] = { pCommandList };

            pCommandQueue->ExecuteCommandLists(1, ppCommandLists);

            // wait for the compute shader to complete the simulation
            uint64_t threadFenceValue = InterlockedIncrement(&thread_fence_values_[thread_index]);
            ThrowIfFailed(pCommandQueue->Signal(pFence, threadFenceValue));
            ThrowIfFailed(pFence->SetEventOnCompletion(threadFenceValue, thread_fence_events_[thread_index]));
            WaitForSingleObject(thread_fence_events_[thread_index], INFINITE);

            // wait for the render thread to be done with the SRV so that
            // the next frame in the simulation can run
            uint64_t renderContextFenceValue = InterlockedGetValue(&render_context_fence_values_[thread_index]);
            if (render_context_fence_->GetCompletedValue() < renderContextFenceValue) {
                ThrowIfFailed(pCommandQueue->Wait(render_context_fence_.Get(), renderContextFenceValue));
                InterlockedExchange(&render_context_fence_values_[thread_index], 0);
            }

            // swap the indices to the SRV and UAV
            SRVindex_[thread_index] = 1 - SRVindex_[thread_index];

            // prepare for the next frame
            ThrowIfFailed(pCommandAllocator->Reset());
            ThrowIfFailed(pCommandList->Reset(pCommandAllocator, compute_state_.Get()));
        }

        return 0;
    }

    void run_simulation(uint32_t thread_index) {
        ID3D12GraphicsCommandList* pCommandList = compute_command_list_[thread_index].Get();

        uint32_t srvIndex;
        uint32_t uavIndex;

        ID3D12Resource* pUavResource;

        if (SRVindex_[thread_index] == 0) {
            srvIndex = SRV_particle_buf_0;
            uavIndex = UAV_particle_buf_1;
            pUavResource = particle_buffer1_[thread_index].Get();
        }
        else {
            srvIndex = SRV_particle_buf_1;
            uavIndex = UAV_particle_buf_0;
            pUavResource = particle_buffer0_[thread_index].Get();
        }

        pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(pUavResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

        pCommandList->SetPipelineState(compute_state_.Get());
        pCommandList->SetComputeRootSignature(compute_root_signature_.Get());

        ID3D12DescriptorHeap* ppHeaps[] = { SRV_UAVheap_.Get() };
        pCommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

        CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(SRV_UAVheap_->GetGPUDescriptorHandleForHeapStart(), srvIndex + thread_index, SRV_UAVdescriptor_size_);
        CD3DX12_GPU_DESCRIPTOR_HANDLE uavHandle(SRV_UAVheap_->GetGPUDescriptorHandleForHeapStart(), uavIndex + thread_index, SRV_UAVdescriptor_size_);

        pCommandList->SetComputeRootConstantBufferView(compute_CBV, compute_constant_buffer_->GetGPUVirtualAddress());
        pCommandList->SetComputeRootDescriptorTable(computeSRVtable, srvHandle);
        pCommandList->SetComputeRootDescriptorTable(computeUAVtable, uavHandle);

        pCommandList->Dispatch(static_cast<int>(ceil(particle_count_ / 128.0f)), 1, 1);

        pCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(pUavResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    }

    // wait for render context
    void queue_wait_idle() {
        // add a signal command to the queue
        ThrowIfFailed(command_queue_->Signal(render_context_fence_.Get(), render_context_fence_value_));

        // instruct the fence to set the event object when the signal command completes
        ThrowIfFailed(render_context_fence_->SetEventOnCompletion(render_context_fence_value_, render_context_fence_event_));
        render_context_fence_value_++;

        // wait until the signal command has been processed
        WaitForSingleObject(render_context_fence_event_, INFINITE);
    }


    // Cycle through the frame resources. This method blocks execution if the 
    // next frame resource in the queue has not yet had its previous contents 
    // processed by the GPU.

    void acquire_next_frame() {
        // assign the current fence value to the current frame
        frame_fence_values_[current_frame_] = render_context_fence_value_;

        // signal and increment the fence value
        ThrowIfFailed(command_queue_->Signal(render_context_fence_.Get(), render_context_fence_value_));
        render_context_fence_value_++;

        // update the frame index
        current_frame_ = swapchain_->GetCurrentBackBufferIndex();

        // if the next frame is not ready to be rendered yet, wait until it is ready.
        if (render_context_fence_->GetCompletedValue() < frame_fence_values_[current_frame_]) {
            ThrowIfFailed(render_context_fence_->SetEventOnCompletion(frame_fence_values_[current_frame_], render_context_fence_event_));
            WaitForSingleObject(render_context_fence_event_, INFINITE);
        }
    }

    std::wstring asset_full_path(LPCWSTR asset_name) const {
        return assets_path_ + asset_name;
    }

    void get_adapter(
        IDXGIFactory1* pFactory,
        IDXGIAdapter1** ppAdapter,
        bool requestHighPerformanceAdapter = false) {

        *ppAdapter = nullptr;

        ComPtr<IDXGIAdapter1> adapter;

        ComPtr<IDXGIFactory6> factory6;
        if (SUCCEEDED(pFactory->QueryInterface(IID_PPV_ARGS(&factory6))))
        {
            for (
                UINT adapterIndex = 0;
                SUCCEEDED(factory6->EnumAdapterByGpuPreference(
                    adapterIndex,
                    requestHighPerformanceAdapter == true ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE : DXGI_GPU_PREFERENCE_UNSPECIFIED,
                    IID_PPV_ARGS(&adapter)));
                    ++adapterIndex)
            {
                DXGI_ADAPTER_DESC1 desc;
                adapter->GetDesc1(&desc);

                if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                {
                    // Don't select the Basic Render Driver adapter.
                    // If you want a software adapter, pass in "/warp" on the command line.
                    continue;
                }

                // Check to see whether the adapter supports Direct3D 12, but don't create the
                // actual device yet.
                if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr)))
                {
                    break;
                }
            }
        }

        if (adapter.Get() == nullptr) {
            for (uint32_t adapterIndex = 0; SUCCEEDED(pFactory->EnumAdapters1(adapterIndex, &adapter)); ++adapterIndex) {
                DXGI_ADAPTER_DESC1 desc;
                adapter->GetDesc1(&desc);

                if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                    // Don't select the Basic Render Driver adapter.
                    // If you want a software adapter, pass in "/warp" on the command line.
                    continue;
                }

                // Check to see whether the adapter supports Direct3D 12, but don't create the
                // actual device yet.
                if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr)))
                    break;
            }
        }

        *ppAdapter = adapter.Detach();
    }

    void set_window_title(const std::wstring &title) {
        auto applicationView = Windows::UI::ViewManagement::ApplicationView::GetForCurrentView();
        applicationView->Title = ref new Platform::String(title.c_str());
    }

private:
    static const uint32_t               max_frames_in_flight_   = 2;
    static const uint32_t               thread_count_           = 1;
    const uint32_t                      particle_count_         = 10000;
    const float                         particle_spread_        = 400.0f;

    // pipeline objects
    CD3DX12_VIEWPORT                    viewport_;
    CD3DX12_RECT                        scissor_rect_;
    ComPtr<IDXGISwapChain3>             swapchain_;
    ComPtr<ID3D12Device>                device_;
    ComPtr<ID3D12Resource>              rendertargets_[max_frames_in_flight_];
    uint32_t                            current_frame_;
    ComPtr<ID3D12CommandAllocator>      command_allocators_[max_frames_in_flight_];
    ComPtr<ID3D12CommandQueue>          command_queue_;
    ComPtr<ID3D12RootSignature>         root_signature_;
    ComPtr<ID3D12RootSignature>         compute_root_signature_;
    ComPtr<ID3D12DescriptorHeap>        RTVheap_;
    ComPtr<ID3D12DescriptorHeap>        SRV_UAVheap_;
    uint32_t                            RTVdescriptor_size_;
    uint32_t                            SRV_UAVdescriptor_size_;

    // asset objects
    ComPtr<ID3D12PipelineState>         pipeline_state_;
    ComPtr<ID3D12PipelineState>         compute_state_;
    ComPtr<ID3D12GraphicsCommandList>   command_list_;
    ComPtr<ID3D12Resource>              vertex_buffer_;
    ComPtr<ID3D12Resource>              vertex_buffer_upload_;
    D3D12_VERTEX_BUFFER_VIEW            vertex_buffer_view_;
    ComPtr<ID3D12Resource>              particle_buffer0_[thread_count_];
    ComPtr<ID3D12Resource>              particle_buffer1_[thread_count_];
    ComPtr<ID3D12Resource>              particle_buffer0_upload_[thread_count_];
    ComPtr<ID3D12Resource>              particle_buffer1_upload_[thread_count_];
    ComPtr<ID3D12Resource>              geometry_constant_buffer_;
    uint8_t*                            geometry_constant_buffer_data_;
    ComPtr<ID3D12Resource>              compute_constant_buffer_;

    uint32_t                            SRVindex_[thread_count_];           // which of the particle buffer resource views is the SRV (0 or 1) 
                                                                            // the UAV is 1 - srvIndex
    
    uint32_t                            height_instances_;
    uint32_t                            width_instances_;
    camera                              camera_;
    StepTimer                           timer_;

    // compute shader related objects
    ComPtr<ID3D12CommandAllocator>      compute_allocator_[thread_count_];
    ComPtr<ID3D12CommandQueue>          compute_command_queue_[thread_count_];
    ComPtr<ID3D12GraphicsCommandList>   compute_command_list_[thread_count_];

    // synchronization objects
    HANDLE                              swapchain_event_;
    ComPtr<ID3D12Fence>                 render_context_fence_;
    uint64_t                            render_context_fence_value_;
    HANDLE                              render_context_fence_event_;
    uint64_t                            frame_fence_values_[max_frames_in_flight_];

    ComPtr<ID3D12Fence>                 thread_fences_[thread_count_];
    volatile HANDLE                     thread_fence_events_[thread_count_];

    // Thread state.
    long volatile                       terminating_;
    uint64_t volatile                   render_context_fence_values_[thread_count_];
    uint64_t volatile                   thread_fence_values_[thread_count_];

    thread_t                            thread_data_[thread_count_];
    HANDLE                              thread_handle_[thread_count_];

    // viewport-related variables
    uint32_t                            width_;
    uint32_t                            height_;

    float                               aspect_ratio_;
    bool                                use_warp_device_;

    std::wstring                        assets_path_;
    std::wstring                        window_title_;
};
