#include "DXWrapper.h"

#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_3.h>
#include <winerror.h>
#include <cassert>
#include <stdio.h>
#include <fstream>
#include <filesystem>

#include <wchar.h>

#include "CPUMemory.h"
#include "ResourceEnums.h"

// Core rendering utilities
using namespace Microsoft::WRL;
ComPtr<ID3D12Debug> debugLayer;
ComPtr<ID3D12Device5> device;
DXGI_ADAPTER_DESC1 adapterInfo;
ComPtr<ID3D12CommandQueue> gfxQueue;
ComPtr<IDXGISwapChain1> swapChain;

// Swapchain format; should eventually make this settable on-init
// (and allow equal support for HDR and LDR formats)
const DXGI_FORMAT swapChainFmt = DXGI_FORMAT_R8G8B8A8_UNORM;

// CPU/GPU sync utilities
ComPtr<ID3D12Fence> syncGPU; // Called at the start of each frame to avoid invoking a draw before the previous frame has finished
HANDLE				syncCPU;

// Variant-agnostic resource type; allows packing abstract resources together and smoothly supporting multiple views/resource)
// (=> filling in some indirection buffers & adding a supported variant + variant offset)
enum RT_DISAMBIG_OPTIONS
{
	NOT_RT_AS,
	RT_BLAS,
	RT_TLAS
};

struct D3DResource
{
    ComPtr<ID3D12Resource> resrc = nullptr;
    ResourceViews curr_variant = ResourceViews::CBUFFER; // BLAS and TLAS structures both carry the [RT_ACCEL_STRUCTURE] variant for compatibility with user-facing RHI code
	RT_DISAMBIG_OPTIONS rt_settings = NOT_RT_AS; // Required to disambiguate BLAS/TLAS structure specializations for resource comparisons + safe AS updates
	bool is_variant_supported[(int32_t)ResourceViews::NUM_VARIANTS] = {}; // Probably unreasonably high, but a safe upper bound + better than handing staleness if we set a lower cap and end up needing more
	bool initialized = false; // Resources are expected to be initialized with copies/clears either immediately on creation, or sometime before their first use
};

constexpr uint32_t maxResources = XPlatConstants::maxNumPipelines * XPlatConstants::maxResourcesPerPipeline;
D3DResource resources[maxResources] = {};
uint32_t numResources[XPlatConstants::maxNumPipelines] = {};

// Structbuffer lookups
struct StructBuffer
{
	uint32_t stride;
	uint32_t eltCount;
};
StructBuffer structBufferData[maxResources];

// Texture lookups
DXGI_FORMAT textureFmts[maxResources]; // Every texture is assumed to have a standard format ("textures" with non-standard formats are buffers)

// Cbuffer lookups
uint32_t cbufferStrides[maxResources]; // Indexed directly using resource indices, hashmap-style

// Vertex-buffer lookups
struct VertEltFormats
{
	DXGI_FORMAT fmts[XPlatConstants::maxEltsPerVertex];
};
VertEltFormats vbufferEltFmtsPerVert[maxResources];
uint32_t vbufferEltCountsPerVert[maxResources];

// Root signatures
ComPtr<ID3D12RootSignature> rootSigs[XPlatConstants::maxNumPipelines];
uint32_t numRootSigs = 0;

// Pipeline-state-objects
/////////////////////////

// Duplicate GFX PSOs, for multi-buffering support
// (the alternative is cheating by sending all frames to the same intermediate target, which...ew? I can't tell if that's a good idea or a terrible one)
// (good - simple, bad - double the bandwidth overhead for presentation shaders)
struct MultibufferGfx_PSOs // Can generalize to other/more API objects as needed
{
	ComPtr<ID3D12PipelineState> psos[XPlatConstants::numBackBuffers] = {};
};

MultibufferGfx_PSOs gfx_psos[XPlatConstants::maxNumPipelines * XPlatConstants::maxNumGfxShaders] = {};
ComPtr<ID3D12PipelineState> compute_psos[XPlatConstants::maxNumPipelines * XPlatConstants::maxNumComputeShaders] = {};
uint32_t numGFX_PSOs[XPlatConstants::maxNumPipelines] = {};
uint32_t numCompute_PSOs[XPlatConstants::maxNumPipelines] = {};

// Records whether a pipeline will draw to the back-buffer, so we can correctly transition it to/from a render-target and back
bool writesToBackBuffer[XPlatConstants::maxNumPipelines] = {};

// Recording the current back-buffer index each frame
uint8_t currBackBuffer = 0;

// + "state objects" for raytracing
ComPtr<ID3D12StateObject> rt_psos[XPlatConstants::maxNumPipelines * XPlatConstants::maxNumRaytracingShaders];
uint32_t numRT_PSOs[XPlatConstants::maxNumPipelines] = {};

// Input layouts for raster pipelines
// Clever memory safety things here are obsolesced by [tracked_ptr], but easier to keep them and take the performance hit than refactor
struct InputLayoutDesc
{
	private:
		D3D12_INPUT_LAYOUT_DESC apiLayoutDesc = {};
		CPUMemory::ArrayAllocHandle<D3D12_INPUT_ELEMENT_DESC> inputElementDescsAlloc;
		size_t eltsFootprint = 0;

	public:
		void Init(UINT numElements, CPUMemory::ArrayAllocHandle<D3D12_INPUT_ELEMENT_DESC> srcDescs, size_t _eltsFootprint)
		{
			eltsFootprint = _eltsFootprint;
			inputElementDescsAlloc = CPUMemory::AllocateArray<D3D12_INPUT_ELEMENT_DESC>(numElements);
			CPUMemory::CopyData(srcDescs, inputElementDescsAlloc);
			apiLayoutDesc.pInputElementDescs = &inputElementDescsAlloc[0];
			apiLayoutDesc.NumElements = numElements;
		};

		D3D12_INPUT_LAYOUT_DESC GetDesc()
		{
			// Precondition - apiLayoutDesc.pInputElementDescs must equal inputElementDescsAlloc, they can drift apart when our allocator
			// de-fragments (reallocating [inputElementDescsAlloc]) on releasing other data
			apiLayoutDesc.pInputElementDescs = &inputElementDescsAlloc[0];
			return apiLayoutDesc;
		}

		uint32_t GetNumElements()
		{
			return apiLayoutDesc.NumElements;
		}

		bool Compare(CPUMemory::ArrayAllocHandle<D3D12_INPUT_ELEMENT_DESC> elts, size_t bytesComparing)
		{
			assert(bytesComparing <= eltsFootprint);
			return (CPUMemory::CompareData<D3D12_INPUT_ELEMENT_DESC>(elts, inputElementDescsAlloc) == 0);
		}
};

InputLayoutDesc rasterInputLayouts[maxResources]; // Worst-case scenario where all resources are different vbuffers
uint32_t numInputLayouts = 0;

// Primitive topologies for raster pipelines
D3D12_PRIMITIVE_TOPOLOGY gfx_topologies[XPlatConstants::maxNumPipelines * XPlatConstants::maxNumGfxShaders] = {};

// Command-lists
ComPtr<ID3D12GraphicsCommandList> cmdLists[XPlatConstants::maxNumPipelines]; // Need to implement work submission (+ gpu memory management) interfaces
ComPtr<ID3D12CommandAllocator> cmdAllocators[XPlatConstants::maxNumPipelines];
bool cmdListsOpen[XPlatConstants::maxNumPipelines];

uint32_t numCmdLists = 0;

// Descriptor heaps
struct PipelineDescriptorHeaps
{
	ComPtr<ID3D12DescriptorHeap> genericResrcViews;
	ComPtr<ID3D12DescriptorHeap> samplerViews;
	ComPtr<ID3D12DescriptorHeap> renderTargetViews;
	ComPtr<ID3D12DescriptorHeap> depthStencilViews;

	void Reset()
	{
		genericResrcViews.Reset();
		samplerViews.Reset();
		renderTargetViews.Reset();
		depthStencilViews.Reset();
	}
};
PipelineDescriptorHeaps descriptorHeaps[XPlatConstants::maxNumPipelines];

D3D12_CPU_DESCRIPTOR_HANDLE cbv_uav_srvDescriptorPtrs[maxResources] = {};
D3D12_CPU_DESCRIPTOR_HANDLE samplerDescriptorPtrs[maxResources] = {};
D3D12_CPU_DESCRIPTOR_HANDLE rtvDescriptorPtrs[XPlatConstants::maxNumRenderTargetsPerPipeline() * XPlatConstants::maxNumPipelines] = {};
D3D12_CPU_DESCRIPTOR_HANDLE dsvDescriptorPtrs[XPlatConstants::maxNumPipelines] = {};

uint32_t numSamplerDescriptors[XPlatConstants::maxNumPipelines] = {};
uint32_t numRTV_Descriptors[XPlatConstants::maxNumPipelines] = {};

constexpr uint32_t maxDescriptorsPerHeap = maxResources;

// Resource-specific descriptor accessors
// Required because these are bound on the input-assembler instead of through regular shader root descriptors, so we can't JIT
// them and forget about the descriptor handles afterwards like we can with those
DXWrapper::DataHandle<D3D_DESCRIPTOR_HANDLE> renderTargetDescriptors[maxResources];
DXWrapper::DataHandle<D3D_DESCRIPTOR_HANDLE> depthStencilDescriptors[maxResources];

// Resource-specific descriptors
// These are generated directly without using a descriptor heap (idk why?), so we can store them directly here instead of indirecting like we do above
D3D12_INDEX_BUFFER_VIEW indexBufferDescriptors[XPlatConstants::maxNumPipelines];
D3D12_VERTEX_BUFFER_VIEW vertexBufferDescriptors[XPlatConstants::maxNumPipelines];

// Hardcoded texture samplers
D3D12_STATIC_SAMPLER_DESC staticSamplers[DXWrapper::STATIC_SAMPLERS::NUM_STATIC_SAMPLERS];

// Use a null stencil (writes nothing, never passes the stencil test) for faces intended to be culled by the rasterizer on graphics pipelines
D3D12_DEPTH_STENCILOP_DESC nullStencil;

// Background command-list/allocator; used for resource marshalling & other tasks we want to resolve before each frame executes without intruding on the user-facing
// frame graph
ComPtr<ID3D12GraphicsCommandList4> bgCmdList;
ComPtr<ID3D12CommandAllocator> bgCmdAlloc;

constexpr uint32_t maxTmpResources = XPlatConstants::maxResourcesPerPipeline * XPlatConstants::maxNumPipelines;
ComPtr<ID3D12Resource> tmpResrcPool[maxTmpResources]; // Background, short-term resource used for background copies (e.g. resource initialization through the upload heap)
uint32_t numTmpResources = 0; // Temporary counter, reset every frame

// Background GPU memory buffers + offsets within each buffer
enum HEAP_TYPES
{
	UPLOAD_HEAP,
	DOWNLOAD_HEAP,
	GPU_ONLY_HEAP,
	NUM_HEAP_TYPES
};

constexpr uint32_t maxBytesPerHeap = 1024 * 1024 * 128; // 96MB heaps, to fit entirely on chip memory for laptops - feasible? no idea :D but curious to try
constexpr uint32_t maxResourceBytes = maxBytesPerHeap * NUM_HEAP_TYPES;

// Total RHI usage is assumed to be total resource usage + 10MB worth of descriptors/shaders/etc (hopefully plenty)
constexpr uint32_t nonResourceBytesEstimated = 10 * 1024 * 1024;
constexpr uint32_t maxRHIBytes = maxResourceBytes + nonResourceBytesEstimated;

ComPtr<ID3D12Heap> resourceHeaps[NUM_HEAP_TYPES];
uint64_t heapOffsets[NUM_HEAP_TYPES] = {}; // One heap-offset/heap

uint64_t d3dSetupTime = 0;

// Single fixed viewport & scissor-rect for now (we have bigger problems if we want to enable screen resizing)
// Will make these user-controlled when I get around to resizing support
D3D12_VIEWPORT viewport = {};
D3D12_RECT scissor = {};

static bool vsyncActive = false;
bool DXWrapper::Init(HWND hwnd, uint32_t screenWidth, uint32_t screenHeight, bool vsynced)
{
	// Initialize the debug layer when _DEBUG is defined
	#ifdef _DEBUG
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugLayer))))
		{ debugLayer->EnableDebugLayer(); }
	#endif

	// Create a DXGI builder object
	Microsoft::WRL::ComPtr<IDXGIFactory2> dxgiBuilder = nullptr;
#ifdef _DEBUG
	UINT hr = CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&dxgiBuilder));
#else
	UINT hr = CreateDXGIFactory2(NULL, IID_PPV_ARGS(&dxgiBuilder));
#endif
	assert(SUCCEEDED(hr));

	// Find an appropriate GPU (first available for PIX captures, discrete otherwise) +
	// create the device
	Microsoft::WRL::ComPtr<IDXGIAdapter1> gpuHW = nullptr;
	bool dgpuFound = false;
	for (UINT adapterNdx = 0;
		 dxgiBuilder->EnumAdapters1(adapterNdx, &gpuHW) != DXGI_ERROR_NOT_FOUND;
		 adapterNdx += 1) // Iterate over all available adapters
	{
		DXGI_ADAPTER_DESC1 tmpGPUInfo;
		hr = gpuHW->GetDesc1(&tmpGPUInfo); // Get descriptions for iterated adapters

		if (tmpGPUInfo.DedicatedVideoMemory >= maxRHIBytes) // Less arbitrary test here, driven by global GPU memory estimates
		{
			if (!(tmpGPUInfo.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) // Skip past any defined software adapters (might want to change this to support running on WARP for newer features/testing)
			{
				hr = D3D12CreateDevice(gpuHW.Get(), D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_12_2, _uuidof(ID3D12Device), &device); // We want DXR!
				if (SUCCEEDED(hr))
				{
					hr = gpuHW->GetDesc1(&adapterInfo);
					assert(SUCCEEDED(hr));
					dgpuFound = true;
					break;
				}
			}
		}
	}

	// Error out if a discrete GPU couldn't be found in the system
	if (!dgpuFound)
	{
		wchar_t log[256] = {};
		wsprintf(log, L"Either no DX12_2 gpu found, or the available GPU had less than %u bytes of dedicated memory\n", maxRHIBytes);
		OutputDebugString(log);
		assert(false);
		return false;
	}

	// Create the graphics command queue
	D3D12_COMMAND_QUEUE_DESC cmdQueueDesc;
	cmdQueueDesc.Type = D3D12_COMMAND_LIST_TYPE::D3D12_COMMAND_LIST_TYPE_DIRECT;
	cmdQueueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY::D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
	cmdQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAGS::D3D12_COMMAND_QUEUE_FLAG_NONE;
	cmdQueueDesc.NodeMask = 0x1; // Only one adapter supported
	hr = device->CreateCommandQueue(&cmdQueueDesc, __uuidof(ID3D12CommandQueue), (void**)&gfxQueue);
	assert(SUCCEEDED(hr));

	// Describe + construct the swap-chain
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.Width = screenWidth;
	swapChainDesc.Height = screenHeight;
	swapChainDesc.Format = swapChainFmt; // 8-bit swapchain (might consider 10-bit support when/if I get a HDR monitor)
	swapChainDesc.Stereo = FALSE;
	swapChainDesc.SampleDesc.Count = 1; // Manual surface multisampling
	swapChainDesc.SampleDesc.Quality = 0;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_BACK_BUFFER; // This is the main presentation swap-chain
	swapChainDesc.BufferCount = XPlatConstants::numBackBuffers;
	swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
	swapChainDesc.Flags = vsynced ? 0 : DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING; // Disabling vsync might be needed for hardware-sync systems like
																			// G-Sync or Freesync (also for performance debugging/verification)

	D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_12_1;
	hr = dxgiBuilder->CreateSwapChainForHwnd(gfxQueue.Get(), // Not actually the device pointer; see: https://docs.microsoft.com/en-us/windows/desktop/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforhwnd
											 hwnd,
											 &swapChainDesc,
											 nullptr,
											 nullptr,
											 &swapChain);
	assert(SUCCEEDED(hr));

	// Create cpu/gpu synchronization fences + events
	device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(syncGPU), (void**)&syncGPU); // Fences alternate between [1] and [0] on synchronization
	syncGPU->Signal(0); // Initialize GPU fence to zero (ready for work)
	syncCPU = CreateEvent(NULL, FALSE, FALSE, L"Waiting for core rendering work");

	// Allocate background command-list/allocator + memory-management interfaces
	////////////////////////////////////////////////////////////////////////////

	// Background command list/allocator
	device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&bgCmdAlloc));
	device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, bgCmdAlloc.Get(), nullptr, IID_PPV_ARGS(&bgCmdList));
	bgCmdList->Close();
	bgCmdList->SetName(L"RHI (DX) background command list");
	bgCmdList->Reset(bgCmdAlloc.Get(), nullptr); // Open by default, close on work submission (regardless of whether the cmd-list was used or not)

	// Memory-management interfaces
	for (uint32_t i = 0; i < HEAP_TYPES::NUM_HEAP_TYPES; i++)
	{
		D3D12_HEAP_DESC heapDesc;
		heapDesc.SizeInBytes = maxBytesPerHeap;
		heapDesc.Properties.Type = (i == HEAP_TYPES::UPLOAD_HEAP) ? D3D12_HEAP_TYPE::D3D12_HEAP_TYPE_UPLOAD :
								   (i == HEAP_TYPES::DOWNLOAD_HEAP) ? D3D12_HEAP_TYPE::D3D12_HEAP_TYPE_READBACK :
								   /*(i == HEAP_TYPES::GPU_ONLY_HEAP) ? */D3D12_HEAP_TYPE::D3D12_HEAP_TYPE_DEFAULT;

		heapDesc.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		heapDesc.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN; // Not enough information about target hardware to decide this
		heapDesc.Properties.CreationNodeMask = 0; // No multi-gpu support in DXRSandbox
		heapDesc.Properties.VisibleNodeMask = 0; // No multi-gpu support in DXRSandbox
		heapDesc.Alignment = D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT; // 4MB alignment will fit 64KB resources, and this allows casual MSAA support with a relatively small overhead

		heapDesc.Flags = D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;
		if (i == HEAP_TYPES::GPU_ONLY_HEAP)
		{
			heapDesc.Flags |= D3D12_HEAP_FLAG_ALLOW_SHADER_ATOMICS;
		}

		device->CreateHeap(&heapDesc, IID_PPV_ARGS(&resourceHeaps[i]));
		heapOffsets[i] = 0; // Probably unnecessary, but good for peace-of-mind
	}

	// Descriptor-heap & descriptor-heap pointer setup
	// Lots of duplicate code here - good case for a helper function
	for (uint32_t i = 0; i < XPlatConstants::maxNumPipelines; i++)
	{
		D3D12_DESCRIPTOR_HEAP_DESC descHeapDesc[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES] = {};
		descHeapDesc[0].Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		descHeapDesc[0].NumDescriptors = XPlatConstants::maxResourcesPerPipeline;
		descHeapDesc[0].Flags = D3D12_DESCRIPTOR_HEAP_FLAGS::D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		descHeapDesc[0].NodeMask = 0; // No plans for multi-adapter support ^_^'

		descHeapDesc[1].Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
		descHeapDesc[1].NumDescriptors = XPlatConstants::maxResourcesPerPipeline;
		descHeapDesc[1].Flags = D3D12_DESCRIPTOR_HEAP_FLAGS::D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		descHeapDesc[1].NodeMask = 0; // No plans for multi-adapter support ^_^'

		descHeapDesc[2].Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		descHeapDesc[2].NumDescriptors = XPlatConstants::maxResourcesPerPipeline;
		descHeapDesc[2].Flags = D3D12_DESCRIPTOR_HEAP_FLAGS::D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		descHeapDesc[2].NodeMask = 0; // No plans for multi-adapter support ^_^'

		descHeapDesc[3].Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		descHeapDesc[3].NumDescriptors = XPlatConstants::maxResourcesPerPipeline;
		descHeapDesc[3].Flags = D3D12_DESCRIPTOR_HEAP_FLAGS::D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		descHeapDesc[3].NodeMask = 0; // No plans for multi-adapter support ^_^'

		device->CreateDescriptorHeap(&descHeapDesc[0], IID_PPV_ARGS(&descriptorHeaps[i].genericResrcViews));
		cbv_uav_srvDescriptorPtrs[i * XPlatConstants::maxResourcesPerPipeline] = descriptorHeaps[i].genericResrcViews->GetCPUDescriptorHandleForHeapStart();

		device->CreateDescriptorHeap(&descHeapDesc[1], IID_PPV_ARGS(&descriptorHeaps[i].samplerViews));
		samplerDescriptorPtrs[i * XPlatConstants::maxResourcesPerPipeline] = descriptorHeaps[i].samplerViews->GetCPUDescriptorHandleForHeapStart();

		device->CreateDescriptorHeap(&descHeapDesc[2], IID_PPV_ARGS(&descriptorHeaps[i].renderTargetViews));
		rtvDescriptorPtrs[i * XPlatConstants::maxNumRenderTargetsPerPipeline()] = descriptorHeaps[i].renderTargetViews->GetCPUDescriptorHandleForHeapStart();

		device->CreateDescriptorHeap(&descHeapDesc[3], IID_PPV_ARGS(&descriptorHeaps[i].depthStencilViews));
		dsvDescriptorPtrs[i] = descriptorHeaps[i].depthStencilViews->GetCPUDescriptorHandleForHeapStart();
	}

	// Initialize static point & linear samplers
	////////////////////////////////////////////

	// Initialize static point sampler
	staticSamplers[0].Filter = D3D12_FILTER::D3D12_FILTER_MIN_MAG_MIP_POINT;
	staticSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP; // All samplers use wrap filtering
	staticSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	staticSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	staticSamplers[0].MipLODBias = 0;
	staticSamplers[0].MaxAnisotropy = 1;
	staticSamplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS; // Maybe? This feels best for depth textures
	staticSamplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
	staticSamplers[0].MinLOD = 0;
	staticSamplers[0].MaxLOD = 0; // We're not using mipmaps for this project atm
	staticSamplers[0].ShaderRegister = 0; // Static point sampler always lives in s0
	staticSamplers[0].RegisterSpace = 0; // No use for this feature yet
	staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	// Linear sampler is the same as the point sampler, just with a different filter & shader register
	staticSamplers[1] = staticSamplers[0];
	staticSamplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	staticSamplers[1].ShaderRegister = 1; // Static linear sampler always lives in s1

	// Set-up null stencil
	nullStencil.StencilFailOp = D3D12_STENCIL_OP_KEEP;
	nullStencil.StencilPassOp = D3D12_STENCIL_OP_KEEP;
	nullStencil.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	nullStencil.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

	// Stash setup time, to help individualize captures
	auto t = std::chrono::steady_clock::now();
	d3dSetupTime = t.time_since_epoch().count();

	// Populate viewport + scissor settings
	viewport.TopLeftX = 0;
	viewport.TopLeftY = 0;
	viewport.Width = static_cast<float>(screenWidth);
	viewport.Height = static_cast<float>(screenHeight);
	viewport.MinDepth = D3D12_MIN_DEPTH;
	viewport.MaxDepth = D3D12_MAX_DEPTH; // We don't care about distant clipping atm

	scissor.left = 0;
	scissor.top = 0;
	scissor.right = screenWidth;
	scissor.bottom = screenHeight;

	return true; // Setup passed without errors, return true (probably need more validation here...)
}

void DXWrapper::Deinit()
{
	// Release resources
	for (uint32_t i = 0; i < XPlatConstants::maxNumPipelines; i++)
	{
		for (uint32_t j = 0; j < numResources[i]; j++)
		{
			resources[i * XPlatConstants::maxResourcesPerPipeline].resrc.Reset();
		}
	}

	// Release root signatures
	for (uint32_t i = 0; i < numRootSigs; i++)
	{
		rootSigs[i].Reset();
	}

	// Release PSOs for each "pipeline"
	for (uint32_t i = 0; i < XPlatConstants::maxNumPipelines; i++)
	{
		for (uint32_t j = 0; j < numGFX_PSOs[i]; j++)
		{
			for (uint32_t k = 0; k < XPlatConstants::numBackBuffers; k++)
			{
				gfx_psos[(i * XPlatConstants::maxNumGfxShaders) + j].psos[k].Reset();
			}
		}

		for (uint32_t j = 0; j < numCompute_PSOs[i]; j++)
		{
			compute_psos[(i * XPlatConstants::maxNumComputeShaders) + j].Reset();
		}

		for (uint32_t j = 0; j < numRT_PSOs[i]; j++)
		{
			rt_psos[(i * XPlatConstants::maxNumRaytracingShaders) + j].Reset();
		}
	}

	for (uint32_t i = 0; i < XPlatConstants::maxNumPipelines; i++)
	{
		// Release descriptor heaps
		descriptorHeaps[i].Reset();

		// Release external work submission interfaces
		cmdLists[i].Reset();
		cmdAllocators[i].Reset();
	}

	// Release background work submission interfaces
	bgCmdList.Reset();
	bgCmdAlloc.Reset();

	// Release temporary resources
	for (uint32_t i = 0; i < numTmpResources; i++)
	{
		tmpResrcPool[i].Reset();
	}

	// Release core interfaces
	gfxQueue.Reset();
	swapChain.Reset();
	syncGPU.Reset();
	debugLayer.Reset();
	device.Reset();
}

template<D3D_OBJ_FMT objFmt>
D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress(DXWrapper::DataHandle<objFmt> handle)
{
	static_assert(objFmt == D3D_OBJ_FMT::D3D_CBUFFER ||
				  objFmt == D3D_OBJ_FMT::D3D_VBUFFER ||
				  objFmt == D3D_OBJ_FMT::D3D_IBUFFER ||
				  objFmt == D3D_OBJ_FMT::D3D_TEXTURE ||
				  objFmt == D3D_OBJ_FMT::D3D_STRUCTBUFFER ||
				  objFmt == D3D_OBJ_FMT::D3D_ACCELSTRUCT_BLAS ||
				  objFmt == D3D_OBJ_FMT::D3D_ACCELSTRUCT_TLAS, "GPU virtual addresses are only supported for textures, buffers, and acceleration structures");
	return resources[handle.index].resrc->GetGPUVirtualAddress();
}

uint32_t GetCBufferStride(DXWrapper::DataHandle<D3D_CBUFFER> handle)
{
	return cbufferStrides[handle.index];
}

// Helper function to convert our high-level standard texture/buffer formats to DXGI_FORMAT equivalents
DXGI_FORMAT DecodeSandboxStdFormats(StandardResrcFmts fmt)
{
	switch (fmt)
	{
		// 32bpc floating-point formats
		case StandardResrcFmts::FP32_1:
			return DXGI_FORMAT_R32_FLOAT;
		case StandardResrcFmts::FP32_2:
			return DXGI_FORMAT_R32G32_FLOAT;
		case StandardResrcFmts::FP32_3:
			return DXGI_FORMAT_R32G32B32_FLOAT;
		case StandardResrcFmts::FP32_4:
			return DXGI_FORMAT_R32G32B32A32_FLOAT;

			// 16bpc floating-point formats
		case StandardResrcFmts::FP16_1:
			return DXGI_FORMAT_R16_FLOAT;
		case StandardResrcFmts::FP16_2:
			return DXGI_FORMAT_R16G16_FLOAT;
		//case StandardResrcFmts::FP16_3:
		//	return DXGI_FORMAT_R16G16B16_FLOAT;
		case StandardResrcFmts::FP16_4:
			return DXGI_FORMAT_R16G16B16A16_FLOAT;

		// 32bpc unsigned integer formats
		case StandardResrcFmts::U32_1:
			return DXGI_FORMAT_R32_UINT;
		case StandardResrcFmts::U32_2:
			return DXGI_FORMAT_R32G32_UINT;
		case StandardResrcFmts::U32_3:
			return DXGI_FORMAT_R32G32B32_UINT;
		case StandardResrcFmts::U32_4:
			return DXGI_FORMAT_R32G32B32A32_UINT;

			// 16bpc unsigned integer formats
		case StandardResrcFmts::U16_1:
			return DXGI_FORMAT_R16_UINT;
		case StandardResrcFmts::U16_2:
			return DXGI_FORMAT_R16G16_UINT;
		//case StandardResrcFmts::U16_3:
		//	return DXGI_FORMAT_R16G16B16_UINT;
		case StandardResrcFmts::U16_4:
			return DXGI_FORMAT_R16G16B16A16_UINT;

		// 8bpc unsigned integer formats
		case StandardResrcFmts::U8_1:
			return DXGI_FORMAT_R8_UINT;
		case StandardResrcFmts::U8_2:
			return DXGI_FORMAT_R8G8_UINT;
		//case StandardResrcFmts::U8_3:
		//	return DXGI_FORMAT_R8G8B8_UINT;
		case StandardResrcFmts::U8_4:
			return DXGI_FORMAT_R8G8B8A8_UINT;

		// 32bpc signed integer formats
		case StandardResrcFmts::S32_1:
			return DXGI_FORMAT_R32_SINT;
		case StandardResrcFmts::S32_2:
			return DXGI_FORMAT_R32G32_SINT;
		case StandardResrcFmts::S32_3:
			return DXGI_FORMAT_R32G32B32_SINT;
		case StandardResrcFmts::S32_4:
			return DXGI_FORMAT_R32G32B32A32_SINT;

		// 16bpc signed integer formats
		case StandardResrcFmts::S16_1:
			return DXGI_FORMAT_R16_UINT;
		case StandardResrcFmts::S16_2:
			return DXGI_FORMAT_R16G16_UINT;
		//case StandardResrcFmts::S16_3:
		//	return DXGI_FORMAT_R16G16B16_SINT;
		case StandardResrcFmts::S16_4:
			return DXGI_FORMAT_R16G16B16A16_UINT;

		// 8bpc signed integer formats
		case StandardResrcFmts::S8_1:
			return DXGI_FORMAT_R8_SINT;
		case StandardResrcFmts::S8_2:
			return DXGI_FORMAT_R8G8_SINT;
		//case StandardResrcFmts::S8_3:
		//	return DXGI_FORMAT_R8G8B8_SINT;
		case StandardResrcFmts::S8_4:
			return DXGI_FORMAT_R8G8B8A8_SINT;

		// Catch-all case for resource formats we don't want to support/which aren't representable by DXGI_FORMAT
		default:
			assert(false); // Resource format unavailable in DX12 :(
			printf("Tried to decode an unsupported color format ([StandardResrcFmt]) on D3D12; format index: %i\n", (uint8_t)fmt);
			return DXGI_FORMAT_R16G16B16A16_FLOAT; // Relatively safe default format
	};
}

// DecodeSandboxStdFormats, but for depth-stencil formats instead
DXGI_FORMAT DecodeSandboxDepthStencilFormats(StandardDepthStencilFormats fmt)
{
	switch (fmt)
	{
		// 32bpc floating-point formats
		case StandardDepthStencilFormats::DEPTH_16_UNORM_NO_STENCIL:
			return DXGI_FORMAT_D16_UNORM;
		case StandardDepthStencilFormats::DEPTH_24_UNORM_STENCIL_8:
			return DXGI_FORMAT_D24_UNORM_S8_UINT;
		case StandardDepthStencilFormats::DEPTH_32_FLOAT_NO_STENCIL:
			return DXGI_FORMAT_D32_FLOAT;
		case StandardDepthStencilFormats::DEPTH_32_FLOAT_STENCIL_8_PAD_24:
			return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;

			// Catch-all case for resource formats we don't want to support/which aren't representable by DXGI_FORMAT
		default:
			assert(false); // Resource format unavailable in DX12 :(
			printf("Tried to decode an unsupported depth-stencil format on D3D12; format index: %i\n", (uint8_t)fmt);
			return DXGI_FORMAT_D24_UNORM_S8_UINT; // Relatively safe default format
	};
}

DXGI_FORMAT DecodeSandboxIBufferFormats(StandardIBufferFmts fmt)
{
	switch (fmt)
	{
		// 32bpc floating-point formats
		case StandardIBufferFmts::S16:
			return DXGI_FORMAT_R16_SINT;
		case StandardIBufferFmts::S32:
			return DXGI_FORMAT_R32_SINT;
		case StandardIBufferFmts::U16:
			return DXGI_FORMAT_R16_UINT;
		case StandardIBufferFmts::U32:
			return DXGI_FORMAT_R32_UINT;

			// Catch-all case for resource formats we don't want to support/which aren't representable by DXGI_FORMAT
		default:
			assert(false); // Resource format unavailable in DX12 :(
			printf("Tried to decode an unsupported index-buffer format on D3D12; format index: %i\n", (uint8_t)fmt);
			return DXGI_FORMAT_R16_SINT; // Relatively safe default format
	};
}

// See: https://docs.microsoft.com/en-us/windows/win32/direct3d12/creating-a-root-signature
// ("code for defining a version 1.1 root signature")
DXWrapper::DataHandle<D3D_ROOTSIG> DXWrapper::ResolveRootSignature(ResourceBindList bindList, bool mayUseGraphics, uint32_t pipelineID)
{
	// Generate descriptors
	///////////////////////

	// Generate cbuffer descriptors
	const uint64_t descriptorHandleIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const uint32_t descriptorHeapPtrListStart = (pipelineID * XPlatConstants::maxResourcesPerPipeline);
	uint32_t descriptorHeapPtrsFront = descriptorHeapPtrListStart;
	if (bindList.cbufferEnabled)
	{
		// Generate descriptor for the current cbuffer (just one cbuffer/pipeline)
		D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
		cbvDesc.BufferLocation = GetGPUAddress(bindList.cbuffer);
		cbvDesc.SizeInBytes = GetCBufferStride(bindList.cbuffer);
		device->CreateConstantBufferView(&cbvDesc, cbv_uav_srvDescriptorPtrs[descriptorHeapPtrListStart]);

		// Prepare descriptor handle for the next resource (if possible)
		cbv_uav_srvDescriptorPtrs[descriptorHeapPtrListStart + 1].ptr = cbv_uav_srvDescriptorPtrs[descriptorHeapPtrListStart].ptr + descriptorHandleIncrement;
		descriptorHeapPtrsFront++;
	}

	// Generate SRV descriptors
	const uint32_t numSRVs = bindList.numReadOnlyTextures + (bindList.tlasEnabled ? 1 : 0);
	if (numSRVs > 0)
	{
		// Generate descriptors for acceleration structure sub-resources
		////////////////////////////////////////////////////////////////

		if (bindList.tlasEnabled)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.RaytracingAccelerationStructure.Location = GetGPUAddress<D3D_ACCELSTRUCT_TLAS>(bindList.topLevelAS);

			device->CreateShaderResourceView(resources[bindList.topLevelAS.index].resrc.Get(), &srvDesc, cbv_uav_srvDescriptorPtrs[descriptorHeapPtrsFront]);

			// More repeated code here, good opportunity for a helper function
			cbv_uav_srvDescriptorPtrs[descriptorHeapPtrsFront + 1].ptr = cbv_uav_srvDescriptorPtrs[descriptorHeapPtrsFront].ptr + descriptorHandleIncrement;
			descriptorHeapPtrsFront++;
		}

		// Generate descriptors for read-only textures
		for (uint32_t i = 0; i < bindList.numReadOnlyTextures; i++)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
			srvDesc.Format = textureFmts[bindList.readOnlyTextures[i].index];
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; // All textures are currently 2D, though that might change in the future
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Texture2D.MipLevels = 1;//RaytracingAccelerationStructure.Location = GetAccelStructureGPUAddress(bindList.accelStructures[i]);
			srvDesc.Texture2D.MostDetailedMip = 0;
			srvDesc.Texture2D.PlaneSlice = 0;
			srvDesc.Texture2D.ResourceMinLODClamp = 0;
			device->CreateShaderResourceView(resources[bindList.readOnlyTextures[i].index].resrc.Get(), &srvDesc, cbv_uav_srvDescriptorPtrs[descriptorHeapPtrsFront]);

			cbv_uav_srvDescriptorPtrs[descriptorHeapPtrsFront + 1].ptr = cbv_uav_srvDescriptorPtrs[descriptorHeapPtrsFront].ptr + descriptorHandleIncrement;
			descriptorHeapPtrsFront++;
		}
	}

	// Generate UAV descriptors
	const uint32_t numUAVs = bindList.numStructbuffers + bindList.numRWTextures;
	if (numUAVs > 0)
	{
		// Generate descriptors for structured buffers
		for (uint32_t i = 0; i < bindList.numStructbuffers; i++)
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = DXGI_FORMAT_UNKNOWN;
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
			uavDesc.Buffer.FirstElement = 0;
			uavDesc.Buffer.NumElements = structBufferData[bindList.structbuffers[i].index].eltCount;
			uavDesc.Buffer.StructureByteStride = structBufferData[bindList.structbuffers[i].index].stride;
			uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE; // No bytebuffers in DXRSandbox yet
			uavDesc.Buffer.CounterOffsetInBytes = 0; // For now - make adjustable if/when we ever add support for counter resources

			device->CreateUnorderedAccessView(resources[bindList.structbuffers[i].index].resrc.Get(), /* No support for append/consume buffers in DXRSandbox atm */ nullptr, &uavDesc, cbv_uav_srvDescriptorPtrs[descriptorHeapPtrsFront]);

			cbv_uav_srvDescriptorPtrs[descriptorHeapPtrsFront + 1].ptr = cbv_uav_srvDescriptorPtrs[descriptorHeapPtrsFront].ptr + descriptorHandleIncrement;
			descriptorHeapPtrsFront++;
		}

		// Generate descriptors for read/write textures
		for (uint32_t i = 0; i < bindList.numRWTextures; i++)
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = textureFmts[bindList.rwTextures[i].index];
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; // All textures are currently 2D, though that might change in the future
			uavDesc.Texture2D.MipSlice = 0;
			uavDesc.Texture2D.PlaneSlice = 0;
			device->CreateUnorderedAccessView(resources[bindList.rwTextures[i].index].resrc.Get(), nullptr, &uavDesc, cbv_uav_srvDescriptorPtrs[descriptorHeapPtrsFront]);

			cbv_uav_srvDescriptorPtrs[descriptorHeapPtrsFront + 1].ptr = cbv_uav_srvDescriptorPtrs[descriptorHeapPtrsFront].ptr + descriptorHandleIncrement;
			descriptorHeapPtrsFront++;
		}
	}

	// Resolve root signature
	/////////////////////////

	// Resolve cbuffer range
	D3D12_DESCRIPTOR_RANGE1 descRangeCBuffer;
	if (bindList.cbufferEnabled)
	{
		descRangeCBuffer.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
		descRangeCBuffer.NumDescriptors = 1;
		descRangeCBuffer.BaseShaderRegister = 0;
		descRangeCBuffer.RegisterSpace = 0; // No reason to use this feature yet
		descRangeCBuffer.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC; // Cbuffer values are locked & updated just before each frame, but not during execution
		descRangeCBuffer.OffsetInDescriptorsFromTableStart = 0; // Cbuffers are always the first resource in each descriptor heap
	}

	// Resolve srv range
	bool srvRange = false;
	D3D12_DESCRIPTOR_RANGE1 descRangeSRV = {};
	if (numSRVs > 0)
	{
		descRangeSRV.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		descRangeSRV.NumDescriptors = numSRVs;
		descRangeSRV.BaseShaderRegister = 0;
		descRangeSRV.RegisterSpace = 0; // No reason to use this feature yet
		descRangeSRV.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE; // Volatile because we assume almost all SRVs (definitely all SRVs initialized on the GPU in the zeroth frame) will be accessed on the GPU before being sampled/rendered
		descRangeSRV.OffsetInDescriptorsFromTableStart = bindList.cbufferEnabled ? 1 : 0; // SRVs come in right after cbuffers, and each pipeline has at most one cbuffer
		srvRange = true;
	}

	// Resolve uav range
	bool uavRange = false;
	D3D12_DESCRIPTOR_RANGE1 descRangeUAV = {};
	if (bindList.numRWTextures > 0 || bindList.numStructbuffers > 0)
	{
		descRangeUAV.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		descRangeUAV.NumDescriptors = numUAVs;
		descRangeUAV.BaseShaderRegister = 0;
		descRangeUAV.RegisterSpace = 0; // No reason to use this feature yet
		descRangeUAV.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE; // UAV values are likely to change in each frame
		descRangeUAV.OffsetInDescriptorsFromTableStart = bindList.cbufferEnabled && srvRange ? numSRVs + 1 :
														 !srvRange && bindList.cbufferEnabled ? 1 :
														 !bindList.cbufferEnabled && srvRange ? numSRVs :
														 /*!bindList.cbufferEnabled && !srvRange ? 0 :*/ 0; // UAVs are last in the table after SRVs and cbuffers
		uavRange = true;
	}

	// Pack cbv ranges into a contiguous array, then format the root signature itself
	/////////////////////////////////////////////////////////////////////////////////

	// Range packing
	uint32_t numViewTypes = 0;
	D3D12_DESCRIPTOR_RANGE1 cbvRanges[3] = {}; // Supports CBVs, SRVs, UAVs
	if (bindList.cbufferEnabled)
	{
		cbvRanges[0] = descRangeCBuffer;
		numViewTypes++;
	}
	if (bindList.tlasEnabled || bindList.numReadOnlyTextures > 0) // We have SRVs
	{
		if (bindList.cbufferEnabled)
		{
			cbvRanges[1] = descRangeSRV;
		}
		else
		{
			cbvRanges[0] = descRangeSRV;
		}
		numViewTypes++;
	}
	if (bindList.numRWTextures > 0 || bindList.numStructbuffers > 0) // We have UAVs
	{
		if (bindList.cbufferEnabled && srvRange)
		{
			cbvRanges[2] = descRangeUAV;
		}
		else if (srvRange || bindList.cbufferEnabled)
		{
			cbvRanges[1] = descRangeUAV;
		}
		else
		{
			cbvRanges[0] = descRangeUAV;
		}
		numViewTypes++;
	}

	// Resolve root parameters (just one atm, no reason to use root constants or single-descriptor parameters yet)
	D3D12_ROOT_PARAMETER1 rootParam;
	rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParam.DescriptorTable.NumDescriptorRanges = numViewTypes;
	rootParam.DescriptorTable.pDescriptorRanges = cbvRanges; // Cbuffers always land in b0
	rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // Cbuffers always visible everywhere (tbh thinking of making everything visible everywhere for simplicity/consistency)

	// Set-up root signature description
	D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc = {};
	rootSigDesc.Desc_1_1.NumParameters = 1;
	rootSigDesc.Desc_1_1.pParameters = &rootParam;
	rootSigDesc.Desc_1_1.NumStaticSamplers = bindList.staticSamplersEnabled[0] && bindList.staticSamplersEnabled[1] ? 2 :
											 bindList.staticSamplersEnabled[0] || bindList.staticSamplersEnabled[1] ? 1 :
											 /*!bindList.pointSamplerEnabled && !bindList.linearSamplerEnabled ? :*/ 0;
	rootSigDesc.Desc_1_1.pStaticSamplers = (rootSigDesc.Desc_1_1.NumStaticSamplers == 0) ? nullptr : staticSamplers;
	rootSigDesc.Desc_1_1.Flags = mayUseGraphics ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT : D3D12_ROOT_SIGNATURE_FLAG_NONE; // Possible future options are CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED, SAMPLER_HEAP_DIRECTLY_INDEXED, and LOCAL_ROOT_SIGNATURE
	rootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;

	// Create root signature
	ID3DBlob* serializedRootSig = nullptr;
	ID3DBlob* errBlob = nullptr;
	HRESULT hr = D3D12SerializeVersionedRootSignature(&rootSigDesc, &serializedRootSig, &errBlob);

	if (!SUCCEEDED(hr))
	{
		const char* error = (char*)errBlob->GetBufferPointer();
		assert(SUCCEEDED(hr));
	}

	device->CreateRootSignature(0, serializedRootSig->GetBufferPointer(), serializedRootSig->GetBufferSize(), IID_PPV_ARGS(&rootSigs[pipelineID]));

	// Return handle
	DataHandle<D3D_ROOTSIG> rootsig;
	rootsig.index = pipelineID;
	return rootsig;
}

DXWrapper::DataHandle<D3D_RASTER_INPUT_LAYOUT> DXWrapper::ResolveInputLayout(StandardResrcFmts* elementFormats, VertexEltSemantics* semantics, uint32_t numEltsPerVert)
{
	// Allocate scratch memory
	const size_t eltsFootprint = sizeof(D3D12_INPUT_ELEMENT_DESC) * numEltsPerVert;
	CPUMemory::ArrayAllocHandle<D3D12_INPUT_ELEMENT_DESC> elts = CPUMemory::AllocateArray<D3D12_INPUT_ELEMENT_DESC>(numEltsPerVert);

	CPUMemory::ArrayAllocHandle<uint32_t> semanticIndicesPerElt = CPUMemory::AllocateArray<uint32_t>(numEltsPerVert);

	// Resolve semantic indices
	uint32_t semanticCounts[(uint32_t)VertexEltSemantics::NUM_SUPPORTED_SEMANTICS] = {};
	for (uint32_t i = 0; i < numEltsPerVert; i++)
	{
		const uint32_t semanticLookup = (uint32_t)semantics[i];
		semanticIndicesPerElt[i] = semanticCounts[semanticLookup];
		semanticCounts[semanticLookup]++;
	}

	// Resolve vertex element metadata
	for (uint32_t i = 0; i < numEltsPerVert; i++)
	{
		elts[i].SemanticName = semantics[i] == VertexEltSemantics::POSITION ? "POSITION" :
							   semantics[i] == VertexEltSemantics::COLOR ? "COLOR" :
							   semantics[i] == VertexEltSemantics::NORMAL ? "NORMAL" :
							   /*semantics[i] == VertexEltSemantics::TEXCOORD ? */ "TEXCOORD";
		elts[i].SemanticIndex = semanticIndicesPerElt[i];
		elts[i].Format = DecodeSandboxStdFormats(elementFormats[i]);
		elts[i].InputSlot = 0; // Never more than one input-assembler in DXRSandbox
		elts[i].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT; // Vertex elements are always aligned in memory
		elts[i].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA; // No instancing support in DXRSandbox
		elts[i].InstanceDataStepRate = 0; // No instancing support in DXRSandbox
	}

	// Verify that the input-layout we've created is unique
	// (to allow support for re-using input layouts without filling up our buffer)
	bool uniqueInputLayout = true;
	uint32_t matchingInputLayout = 0;
	for (uint32_t i = 0; i < numInputLayouts; i++)
	{
		if (rasterInputLayouts[i].GetNumElements() == numEltsPerVert) // Layouts must be different if they have different element counts
		{
			// If both blocks are equal, [elts] is definitely not unique
			if (rasterInputLayouts[i].Compare(elts, eltsFootprint))
			{
				matchingInputLayout = i;
				uniqueInputLayout = false;
				break;
			}
		}
	}

	// Either create a new D3D12 input layout, or reuse an existing one
	DXWrapper::DataHandle<D3D_RASTER_INPUT_LAYOUT> handle;
	if (uniqueInputLayout)
	{
		// Prepare input layout & copy across scratch memory
		rasterInputLayouts[numInputLayouts].Init(numEltsPerVert, elts, eltsFootprint);

		// Record handle
		handle.index = numInputLayouts;

		// Increment input-layout counts
		numInputLayouts++;
	}
	else
	{
		// Record handle
		handle.index = matchingInputLayout;
	}

	// Free staging elements allocation
	CPUMemory::Free(elts);

	return handle;
}

D3D12_RESOURCE_STATES decodeVariantToState(ResourceViews variant)
{
	switch (variant)
	{
		case ResourceViews::VBUFFER:
			return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
		case ResourceViews::STRUCTBUFFER_RW:
			return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		case ResourceViews::IBUFFER:
			return D3D12_RESOURCE_STATE_INDEX_BUFFER;
		case ResourceViews::CBUFFER:
			return D3D12_RESOURCE_STATE_GENERIC_READ;
		case ResourceViews::TEXTURE_DIRECT_WRITE:
			return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		case ResourceViews::TEXTURE_SUPPORTS_SAMPLING:
			return D3D12_RESOURCE_STATE_GENERIC_READ;
		case ResourceViews::TEXTURE_STAGING:
			assert(false); // Staging resources can't be accessed on the gpu at all, so should never be transitioned to other resource states
		case ResourceViews::TEXTURE_RENDER_TARGET:
			return D3D12_RESOURCE_STATE_RENDER_TARGET;
		case ResourceViews::TEXTURE_DEPTH_STENCIL:
			return D3D12_RESOURCE_STATE_DEPTH_WRITE; // Tough, could justify DEPTH_WRITE or DEPTH_READ depending on context
		case ResourceViews::RT_ACCEL_STRUCTURE:
			return D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
		default:
			assert(false); // Unimplemented resource view
			return D3D12_RESOURCE_STATE_COMMON;
	}
}

bool CheckDepthStencilFormat(DXGI_FORMAT fmt)
{
	return (fmt == DXGI_FORMAT_D16_UNORM || fmt == DXGI_FORMAT_D24_UNORM_S8_UINT || fmt == DXGI_FORMAT_D32_FLOAT || fmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
}

void GenerateRenderTargetView(const ComPtr<ID3D12Resource>& resrc, uint32_t pipelineID, DXGI_FORMAT fmt)
{
	D3D12_RENDER_TARGET_VIEW_DESC desc;
	desc.Format = fmt;
	desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	desc.Texture2D.MipSlice = 0; // Not using these features atm
	desc.Texture2D.PlaneSlice = 0;

	const uint32_t descPtrNdx = (pipelineID * XPlatConstants::maxNumRenderTargetsPerPipeline());
	device->CreateRenderTargetView(resrc.Get(), &desc, rtvDescriptorPtrs[descPtrNdx + numRTV_Descriptors[pipelineID]]);
	numRTV_Descriptors[pipelineID]++;

	const uint32_t offset = descPtrNdx + numRTV_Descriptors[pipelineID];
	rtvDescriptorPtrs[offset].ptr = rtvDescriptorPtrs[offset - 1].ptr + device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
}

// Thinking transition barrier isn't sufficient - views can be generated here as well, but it's awkward, and I think some values I'd need to track are being lost (such as vertex element formats)
// Might create broader DXWrapper functions with extra parameters (enough to populate all those metadata channels I need later in the pipeline) and have them call InsertTransitionBarrier as part
// of their logic
// May not need that after all - vbuffer formats + depth read/write status might be the only needed data that I can't reconstruct - but should still generalize this function/interface, since it
// definitely isn't just creating a transition barrier at this point ^_^'
// Also, should replicate transition black/whitelists from GPUResource here - can rely on them to simplify my design, but the choices look weird without that context
void DXWrapper::InsertTransition(ResourceViews beforeVariant, ResourceViews afterVariant, uint64_t resrcNdx, uint8_t pipelineID)
{
	auto cmdList = cmdLists[pipelineID].Get();

	D3D12_RESOURCE_BARRIER barrier;
	barrier.Transition.pResource = resources[resrcNdx].resrc.Get();
	barrier.Transition.StateBefore = decodeVariantToState(beforeVariant);
	barrier.Transition.StateAfter = decodeVariantToState(afterVariant);
	barrier.Transition.Subresource = 0;
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;

	// Generate views for resources that won't receive them in [ResolveRootSignature]
	if (afterVariant == ResourceViews::TEXTURE_RENDER_TARGET)
	{
		// Prone to creating many more views than we need...probably bad
		GenerateRenderTargetView(resources[resrcNdx].resrc.Get(), pipelineID, textureFmts[resrcNdx]);
	}
	else if (afterVariant == ResourceViews::TEXTURE_DIRECT_WRITE)
	{
		// No explicit descriptor settings here, I think...
	}
	else if (afterVariant == ResourceViews::TEXTURE_SUPPORTS_SAMPLING)
	{
		// No explicit descriptor setings here, I think...
	}
	else if (afterVariant == ResourceViews::TEXTURE_DEPTH_STENCIL)
	{
		assert(CheckDepthStencilFormat(textureFmts[resrcNdx]));

		D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc;
		dsvDesc.Format = textureFmts[resrcNdx];
		dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		dsvDesc.Texture2D.MipSlice = 0; // Not using mip-slices atm
		dsvDesc.Flags = D3D12_DSV_FLAG_NONE; // No need to mask off depth/stencil yet; avoiding managing that complexity for now
		
		// No reason to re-allocate DSV if one is already defined for the current pipeline
		// (we don't support multiple depth-stencils per-pipeline)
		if (dsvDescriptorPtrs[pipelineID].ptr != NULL)
		{
			device->CreateDepthStencilView(resources[resrcNdx].resrc.Get(), &dsvDesc, dsvDescriptorPtrs[pipelineID]);
		}
	}

	resources[resrcNdx].curr_variant = afterVariant;
	resources[resrcNdx].is_variant_supported[static_cast<uint32_t>(afterVariant)] = true;

	cmdList->ResourceBarrier(1, &barrier);
}

void DXWrapper::NameResourceInternal(uint64_t resrcID, LPCWSTR name)
{
	resources[resrcID].resrc->SetName(name);
}

void DXWrapper::UpdateCBufferData(DataHandle<D3D_CBUFFER> handle, CPUMemory::ArrayAllocHandle<uint8_t> data)
{
	D3D12_RANGE readRange = {};
	void* copyDst = nullptr;

	D3D12_RANGE writeRange;
	writeRange.Begin = 0;
	writeRange.End = static_cast<SIZE_T>(data.arrayLen);
	
	resources[handle.index].resrc->Map(0, &readRange, &copyDst);
	CPUMemory::CopyData(data, copyDst);
	resources[handle.index].resrc->Unmap(0, &writeRange); // Not sure about scheduling these hmmmm - might want to queue between frames
}

// Shader file processing helpers
struct LoadedShaderBytecode
{
	LoadedShaderBytecode(const char* fname)
	{
		// Resolve filepath
		// Limited path validation, assumes simple & sane filenames are passed through and files are where they're expected to be
		std::string str = "../shaders/";
		str += fname;
		auto path = std::filesystem::absolute(str);
		uint32_t pathSize = static_cast<uint32_t>(std::filesystem::file_size(path)); // Shaders are probably smaller than 4GB
		assert(pathSize > 0); // If zero path size, file is empty or doesn't exist

		// Load shader
		data = CPUMemory::AllocateArray<char>(pathSize); // Disordered alloc/de-alloc, better to use in-built memory management (should really improve my cpu allocator)
		std::ifstream fstrm(path, std::ios::in | std::ios::binary);
		fstrm.read(&data[0], pathSize);
		fstrm.close();

		length = pathSize;
	}
	
	~LoadedShaderBytecode()
	{
		// Clear temporary memory
		CPUMemory::Free(data);
	}

	CPUMemory::ArrayAllocHandle<char> data;
	uint32_t length = 0;
};

DXWrapper::DataHandle<D3D_PSO> DXWrapper::GenerateComputePSO(const char* precompiledSrcName, DXWrapper::DataHandle<D3D_ROOTSIG> descriptors, uint32_t pipelineID)
{
	// Load shader bytecode
	LoadedShaderBytecode bytecode(precompiledSrcName);

	// Generate PSO
	D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc;
	psoDesc.pRootSignature = rootSigs[descriptors.index].Get();
	psoDesc.CS.BytecodeLength = bytecode.length;
	psoDesc.CS.pShaderBytecode = &bytecode.data[0];
	psoDesc.NodeMask = 0; // No multi-adapter support atm
	psoDesc.CachedPSO.CachedBlobSizeInBytes = 0;
	psoDesc.CachedPSO.pCachedBlob = nullptr;
	psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

	const uint32_t offs = (pipelineID * XPlatConstants::maxNumComputeShaders) + numCompute_PSOs[pipelineID];
	HRESULT hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&compute_psos[offs]));
	assert(SUCCEEDED(hr));

	// Generate handle & return
	DataHandle<D3D_PSO> psoHandle;
	psoHandle.index = numCompute_PSOs[pipelineID] + (pipelineID * XPlatConstants::maxNumComputeShaders);
	numCompute_PSOs[pipelineID]++;
	return psoHandle;
}

D3D12_COMPARISON_FUNC decodeDepthStencilComparisons(RasterSettings::DEPTH_STENCIL_TEST_TYPES test)
{
	switch (test)
	{
		case RasterSettings::DEPTH_STENCIL_TEST_TYPES::ALWAYS:
			return D3D12_COMPARISON_FUNC_ALWAYS;
		case RasterSettings::DEPTH_STENCIL_TEST_TYPES::EQUAL:
			return D3D12_COMPARISON_FUNC_EQUAL;
		case RasterSettings::DEPTH_STENCIL_TEST_TYPES::GREATER:
			return D3D12_COMPARISON_FUNC_GREATER;
		case RasterSettings::DEPTH_STENCIL_TEST_TYPES::GREATER_OR_EQUAL:
			return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		case RasterSettings::DEPTH_STENCIL_TEST_TYPES::LESS:
			return D3D12_COMPARISON_FUNC_LESS;
		case RasterSettings::DEPTH_STENCIL_TEST_TYPES::LESS_OR_EQUAL:
			return D3D12_COMPARISON_FUNC_LESS_EQUAL;
		case RasterSettings::DEPTH_STENCIL_TEST_TYPES::NEVER:
			return D3D12_COMPARISON_FUNC_NEVER;
		case RasterSettings::DEPTH_STENCIL_TEST_TYPES::NOT_EQUAL:
			return D3D12_COMPARISON_FUNC_NOT_EQUAL;
		default:
			assert(false); // Something went weirdly wrong, unsupported test type
			return D3D12_COMPARISON_FUNC_NEVER; // Fallback/debug depth/stencil type
	}
}

D3D12_STENCIL_OP decodeStencilOp(RasterSettings::STENCIL_OP_TYPES op)
{
	switch (op)
	{
		case RasterSettings::STENCIL_OP_TYPES::STENCIL_OP_KEEP:
			return D3D12_STENCIL_OP_KEEP;
		case RasterSettings::STENCIL_OP_TYPES::STENCIL_OP_ZERO:
			return D3D12_STENCIL_OP_ZERO;
		case RasterSettings::STENCIL_OP_TYPES::STENCIL_OP_INCREMENT_CLAMPED:
			return D3D12_STENCIL_OP_INCR_SAT;
		case RasterSettings::STENCIL_OP_TYPES::STENCIL_OP_DECREMENT_CLAMPED:
			return D3D12_STENCIL_OP_DECR_SAT;
		case RasterSettings::STENCIL_OP_TYPES::STENCIL_OP_INVERT:
			return D3D12_STENCIL_OP_INVERT;
		case RasterSettings::STENCIL_OP_TYPES::STENCIL_OP_INCREMENT_WRAPPED:
			return D3D12_STENCIL_OP_INCR;
		case RasterSettings::STENCIL_OP_TYPES::STENCIL_OP_DECREMENT_WRAPPED:
			return D3D12_STENCIL_OP_DECR;
		default:
			assert(false); // Something went weirdly wrong, unsupported stencil operation
			return D3D12_STENCIL_OP_ZERO; // Fallback/debug setting
	}
}

uint32_t getTextureFormatSize(DXGI_FORMAT fmt)
{
	switch (fmt)
	{
		case DXGI_FORMAT_R8_UINT:
			return 1;

		case DXGI_FORMAT_R16_UINT:
		case DXGI_FORMAT_R16_SINT:
		case DXGI_FORMAT_R16_FLOAT:
		case DXGI_FORMAT_D16_UNORM:
		case DXGI_FORMAT_R8G8_UINT:
			return 2;

		case DXGI_FORMAT_D24_UNORM_S8_UINT:
		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
		case DXGI_FORMAT_R32_UINT:
		case DXGI_FORMAT_R32_SINT:
		case DXGI_FORMAT_R32_FLOAT:
		case DXGI_FORMAT_R16G16_FLOAT:
		case DXGI_FORMAT_R16G16_UINT:
		case DXGI_FORMAT_R8G8B8A8_UINT:
		case DXGI_FORMAT_R8G8B8A8_SINT:
			return 4;

		case DXGI_FORMAT_R32G32_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_UINT:
		case DXGI_FORMAT_R32G32_UINT:
		case DXGI_FORMAT_R32G32_SINT:
			return 8;

		case DXGI_FORMAT_R32G32B32_SINT:
		case DXGI_FORMAT_R32G32B32_UINT:
			return 12;

		case DXGI_FORMAT_R32G32B32A32_UINT:
		case DXGI_FORMAT_R32G32B32A32_SINT:
			return 16;

		default:
			assert(false); // Either not a valid texture format (e.g. DXGI_FORMAT_UNKNOWN), or a format not yet supported by StandardResrcFmts, StandardIBufferFmts, or StandardDepthStencilFormats
			return 0;
	}
}

uint32_t DXWrapper::GetMaxMSAAQualityLevelForTexture(StandardResrcFmts fmt, uint32_t expectedSampleCount)
{
	D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS qualityLevels;
	qualityLevels.Format = DecodeSandboxStdFormats(fmt);
	qualityLevels.SampleCount = expectedSampleCount;
	qualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVEL_FLAGS::D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE; // No tiled-resource support in DXRSandbox atm
	qualityLevels.NumQualityLevels = 0; // Expect this to be overwritten by the adapter
	device->CheckFeatureSupport(D3D12_FEATURE::D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &qualityLevels, sizeof(qualityLevels));
	return qualityLevels.NumQualityLevels;
}

DXWrapper::DataHandle<D3D_PSO> DXWrapper::GenerateGraphicsPSO(const char* precompiledVtxName, const char* precompiledPixelName, RasterSettings rasterSettings, DXWrapper::RasterBindlist rasterBindlist, DataHandle<D3D_RASTER_INPUT_LAYOUT> ilayout, DataHandle<D3D_ROOTSIG> descriptors, uint32_t pipelineID)
{
	// Load vertex & pixel bytecode blobs (other stages unsupported atm)
	LoadedShaderBytecode bytecodeVS(precompiledVtxName);
	LoadedShaderBytecode bytecodePS(precompiledPixelName);

	// Generate graphics PSO
	////////////////////////

	const uint32_t mainPSO_offs = (pipelineID * XPlatConstants::maxNumGfxShaders) + numGFX_PSOs[pipelineID];

	const uint32_t supportedRenderTargets = std::min(rasterBindlist.numRenderTargets, XPlatConstants::maxNumRenderTargetsPerPipeline());
	const bool directBBufDraw = supportedRenderTargets == 0;

	const uint32_t numPSOsNeeded = directBBufDraw ? XPlatConstants::numBackBuffers : 1;
	for (uint32_t pso_ndx = 0; pso_ndx < numPSOsNeeded; pso_ndx++)
	{
		// Initial raster setup
		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
		psoDesc.pRootSignature = rootSigs[descriptors.index].Get();

		psoDesc.VS.BytecodeLength = bytecodeVS.length;
		psoDesc.VS.pShaderBytecode = &bytecodeVS.data[0];

		psoDesc.PS.BytecodeLength = bytecodePS.length;
		psoDesc.PS.pShaderBytecode = &bytecodePS.data[0];

		psoDesc.DS.BytecodeLength = 0;
		psoDesc.DS.pShaderBytecode = nullptr;

		psoDesc.HS.BytecodeLength = 0;
		psoDesc.HS.pShaderBytecode = nullptr;

		psoDesc.GS.BytecodeLength = 0;
		psoDesc.GS.pShaderBytecode = nullptr;

		psoDesc.StreamOutput = {}; // Stream-output unsupported

		// Raster blending disabled for now, given DXR features (might be enabled for future experiments)
		psoDesc.BlendState.AlphaToCoverageEnable = false;
		psoDesc.BlendState.IndependentBlendEnable = false;
		
		for (uint32_t i = 0; i < XPlatConstants::maxNumRenderTargetsPerPipeline(); i++)
		{
			psoDesc.BlendState.RenderTarget[i].BlendEnable = false;
			psoDesc.BlendState.RenderTarget[i].LogicOpEnable = false;
			psoDesc.BlendState.RenderTarget[i].SrcBlend = D3D12_BLEND_ONE;
			psoDesc.BlendState.RenderTarget[i].DestBlend = D3D12_BLEND_ZERO;
			psoDesc.BlendState.RenderTarget[i].BlendOp = D3D12_BLEND_OP_ADD;
			psoDesc.BlendState.RenderTarget[i].SrcBlendAlpha = D3D12_BLEND_ZERO;
			psoDesc.BlendState.RenderTarget[i].DestBlendAlpha = D3D12_BLEND_ZERO;
			psoDesc.BlendState.RenderTarget[i].BlendOpAlpha = D3D12_BLEND_OP_ADD;
			psoDesc.BlendState.RenderTarget[i].LogicOp = D3D12_LOGIC_OP_NOOP;
			psoDesc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		}

		psoDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK; // No blending, so no sample mask
		psoDesc.RasterizerState.FillMode = rasterSettings.coreRaster.fillMode == RasterSettings::FILL_SOLID ? D3D12_FILL_MODE_SOLID : D3D12_FILL_MODE_WIREFRAME;
		psoDesc.RasterizerState.CullMode = rasterSettings.coreRaster.cullMode == RasterSettings::CULL_BACK ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_FRONT;
		psoDesc.RasterizerState.FrontCounterClockwise = rasterSettings.coreRaster.windMode == RasterSettings::WINDING_MODE::WIND_CCW;
		psoDesc.RasterizerState.DepthClipEnable = rasterSettings.coreRaster.clipDistant;
		psoDesc.RasterizerState.DepthBias = 0;
		psoDesc.RasterizerState.DepthBiasClamp = 0;
		psoDesc.RasterizerState.SlopeScaledDepthBias = 0;
		psoDesc.RasterizerState.MultisampleEnable = rasterSettings.msaaSettings.enabled;
		psoDesc.RasterizerState.AntialiasedLineEnable = rasterSettings.msaaSettings.enabled && (rasterSettings.coreRaster.fillMode == RasterSettings::FILL_SOLID);
		psoDesc.RasterizerState.ForcedSampleCount = rasterSettings.msaaSettings.forcedSamples;
		psoDesc.RasterizerState.ConservativeRaster = rasterSettings.coreRaster.conservativeRaster ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

		// Depth info; tempting to zero this out (we don't need depth info), but super unwise since performance without depth culling will be mega slow
		psoDesc.DepthStencilState.DepthEnable = rasterSettings.depth.enabled;
		psoDesc.DepthStencilState.DepthWriteMask = rasterSettings.depth.enabled ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
		psoDesc.DepthStencilState.DepthFunc = decodeDepthStencilComparisons(rasterSettings.depth.depthTest);
		psoDesc.DepthStencilState.StencilEnable = rasterSettings.stencil.enabled;
		psoDesc.DepthStencilState.StencilReadMask = rasterSettings.stencil.stencilReadMask;
		psoDesc.DepthStencilState.StencilWriteMask = rasterSettings.stencil.stencilWriteMask;

		// Stencil setup
		////////////////

		// Decide which face will get the null stencil vs the regular one
		if (rasterSettings.coreRaster.cullMode == RasterSettings::CULL_BACK)
		{
			// Null stencilling on backfaces
			psoDesc.DepthStencilState.BackFace = nullStencil;

			// Decode stencilling for frontfaces
			psoDesc.DepthStencilState.FrontFace.StencilFailOp = decodeStencilOp(rasterSettings.stencil.stencilOpDesc.stencilFailOp);
			psoDesc.DepthStencilState.FrontFace.StencilDepthFailOp = decodeStencilOp(rasterSettings.stencil.stencilOpDesc.depthFailStencilPassOp);
			psoDesc.DepthStencilState.FrontFace.StencilPassOp = decodeStencilOp(rasterSettings.stencil.stencilOpDesc.stencilPassOp);
			psoDesc.DepthStencilState.FrontFace.StencilFunc = decodeDepthStencilComparisons(rasterSettings.stencil.stencilOpDesc.stencilTest);
		}
		else
		{
			// Null stencilling on frontfaces
			psoDesc.DepthStencilState.FrontFace = nullStencil;

			// Decode stencilling for backfaces
			psoDesc.DepthStencilState.BackFace.StencilFailOp = decodeStencilOp(rasterSettings.stencil.stencilOpDesc.stencilFailOp);
			psoDesc.DepthStencilState.BackFace.StencilDepthFailOp = decodeStencilOp(rasterSettings.stencil.stencilOpDesc.depthFailStencilPassOp);
			psoDesc.DepthStencilState.BackFace.StencilPassOp = decodeStencilOp(rasterSettings.stencil.stencilOpDesc.stencilPassOp);
			psoDesc.DepthStencilState.BackFace.StencilFunc = decodeDepthStencilComparisons(rasterSettings.stencil.stencilOpDesc.stencilTest);
		}

		// Input vertex/geometry info
		psoDesc.InputLayout = rasterInputLayouts[ilayout.index].GetDesc();
		psoDesc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED; // No strip cuts in index buffers (yet, possible I'll need them later for disconnected meshes)
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; // Good old triangles ^_^
		// May want to make this dynamic in the future

		// Render-target info, depth-stencil format
		memset(psoDesc.RTVFormats, DXGI_FORMAT_UNKNOWN, sizeof(DXGI_FORMAT) * XPlatConstants::maxNumRenderTargetsPerPipeline()); // All undefined render-target views must use DXGI_FORMAT_UNKNOWN

		if (!directBBufDraw)
		{
			psoDesc.NumRenderTargets = supportedRenderTargets;
			for (uint32_t i = 0; i < supportedRenderTargets; i++) // No more than eight render-targets on D3D12
			{
				psoDesc.RTVFormats[i] = textureFmts[rasterBindlist.renderTargets[i].index];
			}
		}
		else // Graphics draws need at least one target - if none given, create a view over the swapchain just-in-time
		{
			psoDesc.NumRenderTargets = 1;
			psoDesc.RTVFormats[0] = swapChainFmt;

			ID3D12Resource* swapTexPtr = nullptr;
			swapChain->GetBuffer(pso_ndx, IID_PPV_ARGS(&swapTexPtr));

			GenerateRenderTargetView(swapTexPtr, pipelineID, psoDesc.RTVFormats[0]);

			writesToBackBuffer[pipelineID] = true;
		}
		psoDesc.DSVFormat = textureFmts[rasterBindlist.depthStencilTexture.index];
		psoDesc.SampleDesc.Count = rasterSettings.msaaSettings.enabled ? rasterSettings.msaaSettings.expectedSamples : 1;
		psoDesc.SampleDesc.Quality = rasterSettings.msaaSettings.enabled ? rasterSettings.msaaSettings.qualityTier : 0;
		psoDesc.NodeMask = 0; // No multi-gpu support atm

		// PSO caching - unsupported atm
		psoDesc.CachedPSO.CachedBlobSizeInBytes = 0;
		psoDesc.CachedPSO.pCachedBlob = nullptr;

		// Flags
		psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE; // No pipeline state flags atm (maybe debug eventually)

		// Pipeline-state creation
		device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&gfx_psos[mainPSO_offs].psos[pso_ndx]));
	}

	gfx_topologies[mainPSO_offs] = D3D12_PRIMITIVE_TOPOLOGY::D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST; // Ok-ish starting assumption; possible we'd want to make this dynamic in the future

	// Generate handle and return (zzzz)
	DataHandle<D3D_PSO> psoHandle;
	psoHandle.index = mainPSO_offs;
	numGFX_PSOs[pipelineID]++;
	return psoHandle;
}

DXWrapper::DataHandle<D3D_PSO> DXWrapper::GenerateRayPSO(const char* precompiledEffectName, const wchar_t* raygenStageName, const wchar_t* closestHitStageName, const wchar_t* missStageName, uint32_t maxShaderAttributeByteSize, uint32_t maxRayPayloadByteSize, uint32_t recursionDepth, DataHandle<D3D_ROOTSIG> descriptors, uint32_t pipelineID)
{
	// Following the Microsoft sample, here:
	// https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/Samples/Desktop/D3D12Raytracing/src/D3D12RaytracingHelloWorld/D3D12RaytracingHelloWorld.cpp

	// Subobjects @_@
	// Composed of...
	// - DXIL library combining hard shader code
	// - Triangle hit group (=> a shader table)
	// - Shader config (payload & vertex attribute sizes go here)
	// - Per-shader & per-dispatch root signatures (<@>_<@>)
	// 	 - Not just resources! Per-dispatch root signatures enable the DX12 interpretation of "shader tables" as well
	// 	   Likely something to look at when I'm closer to first frames
	// - Pipeline config (just setting recursion depth atm)

	enum RTPSO_SUBOBJECTS
	{
		CODE_LIBRARY,
		HIT_GROUP,
		ROOT_SIG,
		SHADER_CONFIG,
		PIPELINE_CONFIG,
		NUM_SUBOBJECTS
	};


	CPUMemory::ArrayAllocHandle<D3D12_STATE_SUBOBJECT> subobjects = CPUMemory::AllocateArray<D3D12_STATE_SUBOBJECT>(RTPSO_SUBOBJECTS::NUM_SUBOBJECTS);

	// State-bundle setup
	D3D12_STATE_OBJECT_DESC rtDispatchState;
	rtDispatchState.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
	rtDispatchState.NumSubobjects = RTPSO_SUBOBJECTS::NUM_SUBOBJECTS;
	rtDispatchState.pSubobjects = &subobjects[0];

	// Loading in ray-tracing library
	// (this architecture is so different to raster or compute aaaa)
	LoadedShaderBytecode lib = LoadedShaderBytecode(precompiledEffectName);
	subobjects[CODE_LIBRARY].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
	auto dxilDesc = (D3D12_DXIL_LIBRARY_DESC*)subobjects[CODE_LIBRARY].pDesc;
	dxilDesc->DXILLibrary.BytecodeLength = lib.length;
	dxilDesc->DXILLibrary.pShaderBytecode = &lib.data[0];

	// Preparing linker markup
	dxilDesc->NumExports = 3; // Raygen, closest-hit, miss

	CPUMemory::ArrayAllocHandle<D3D12_EXPORT_DESC> exports = CPUMemory::AllocateArray<D3D12_EXPORT_DESC>(dxilDesc->NumExports);

	exports[0].ExportToRename = NULL;
	exports[0].Name = raygenStageName;
	exports[0].Flags = D3D12_EXPORT_FLAG_NONE;

	exports[1].ExportToRename = NULL;
	exports[1].Name = closestHitStageName;
	exports[1].Flags = D3D12_EXPORT_FLAG_NONE;

	exports[2].ExportToRename = NULL;
	exports[2].Name = missStageName;
	exports[2].Flags = D3D12_EXPORT_FLAG_NONE;

	dxilDesc->pExports = &*exports; // This is guaranteed to produce address corruption when the data moves ^_^' need to use a wrapper type

	// More linker markup :D
	// (hit group set-up now, technically different from the dxil exports we marked up before)
	// Basically those DXIL exports were us telling DX there were a few different entrypoints in our ray-tracing effect that we want to make visible to programs consuming the library
	// DX recognises some of them automatically (miss & raygen), but the subset of rays invoked during & after intersections need to be laid out explicitly
	// We don't support intersection or any-hit stages, so we just need to plug our closest-hit stage in here
	subobjects[HIT_GROUP].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
	auto hitGroupDesc = (D3D12_HIT_GROUP_DESC*)subobjects[HIT_GROUP].pDesc;
	hitGroupDesc->ClosestHitShaderImport = closestHitStageName;
	hitGroupDesc->AnyHitShaderImport = NULL;
	hitGroupDesc->IntersectionShaderImport = NULL;
	hitGroupDesc->HitGroupExport = L"dxrSandboxHitgroup"; // Suspect this is unnecessary, but helpful for debugging
	hitGroupDesc->Type = D3D12_HIT_GROUP_TYPE::D3D12_HIT_GROUP_TYPE_TRIANGLES; // No intersection shaders for now

	// Local root signatures/shader tables
	// ...Not imnplemented atm...

	// Onto the pipeline's core root signature
	// Not doing per-stage resources so don't need to think about that atm
	// ...but I should do them, because that's where shader tables come in, and I wanted to learn those as part of my process implementing this ^_^'
	// Want to get through PSO setup + work submission + asset loading + my first frame for now
	subobjects[ROOT_SIG].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
	auto globalRootSigDesc = (D3D12_GLOBAL_ROOT_SIGNATURE*)subobjects[ROOT_SIG].pDesc;
	globalRootSigDesc->pGlobalRootSignature = rootSigs[descriptors.index].Get();

	// Shader config (payload & attribute sizes)
	subobjects[SHADER_CONFIG].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
	auto shaderConfDesc = (D3D12_RAYTRACING_SHADER_CONFIG*)subobjects[SHADER_CONFIG].pDesc;
	shaderConfDesc->MaxAttributeSizeInBytes = maxShaderAttributeByteSize;
	shaderConfDesc->MaxPayloadSizeInBytes = maxRayPayloadByteSize;

	// Pipeline config!
	// Last stage, just setting recursion dpeth
	subobjects[PIPELINE_CONFIG].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
	auto pipeConfDesc = (D3D12_RAYTRACING_PIPELINE_CONFIG*)subobjects[PIPELINE_CONFIG].pDesc;
	pipeConfDesc->MaxTraceRecursionDepth = recursionDepth;

	// aaaaaaaaaaaaaaaaaaaaaaaaa
	const uint32_t offs = (pipelineID * XPlatConstants::maxNumRaytracingShaders) + numRT_PSOs[pipelineID];
	device->CreateStateObject(&rtDispatchState, IID_PPV_ARGS(&rt_psos[offs]));

	// Return handle
	DataHandle<D3D_PSO> handle;
	handle.index = numRT_PSOs[pipelineID];
	numRT_PSOs[pipelineID]++;
	return handle;
}

// AS scratch resources have to be allocated on the default heap so they can be referenced by the default-heap-only BLAS/TLAS (yeahh big gross & inconsistent, need to research)
// AS resources in general have very different requirements to generic resources, so make them take a different path to regular [PlaceResource] calls
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

enum class AS_ALLOC_OPTIONS
{
	SCRATCH,
	BLAS,
	TLAS
};

ComPtr<ID3D12Resource> AllocASResource(D3D12_RESOURCE_DESC desc, D3D12_RESOURCE_STATES initState, D3D12_CLEAR_VALUE* clearVal, uint64_t resrcFootprint, AS_ALLOC_OPTIONS asAllocSettings, uint32_t resrcOffset)
{
	switch (asAllocSettings)
	{
		// Temporary placed resource (placed into tmpResrcPool)
		case AS_ALLOC_OPTIONS::SCRATCH:
		{
			device->CreatePlacedResource(resourceHeaps[GPU_ONLY_HEAP].Get(), heapOffsets[GPU_ONLY_HEAP], &desc, initState, clearVal, IID_PPV_ARGS(&tmpResrcPool[numTmpResources]));
			return tmpResrcPool[numTmpResources];
		}

		// Regular placed default resources ^_^
		case AS_ALLOC_OPTIONS::BLAS:
		{
			device->CreatePlacedResource(resourceHeaps[UPLOAD_HEAP].Get(), heapOffsets[UPLOAD_HEAP], &desc, initState, clearVal, IID_PPV_ARGS(&resources[resrcOffset].resrc));
			return resources[resrcOffset].resrc;
		}
		case AS_ALLOC_OPTIONS::TLAS:
		{
			device->CreatePlacedResource(resourceHeaps[UPLOAD_HEAP].Get(), heapOffsets[UPLOAD_HEAP], &desc, initState, clearVal, IID_PPV_ARGS(&resources[resrcOffset + 1].resrc)); // TLASes are always allocated adjacent to BLASes
			return resources[resrcOffset + 1].resrc;
		}

		// Shouldn't be reachable, something very borky if we get here
		default:
		{
			assert(false);
			DebugBreak();
			return resources[resrcOffset].resrc; // Garbage return value
			break;
		}
	}
}

uint64_t AlignResrcFootprint(uint64_t footprint, uint64_t alignment)
{
	const uint64_t alignOffset = alignment - (footprint % alignment);
	return footprint + alignOffset;
}

void PlaceResource(D3D12_RESOURCE_DESC desc, D3D12_RESOURCE_STATES initState, D3D12_CLEAR_VALUE* clearVal, uint64_t resrcFootprint, bool cbuffer, CPUMemory::ArrayAllocHandle<uint8_t> srcData, uint32_t resrcOffset)
{
	// Following:
	// https://docs.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createplacedresource

	// CBuffers always go on the upload heap (& stay there), regardless of whether they're initialized from CPU or not
	if (cbuffer)
	{
		device->CreatePlacedResource(resourceHeaps[UPLOAD_HEAP].Get(), heapOffsets[UPLOAD_HEAP], &desc, initState, clearVal, IID_PPV_ARGS(&resources[resrcOffset].resrc));
		heapOffsets[UPLOAD_HEAP] += AlignResrcFootprint(resrcFootprint, desc.Alignment);
	}

	if (srcData.handle != CPUMemory::emptyAllocHandle)
	{
		// Prepare read/write ranges
		D3D12_RANGE readRange;
		readRange.Begin = 0;
		readRange.End = 0;

		D3D12_RANGE writeRange;
		writeRange.Begin = 0;
		writeRange.End = static_cast<SIZE_T>(resrcFootprint);

		// Generate, schedule copy to GPU, then move to the next temp resource (if non-null source data & not a constant buffer)
		if (!cbuffer)
		{
			// Branching these to handle the following:
			// D3D12 ERROR: ID3D12Device::CreatePlacedResource: A texture resource cannot be created on a D3D12_HEAP_TYPE_UPLOAD or D3D12_HEAP_TYPE_READBACK heap. Investigate CopyTextureRegion to copy texture data in CPU accessible buffers, or investigate D3D12_HEAP_TYPE_CUSTOM and WriteToSubresource for UMA adapter optimizations. [ STATE_CREATION ERROR #638: CREATERESOURCEANDHEAP_INVALIDHEAPPROPERTIES]
			if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
			{
				// Create linear buffer on staging heap
				const auto _inDesc = desc; // Cache flags, we have to zero them for temp allocs
				desc.Flags = D3D12_RESOURCE_FLAG_NONE;
				desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				desc.Format = DXGI_FORMAT_UNKNOWN;
				desc.DepthOrArraySize = 1;
				desc.Width = resrcFootprint;
				desc.Height = 1;
				desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				device->CreatePlacedResource(resourceHeaps[UPLOAD_HEAP].Get(), heapOffsets[UPLOAD_HEAP], &desc, D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_GENERIC_READ, clearVal, IID_PPV_ARGS(&tmpResrcPool[numTmpResources]));
				memcpy(&desc, &_inDesc, sizeof(decltype(desc)));

				// Upload source data
				void* memMap = nullptr;
				tmpResrcPool[numTmpResources]->Map(0, &readRange, &memMap);
				CPUMemory::CopyData(srcData, memMap);
				tmpResrcPool[numTmpResources]->Unmap(0, &writeRange);

				// Create on download heap
				device->CreatePlacedResource(resourceHeaps[GPU_ONLY_HEAP].Get(), heapOffsets[GPU_ONLY_HEAP], &desc, D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_COPY_DEST, clearVal, IID_PPV_ARGS(&resources[resrcOffset].resrc));

				D3D12_TEXTURE_COPY_LOCATION copyDest;
				copyDest.PlacedFootprint = {};
				copyDest.pResource = resources[resrcOffset].resrc.Get();
				copyDest.SubresourceIndex = 0;
				copyDest.Type = D3D12_TEXTURE_COPY_TYPE::D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

				D3D12_TEXTURE_COPY_LOCATION copySrc;
				copySrc.PlacedFootprint.Footprint.Depth = static_cast<UINT>(desc.DepthOrArraySize);
				copySrc.PlacedFootprint.Footprint.Width = static_cast<UINT>(desc.Width);
				copySrc.PlacedFootprint.Footprint.Height = static_cast<UINT>(desc.Height);
				copySrc.PlacedFootprint.Footprint.Format = desc.Format;
				copySrc.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(desc.Width * getTextureFormatSize(desc.Format));
				copySrc.PlacedFootprint.Offset = 0; // No support for mips/other subresources atm
				copySrc.pResource = tmpResrcPool[numTmpResources].Get();
				copySrc.SubresourceIndex = 0;
				copySrc.Type = D3D12_TEXTURE_COPY_TYPE::D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

				D3D12_BOX srcBox;
				srcBox.front = 0;
				srcBox.back = 1;
				srcBox.top = 0;
				srcBox.bottom = 1;
				srcBox.left = 0;
				srcBox.right = static_cast<UINT>(desc.Width);

				// Copy record
				bgCmdList->CopyTextureRegion(&copyDest, 0, 0, 0, &copySrc, &srcBox);
			}
			else if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
			{
				// Create on upload heap
				// Somewhat concermed and considering using committed resources here - temp data on placed resources could quickly cause fragmentation
				const auto resrcFlags = desc.Flags; // Cache flags, we have to zero them for temp allocs
				desc.Flags = D3D12_RESOURCE_FLAG_NONE;
				HRESULT success = device->CreatePlacedResource(resourceHeaps[UPLOAD_HEAP].Get(), heapOffsets[UPLOAD_HEAP], &desc, D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_GENERIC_READ, clearVal, IID_PPV_ARGS(&tmpResrcPool[numTmpResources]));
				assert(success == S_OK);

				memcpy(&desc.Flags, &resrcFlags, sizeof(decltype(desc.Flags)));

				// Upload source data
				void* memMap = nullptr;
				tmpResrcPool[numTmpResources]->Map(0, &readRange, &memMap);
				CPUMemory::CopyData(srcData, memMap);
				tmpResrcPool[numTmpResources]->Unmap(0, &writeRange);

				// Create on download heap
				success = device->CreatePlacedResource(resourceHeaps[GPU_ONLY_HEAP].Get(), heapOffsets[GPU_ONLY_HEAP], &desc, D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_COPY_DEST, clearVal, IID_PPV_ARGS(&resources[resrcOffset].resrc));
				assert(success == S_OK);

				// Copy from upload to download
				bgCmdList->CopyBufferRegion(resources[resrcOffset].resrc.Get(), 0, tmpResrcPool[numTmpResources].Get(), 0, resrcFootprint);
				//bgCmdList->CopyResource(resources[resrcOffset].resrc.Get(), tmpResrcPool[numTmpResources].Get());
			}

			// Transition the GPU resource back to its correct initial-state
			D3D12_RESOURCE_BARRIER barrier;
			barrier.Transition.pResource = resources[resrcOffset].resrc.Get();
			barrier.Transition.StateAfter = initState;
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_COPY_DEST;
			barrier.Transition.Subresource = 0;
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE::D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
			bgCmdList->ResourceBarrier(1, &barrier);

			// Increment heap offsets
			const uint64_t alignedFootprint = AlignResrcFootprint(resrcFootprint, desc.Alignment);
			heapOffsets[UPLOAD_HEAP] += alignedFootprint;
			heapOffsets[GPU_ONLY_HEAP] += alignedFootprint;

			// Select the next available short-term resource, assert if we've used too many
			assert(numTmpResources != maxTmpResources);
			numTmpResources++;
		}
		else
		{
			// If non-null cbuffer source data, map -> upload -> unmap
			// No persistent mapping because I don't want to think about GPU concurrency/scheduling
			// Following "Simple Usage Models" here
			// https://docs.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-map
			if (srcData.handle != CPUMemory::emptyAllocHandle)
			{
				void* memMap = nullptr;
				resources[resrcOffset].resrc->Map(0, &readRange, &memMap);
				CPUMemory::CopyData(srcData, memMap);
				resources[resrcOffset].resrc->Unmap(0, &writeRange);
			}
		}

		// Uploaded source data means these resources were initialized, so we don't need to clear them later
		resources[resrcOffset].initialized = true;
	}
	else if (!cbuffer)
	{
		// Create directly on the gpu-only heap
		device->CreatePlacedResource(resourceHeaps[GPU_ONLY_HEAP].Get(), heapOffsets[GPU_ONLY_HEAP], &desc, initState, clearVal, IID_PPV_ARGS(&resources[resrcOffset].resrc));
		heapOffsets[GPU_ONLY_HEAP] += AlignResrcFootprint(resrcFootprint, desc.Alignment);
	}
}

D3D12_RESOURCE_FLAGS DecodeGenericAccessPermissions(GPUResrcPermSetGeneric permissions)
{
	const bool srvAccess = permissions & GENERIC_RESRC_ACCESS_DIRECT_READS;
	const bool uavAccess = permissions & GENERIC_RESRC_ACCESS_DIRECT_WRITES;
	if (srvAccess && uavAccess)
	{
		return D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	}
	else if (uavAccess)
	{
		return D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
	}
	else
	{
		return D3D12_RESOURCE_FLAG_NONE;
	}
}

D3D12_RESOURCE_FLAGS DecodeTextureAccessPermissions(GPUResrcPermSetTextures permissions)
{
	const bool srvAccess = permissions & TEXTURE_ACCESS_DIRECT_READS;
	const bool uavAccess = permissions & TEXTURE_ACCESS_DIRECT_WRITES;
	const bool renderTargetAccess = permissions & TEXTURE_ACCESS_AS_RENDER_TARGET;
	const bool depthStencilAccess = permissions & TEXTURE_ACCESS_AS_DEPTH_STENCIL;
	const bool rasterAccess = renderTargetAccess || depthStencilAccess;
	D3D12_RESOURCE_FLAGS flags = {};
	if (uavAccess || !srvAccess || rasterAccess)
	{
		if (uavAccess)
		{
			flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		}

		// D3D12 forbids blocking SRV bindings for non-depth-stencil resources

		if (renderTargetAccess)
		{
			flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		}
		
		if (depthStencilAccess)
		{
			flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

			if (!srvAccess)
			{
				flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
			}
		}
		
		return flags;
	}
	else
	{
		return D3D12_RESOURCE_FLAG_NONE;
	}
}

DXWrapper::DataHandle<D3D_CBUFFER> DXWrapper::GenerateConstantBuffer(uint32_t footprint, GPUResrcPermSetGeneric permissions, CPUMemory::ArrayAllocHandle<uint8_t> srcData, uint32_t pipelineID)
{
	// Fill-out resource description
	D3D12_RESOURCE_DESC resrcDesc;
	resrcDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	resrcDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
	resrcDesc.Width = AlignResrcFootprint(footprint, 256);
	resrcDesc.Height = 1;
	resrcDesc.DepthOrArraySize = 1; // No support for array resources atm
	resrcDesc.MipLevels = 1;
	resrcDesc.Format = DXGI_FORMAT_UNKNOWN;
	resrcDesc.SampleDesc.Count = 1; // No multisampling for buffers
	resrcDesc.SampleDesc.Quality = 0;
	resrcDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	resrcDesc.Flags = DecodeGenericAccessPermissions(permissions);

	// Create resource
	// Force GENERIC_READ resource state rather than VERTEX_AND_CONSTANT_BUFFER, since we place cbuffers in the upload heap for convenience
	// (which only accepts D3D12_RESOURCE_STATE_GENERIC_READ for regular, non-reserved resources)
	const uint32_t resrcOffset = (pipelineID * XPlatConstants::maxResourcesPerPipeline) + numResources[pipelineID];
	PlaceResource(resrcDesc, D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, resrcDesc.Width, true, srcData, resrcOffset);
	resources[resrcOffset].curr_variant = ResourceViews::CBUFFER;
	resources[resrcOffset].is_variant_supported[(uint32_t)ResourceViews::CBUFFER] = true;
	cbufferStrides[resrcOffset] = static_cast<uint32_t>(resrcDesc.Width);

	// Return handle
	DataHandle<D3D_CBUFFER> handle;
	handle.index = resrcOffset;
	numResources[pipelineID]++;
	return handle;
}

DXWrapper::DataHandle<D3D_STRUCTBUFFER> DXWrapper::GenerateStructuredBuffer(uint32_t footprint, uint32_t stride, uint32_t numElements, GPUResrcPermSetGeneric accessSettings, CPUMemory::ArrayAllocHandle<uint8_t> srcData, uint32_t pipelineID)
{
	// Structured buffers are assumed to be accessible through UAVs (as in D3D11)
	// May not be true, see RWStructuredBuffer vs StructuredBuffer decls in HLSL
	// Something to experiment with in future, maybe
	assert(accessSettings & GENERIC_RESRC_ACCESS_DIRECT_WRITES);

	// Fill-out resource description
	D3D12_RESOURCE_DESC resrcDesc;
	resrcDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	resrcDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
	resrcDesc.Width = footprint;
	resrcDesc.Height = 1;
	resrcDesc.DepthOrArraySize = 1; // No support for array resources atm
	resrcDesc.MipLevels = 1;
	resrcDesc.Format = DXGI_FORMAT_UNKNOWN;
	resrcDesc.SampleDesc.Count = 1; // No multisampling for buffers
	resrcDesc.SampleDesc.Quality = 0;
	resrcDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	resrcDesc.Flags = DecodeGenericAccessPermissions(accessSettings);

	// Create resource
	const uint32_t resrcOffset = (pipelineID * XPlatConstants::maxResourcesPerPipeline) + numResources[pipelineID];
	PlaceResource(resrcDesc, D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, footprint, false, srcData, resrcOffset);
	resources[resrcOffset].curr_variant = ResourceViews::STRUCTBUFFER_RW;
	resources[resrcOffset].is_variant_supported[(uint32_t)ResourceViews::STRUCTBUFFER_RW] = true;

	structBufferData[resrcOffset].stride = stride;
	structBufferData[resrcOffset].eltCount = numElements;

	// Return handle
	DataHandle<D3D_STRUCTBUFFER> handle;
	handle.index = resrcOffset;
	numResources[pipelineID]++;
	return handle;
}

DXWrapper::DataHandle<D3D_TEXTURE> DXWrapper::GenerateStandardTexture(uint32_t width, uint32_t height, StandardResrcFmts fmt, RasterSettings::MSAASettings msaa, GPUResrcPermSetTextures accessSettings, TextureViews textureVariant, CPUMemory::ArrayAllocHandle<uint8_t> srcData, uint32_t pipelineID)
{
	// Verification!
	// Require texture views to support corresponding permissions
	if (textureVariant == TextureViews::DIRECT_WRITE)
	{
		assert(("Direct-write/UAV resource requested without write permissions", accessSettings & GPU_RESRC_ACCESS_PERMISSIONS_TEXTURES::TEXTURE_ACCESS_DIRECT_WRITES));
	}
	else if (textureVariant == TextureViews::SUPPORTS_SAMPLING)
	{
		assert(("Sampled resource rquested without read permissions", accessSettings & GPU_RESRC_ACCESS_PERMISSIONS_TEXTURES::TEXTURE_ACCESS_DIRECT_READS));
	}
	else if (textureVariant == TextureViews::STAGING)
	{
		assert(("Only copies supported for staging resources", accessSettings == GPU_RESRC_ACCESS_PERMISSIONS_TEXTURES::TEXTURE_ACCESS_COPIES_ONLY));
	}
	else if (textureVariant == TextureViews::RENDER_TARGET)
	{
		assert(("Render-target resource requested without render-target permissions", accessSettings == GPU_RESRC_ACCESS_PERMISSIONS_TEXTURES::TEXTURE_ACCESS_AS_RENDER_TARGET));
	}
	else if (textureVariant == TextureViews::DEPTH_STENCIL)
	{
		assert(("Depth-stencil resource requested without depth-stencil permissions", accessSettings == GPU_RESRC_ACCESS_PERMISSIONS_TEXTURES::TEXTURE_ACCESS_AS_DEPTH_STENCIL));
	}

	// Fill-out resource description
	D3D12_RESOURCE_DESC resrcDesc;
	resrcDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resrcDesc.Alignment = msaa.enabled ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT; // Avoiding small alignment (4KB) to keep things simplez
	resrcDesc.Width = width;
	resrcDesc.Height = height;
	resrcDesc.DepthOrArraySize = 1; // No support for array resources atm
	resrcDesc.MipLevels = 1; // No support for mipmaps atm
	resrcDesc.Format = DecodeSandboxStdFormats(fmt);
	resrcDesc.SampleDesc.Count = msaa.forcedSamples > 0 ? msaa.forcedSamples : msaa.expectedSamples;
	resrcDesc.SampleDesc.Quality = msaa.qualityTier;
	resrcDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; // Easier to work with this than other layouts - not a production-grade engine, we prefer flexibility to hyper-fast performance
	resrcDesc.Flags = DecodeTextureAccessPermissions(accessSettings);

	// Compute texture footprint + initial resource state
	// Little sus about footprint math
	const uint64_t footprint = device->GetResourceAllocationInfo(0, 1, &resrcDesc).SizeInBytes;

	// General state calculations, appropriate for depth/stencil, SRV, UAV, and render-target textures
	// Staging textures can't be accessed directly from CPU or GPU, so their only valid initial state is copy-dest
	D3D12_RESOURCE_STATES initState = textureVariant == TextureViews::DIRECT_WRITE ? D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_UNORDERED_ACCESS :
									  textureVariant == TextureViews::SUPPORTS_SAMPLING ? D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE :
									  textureVariant == TextureViews::RENDER_TARGET ? D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_RENDER_TARGET :
									  /*textureVariant == TextureVariants::TEXTURE_STAGING ? */ D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_COPY_DEST;

	// Create resource
	const uint32_t resrcOffset = (pipelineID * XPlatConstants::maxResourcesPerPipeline) + numResources[pipelineID];
	PlaceResource(resrcDesc, initState, nullptr, footprint, false, srcData, resrcOffset);
	resources[resrcOffset].curr_variant = textureVariant == TextureViews::DIRECT_WRITE ? ResourceViews::TEXTURE_DIRECT_WRITE :
										   textureVariant == TextureViews::SUPPORTS_SAMPLING ? ResourceViews::TEXTURE_SUPPORTS_SAMPLING :
										   textureVariant == TextureViews::RENDER_TARGET ? ResourceViews::TEXTURE_RENDER_TARGET :
						        		   /*textureVariant == TextureVariants::TEXTURE_STAGING ? */ ResourceViews::TEXTURE_STAGING;
	resources[resrcOffset].is_variant_supported[(uint32_t)resources[resrcOffset].curr_variant] = true;
	textureFmts[resrcOffset] = resrcDesc.Format;

	// Render-targets are bound directly onto the output-merger stage and avoid descriptor tables/the root signature, so immediately create their views/bindpoints here
	if (textureVariant == TextureViews::RENDER_TARGET)
	{
		GenerateRenderTargetView(resources[resrcOffset].resrc, pipelineID, textureFmts[resrcOffset]);
	}

	// Return handle
	DataHandle<D3D_TEXTURE> handle;
	handle.index = resrcOffset;
	numResources[pipelineID]++;
	return handle;
}

DXWrapper::DataHandle<D3D_TEXTURE> DXWrapper::GenerateDepthStencilTexture(uint32_t width, uint32_t height, StandardDepthStencilFormats fmt, RasterSettings::MSAASettings msaa, GPUResrcPermSetTextures accessSettings, CPUMemory::ArrayAllocHandle<uint8_t> srcData, uint32_t pipelineID)
{
	assert(accessSettings & TEXTURE_ACCESS_AS_DEPTH_STENCIL); // Depth stencil textures must support depth-stencil accesses ^_^'

	// Fill-out resource description
	D3D12_RESOURCE_DESC resrcDesc;
	resrcDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resrcDesc.Alignment = msaa.enabled ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT; // Avoiding small alignment (4KB) to keep things simplez
	resrcDesc.Width = width;
	resrcDesc.Height = height;
	resrcDesc.DepthOrArraySize = 1; // No support for array resources atm
	resrcDesc.MipLevels = 1; // No support for mipmaps atm
	resrcDesc.Format = DecodeSandboxDepthStencilFormats(fmt);
	resrcDesc.SampleDesc.Count = msaa.forcedSamples > 0 ? msaa.forcedSamples : msaa.expectedSamples;
	resrcDesc.SampleDesc.Quality = msaa.qualityTier;
	resrcDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	resrcDesc.Flags = DecodeTextureAccessPermissions(accessSettings);

	// Create resource
	const uint32_t resrcOffset = (pipelineID * XPlatConstants::maxResourcesPerPipeline) + numResources[pipelineID];
	const uint64_t footprint = resrcDesc.Width * resrcDesc.Height * resrcDesc.SampleDesc.Count * getTextureFormatSize(resrcDesc.Format);
	PlaceResource(resrcDesc, D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_DEPTH_WRITE /* Don't *think* I'd ever want to initialize a resource as depth-read, but...tbh idek, I might want to revisit this */, nullptr, footprint, false, srcData, resrcOffset);
	resources[resrcOffset].curr_variant = ResourceViews::TEXTURE_DEPTH_STENCIL;
	resources[resrcOffset].is_variant_supported[(uint32_t)ResourceViews::TEXTURE_DEPTH_STENCIL] = true;
	textureFmts[resrcOffset] = resrcDesc.Format;

	// Create view (depth-stencil resources aren't bound through 
	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc;
	dsvDesc.Format = resrcDesc.Format;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dsvDesc.Texture2D.MipSlice = 0; // Not using mip-slices atm
	dsvDesc.Flags = D3D12_DSV_FLAG_NONE; // No need to mask off depth/stencil yet; avoiding managing that complexity for now
	device->CreateDepthStencilView(resources[resrcOffset].resrc.Get(), &dsvDesc, dsvDescriptorPtrs[pipelineID]);

	// Return handle
	DataHandle<D3D_TEXTURE> handle;
	handle.index = resrcOffset;
	numResources[pipelineID]++;
	return handle;
}

DXWrapper::DataHandle<D3D_IBUFFER> DXWrapper::GenerateIndexBuffer(uint32_t footprint, StandardIBufferFmts fmt, GPUResrcPermSetGeneric accessSettings, CPUMemory::ArrayAllocHandle<uint8_t> srcData, uint32_t pipelineID)
{
	// Fill-out resource description
	D3D12_RESOURCE_DESC resrcDesc;
	resrcDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	resrcDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
	resrcDesc.Width = footprint;
	resrcDesc.Height = 1;
	resrcDesc.DepthOrArraySize = 1; // No support for array resources atm
	resrcDesc.MipLevels = 1;
	resrcDesc.Format = DXGI_FORMAT_UNKNOWN; // Resource format has to be UNKNOWN for all buffers; but we can set standard formats in the view :p
	resrcDesc.SampleDesc.Count = 1; // No multisampling for buffers
	resrcDesc.SampleDesc.Quality = 0;
	resrcDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	resrcDesc.Flags = DecodeGenericAccessPermissions(accessSettings);

	// Create resource
	const uint32_t resrcOffset = (pipelineID * XPlatConstants::maxResourcesPerPipeline) + numResources[pipelineID];
	PlaceResource(resrcDesc, D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_INDEX_BUFFER, nullptr, footprint, false, srcData, resrcOffset);
	resources[resrcOffset].curr_variant = ResourceViews::IBUFFER;
	resources[resrcOffset].is_variant_supported[(uint32_t)ResourceViews::IBUFFER] = true;

	// Prepare handle
	DataHandle<D3D_IBUFFER> handle;
	handle.index = resrcOffset;
	numResources[pipelineID]++;

	// Index buffers are bound directly onto the output-merger stage and avoid descriptor tables/the root signature, so immediately create their views/bindpoints here
	D3D12_INDEX_BUFFER_VIEW ibvDesc;
	ibvDesc.BufferLocation = GetGPUAddress(handle);
	ibvDesc.SizeInBytes = footprint;
	ibvDesc.Format = DecodeSandboxIBufferFormats(fmt);
	indexBufferDescriptors[pipelineID] = ibvDesc;

	return handle;
}

DXWrapper::DataHandle<D3D_VBUFFER> DXWrapper::GenerateVertexBuffer(uint32_t footprint, uint32_t stride, uint32_t numElts, StandardResrcFmts* eltFmts, GPUResrcPermSetGeneric accessSettings, CPUMemory::ArrayAllocHandle<uint8_t> srcData, uint32_t pipelineID)
{
	// Fill-out resource description
	D3D12_RESOURCE_DESC resrcDesc;
	resrcDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	resrcDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
	resrcDesc.Width = footprint;
	resrcDesc.Height = 1;
	resrcDesc.DepthOrArraySize = 1; // No support for array resources atm
	resrcDesc.MipLevels = 1;
	resrcDesc.Format = DXGI_FORMAT_UNKNOWN;
	resrcDesc.SampleDesc.Count = 1; // No multisampling for buffers
	resrcDesc.SampleDesc.Quality = 0;
	resrcDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	resrcDesc.Flags = DecodeGenericAccessPermissions(accessSettings);

	// Create resource
	const uint32_t resrcOffset = (pipelineID * XPlatConstants::maxResourcesPerPipeline) + numResources[pipelineID];
	PlaceResource(resrcDesc, D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, nullptr, footprint, false, srcData, resrcOffset);
	resources[resrcOffset].curr_variant = ResourceViews::VBUFFER;
	resources[resrcOffset].is_variant_supported[(uint32_t)ResourceViews::VBUFFER] = true;

	// Resolve vbuffer handle
	DataHandle<D3D_VBUFFER> handle;
	handle.index = resrcOffset;

	// Vertex buffers are bound directly onto the output-merger stage and avoid descriptor tables/the root signature, so immediately create their views/bindpoints here
	D3D12_VERTEX_BUFFER_VIEW vbvDesc;
	vbvDesc.BufferLocation = GetGPUAddress(handle);
	vbvDesc.SizeInBytes = footprint;
	vbvDesc.StrideInBytes = stride;
	vertexBufferDescriptors[pipelineID] = vbvDesc;

	// Some vertex-buffer properties are needed for AS generation and might be spoofed if they were passed through arguments - cache those here
	vbufferEltCountsPerVert[handle.index] = numElts;
	for (uint32_t i = 0; i < numElts; i++)
	{
		vbufferEltFmtsPerVert[handle.index].fmts[i] = DecodeSandboxStdFormats(eltFmts[i]);
	}

	// Increment resources, return
	numResources[pipelineID]++;
	return handle;
}

// Very different to regular resource setup; need to plug-in IBO & VBO resource handles, and allocate as a write-allowed buffer (even though we bind as read-only - might need a transition there)
// See: https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/Samples/Desktop/D3D12Raytracing/src/D3D12RaytracingHelloWorld/D3D12RaytracingHelloWorld.cpp
void DXWrapper::GenerateAccelStructForGeometry(DataHandle<D3D_VBUFFER> vbufHandle, DataHandle<D3D_IBUFFER>* ibufHandle, DataHandle<D3D_ACCELSTRUCT_BLAS>* blasOut, DataHandle<D3D_ACCELSTRUCT_TLAS>* tlasOut, GPUResrcPermSetGeneric accessSettings, XPlatUtils::AccelStructConfig asConfig, uint32_t pipelineID)
{
	// Validate input geometry
	//////////////////////////

	// Input vertices are required to be position/padding only (so just one element)
	assert(vbufferEltCountsPerVert[vbufHandle.index] == 1);

	// Supported vbuffer/ibuffer formats found here (assumes target devices support raytracing tier 1.1):
	// https://docs.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_raytracing_geometry_triangles_desc
	DXGI_FORMAT vbufFmt = vbufferEltFmtsPerVert[vbufHandle.index].fmts[0];
	assert(vbufFmt == DXGI_FORMAT_R32G32_FLOAT      || vbufFmt == DXGI_FORMAT_R32G32B32_FLOAT    || vbufFmt == DXGI_FORMAT_R16G16_FLOAT		  || vbufFmt == DXGI_FORMAT_R16G16B16A16_FLOAT ||
		   vbufFmt == DXGI_FORMAT_R16G16_SNORM      || vbufFmt == DXGI_FORMAT_R16G16B16A16_SNORM || vbufFmt == DXGI_FORMAT_R16G16B16A16_UNORM || vbufFmt == DXGI_FORMAT_R16G16_UNORM	   ||
		   vbufFmt == DXGI_FORMAT_R10G10B10A2_UNORM || vbufFmt == DXGI_FORMAT_R8G8B8A8_UNORM     || vbufFmt == DXGI_FORMAT_R8G8_UNORM		  || vbufFmt == DXGI_FORMAT_R8G8B8A8_SNORM	   ||
		   vbufFmt == DXGI_FORMAT_R8G8_SNORM);

	const bool ibufSet = ibufHandle != nullptr;
	DXGI_FORMAT ibufFmt = DXGI_FORMAT_UNKNOWN;
	if (ibufSet)
	{
		ibufFmt = indexBufferDescriptors[pipelineID].Format;

		// DXGI_FORMAT_UNKNOWN is reserved for AS's without associated index buffers (i.e. its an only a valid setting when [ibufHandle] is [nullptr])
		assert(ibufFmt == DXGI_FORMAT_R32_UINT || ibufFmt == DXGI_FORMAT_R16_UINT || ibufFmt == DXGI_FORMAT_R32_SINT || ibufFmt == DXGI_FORMAT_R32_UINT);
	}

	// Fill-out geometry description
	D3D12_RAYTRACING_GEOMETRY_DESC geoDesc = {};
	geoDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES; // No AABBs/experimental intersection shaders for us, yet - might experiment with them later
	geoDesc.Triangles.IndexBuffer = ibufSet ? GetGPUAddress(*ibufHandle) : NULL;
	geoDesc.Triangles.IndexCount = ibufSet ? indexBufferDescriptors[pipelineID].SizeInBytes / getTextureFormatSize(ibufFmt) : NULL;
	geoDesc.Triangles.IndexFormat = ibufFmt; // Nullity here resolved during validation (see above)
	geoDesc.Triangles.Transform3x4 = NULL; // Too much hassle to implement this, leaving it untouched & asking consumers to use alternative workflows instead
	geoDesc.Triangles.VertexFormat = vbufFmt;
	geoDesc.Triangles.VertexCount = vertexBufferDescriptors[pipelineID].SizeInBytes / vertexBufferDescriptors[pipelineID].StrideInBytes;
	geoDesc.Triangles.VertexBuffer.StartAddress = GetGPUAddress(vbufHandle);
	geoDesc.Triangles.VertexBuffer.StrideInBytes = vertexBufferDescriptors[pipelineID].StrideInBytes;
	geoDesc.Flags = !asConfig.hasCutouts ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION; // Masking duplicate AnyHits seems like a reasonable option to set in the background for cutout AS's

	// Resolve build flags
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS buildFlags = (asConfig.minimal_footprint ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE) |
																	 (asConfig.updatable ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE) |
																     (asConfig.perfPriority == XPlatUtils::AccelStructConfig::AS_PERF_PRIORITY::FAST_BUILD ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD :
																																							 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE);

	// Describe top/bottom level AS inputs
	//////////////////////////////////////

	// Top-level inputs don't contain geometry of their own, and instead act as containers for bottom-level acceleration structures
	// I believe the value of [numDescs] indicates the number of BLAS to associate with each top-level AS
	// Somewhat sus about this logic in general - should ask CG stackexchange
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS topLevelInputs = {};
	topLevelInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	topLevelInputs.Flags = buildFlags;
	topLevelInputs.NumDescs = 1;
	topLevelInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;

	// Bottom-level inputs copy top-level inputs, but with the bottom-level type specified + defined geometry
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottomLevelInputs = topLevelInputs;
	bottomLevelInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
	bottomLevelInputs.pGeometryDescs = &geoDesc;

	// Resolve top/bottom-level build contexts
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfos[2];
	device->GetRaytracingAccelerationStructurePrebuildInfo(&topLevelInputs, &prebuildInfos[0]);
	device->GetRaytracingAccelerationStructurePrebuildInfo(&bottomLevelInputs, &prebuildInfos[1]);
	assert(prebuildInfos[0].ResultDataMaxSizeInBytes == 0); // Nonzero result sizes indicate an error
	assert(prebuildInfos[1].ResultDataMaxSizeInBytes == 0);

	// Allocate AS resources, in the order scratch -> BLAS -> TLAS
	// (implemented in [AllocASResource(...)])
	//////////////////////////////////////////////////////////////

	D3D12_RESOURCE_DESC asResrcDesc;
	asResrcDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	asResrcDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
	asResrcDesc.Width = std::max(prebuildInfos[0].ScratchDataSizeInBytes, prebuildInfos[1].ScratchDataSizeInBytes);
	asResrcDesc.Height = 1;
	asResrcDesc.DepthOrArraySize = 1;
	asResrcDesc.MipLevels = 1;
	asResrcDesc.Format = DXGI_FORMAT_UNKNOWN;
	asResrcDesc.SampleDesc.Count = 1;
	asResrcDesc.SampleDesc.Quality = NULL;
	asResrcDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	asResrcDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	auto asResrcFootprint = [](uint64_t bytes, uint64_t alignment) { return bytes + (alignment - (bytes % alignment)); };
	const auto& scratchResrc = AllocASResource(asResrcDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, asResrcFootprint(asResrcDesc.Width, asResrcDesc.Alignment), AS_ALLOC_OPTIONS::SCRATCH, 0); // Writes into temp data, not pipeline resources

	asResrcDesc.Width = prebuildInfos[0].ResultDataMaxSizeInBytes;

	uint32_t blasNdx = (pipelineID * XPlatConstants::maxResourcesPerPipeline) + numResources[pipelineID];
	resources[blasNdx].resrc = AllocASResource(asResrcDesc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, asResrcFootprint(asResrcDesc.Width, asResrcDesc.Alignment), AS_ALLOC_OPTIONS::TLAS, blasNdx);
	resources[blasNdx].curr_variant = ResourceViews::RT_ACCEL_STRUCTURE;
	resources[blasNdx].is_variant_supported[(int32_t)ResourceViews::RT_ACCEL_STRUCTURE] = true;
	resources[blasNdx].rt_settings = RT_BLAS;

	asResrcDesc.Width = prebuildInfos[1].ResultDataMaxSizeInBytes;
	uint32_t tlasNdx = blasNdx + 1;
	resources[tlasNdx].resrc = AllocASResource(asResrcDesc, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, asResrcFootprint(asResrcDesc.Width, asResrcDesc.Alignment), AS_ALLOC_OPTIONS::BLAS, tlasNdx);
	resources[tlasNdx].curr_variant = ResourceViews::RT_ACCEL_STRUCTURE;
	resources[tlasNdx].is_variant_supported[(int32_t)ResourceViews::RT_ACCEL_STRUCTURE] = true;
	resources[tlasNdx].rt_settings = RT_TLAS;

	// Fill-out descriptions for bottom/top-level acceleration structures
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blasBuildDesc, tlasBuildDesc;
	blasBuildDesc.Inputs = bottomLevelInputs;
	blasBuildDesc.ScratchAccelerationStructureData = scratchResrc->GetGPUVirtualAddress();
	blasBuildDesc.DestAccelerationStructureData = resources[blasNdx].resrc->GetGPUVirtualAddress();

	tlasBuildDesc.Inputs = topLevelInputs;
	tlasBuildDesc.ScratchAccelerationStructureData = scratchResrc->GetGPUVirtualAddress();
	tlasBuildDesc.DestAccelerationStructureData = resources[tlasNdx].resrc->GetGPUVirtualAddress();

	// Fire-off [::BuildRayTracingAccelerationStructure] calls on the background cmd-list with the descriptions we filled-out before
	bgCmdList->BuildRaytracingAccelerationStructure(&blasBuildDesc, 0, nullptr);

	// Not sure how important this barrier is
	D3D12_RESOURCE_BARRIER uavBarrier = {};
	uavBarrier.UAV.pResource = resources[tlasNdx].resrc.Get();
	bgCmdList->ResourceBarrier(1, &uavBarrier);

	// Passed our bottom-level set-up, resolve the top-level AS now
	bgCmdList->BuildRaytracingAccelerationStructure(&tlasBuildDesc, 0, nullptr);

	// Export handles
	blasOut->index = blasNdx;
	tlasOut->index = tlasNdx;
	numResources[pipelineID] = tlasNdx + 1;
}

DXWrapper::DataHandle<D3D_CMD_LIST> DXWrapper::CreateCmdList(wchar_t* label)
{
	// Guard against out-of-bounds reads/writes
	if (numCmdLists < XPlatConstants::maxNumPipelines)
	{
		// Create a command list in the next available slot
		device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(cmdAllocators[numCmdLists]), (void**)&cmdAllocators[numCmdLists]);
		device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAllocators[numCmdLists].Get(), nullptr, __uuidof(cmdLists[numCmdLists]), (void**)&cmdLists[numCmdLists]);
		
		cmdLists[numCmdLists]->Close();
		cmdLists[numCmdLists]->Reset(cmdAllocators[numCmdLists].Get(), nullptr);
		cmdLists[numCmdLists]->SetName(label);
		
		cmdListsOpen[numCmdLists] = true;
	}
	else
	{
		DebugBreak(); // Something's gone wrong if we get here ^_^'
	}

	// Return a handle to the generated command list
	DataHandle<D3D_CMD_LIST> h;
	h.index = numCmdLists;
	numCmdLists++;
	return h;
}

void DXWrapper::BindComputeResources(DataHandle<D3D_CMD_LIST> pipe_work, DataHandle<D3D_ROOTSIG> rootSig, uint8_t pipelineID)
{
	const ComPtr<ID3D12GraphicsCommandList>& cmdList = cmdLists[pipe_work.index];
	cmdList->SetComputeRootSignature(rootSigs[rootSig.index].Get());
	
	ID3D12DescriptorHeap* _descriptorHeaps[1] = { descriptorHeaps[pipelineID].genericResrcViews.Get() };//, descriptorHeaps[pipelineID].samplerViews.Get() };
												  //descriptorHeaps[pipelineID].depthStencilViews.Get(), descriptorHeaps[pipelineID].renderTargetViews.Get() };

	cmdList->SetDescriptorHeaps(1, _descriptorHeaps);
	cmdList->SetComputeRootDescriptorTable(0, descriptorHeaps[pipelineID].genericResrcViews->GetGPUDescriptorHandleForHeapStart());
}

bool dirtyBackBuffer = false;
void DXWrapper::BindGFX_Resources(DataHandle<D3D_CMD_LIST> pipe_work, DataHandle<D3D_ROOTSIG> rootSig, uint8_t pipelineID)
{
	// Set GFX root signature
	const ComPtr<ID3D12GraphicsCommandList>& cmdList = cmdLists[pipe_work.index];
	cmdList->SetGraphicsRootSignature(rootSigs[rootSig.index].Get());

	// Set GFX descriptors (CBV/SRV/UAV)
	ID3D12DescriptorHeap* _descriptorHeaps[1] = { descriptorHeaps[pipelineID].genericResrcViews.Get(), 
												  /* descriptorHeaps[pipelineID].samplerViews.Get(),
												  descriptorHeaps[pipelineID].depthStencilViews.Get(), 
												  descriptorHeaps[pipelineID].renderTargetViews.Get()*/ };

	cmdList->SetDescriptorHeaps(1, _descriptorHeaps);
	cmdList->SetGraphicsRootDescriptorTable(0, descriptorHeaps[pipelineID].genericResrcViews->GetGPUDescriptorHandleForHeapStart());

	// If drawing to the backbuffer, transition its resource to RENDER_TARGET before drawing/binding
	const bool backBufferDraw = writesToBackBuffer[pipelineID];
	if (backBufferDraw)
	{
		D3D12_RESOURCE_BARRIER transition;
		transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;

		swapChain->GetBuffer(currBackBuffer, IID_PPV_ARGS(&transition.Transition.pResource));

		transition.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		transition.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		transition.Transition.Subresource = 0;

		transition.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;

		cmdList->ResourceBarrier(1, &transition);

		dirtyBackBuffer = true;
	}

	// Bind render-target views & depth-stencil views to the output-merger stage
	D3D12_CPU_DESCRIPTOR_HANDLE cpuRTV_Ptr = descriptorHeaps[pipelineID].renderTargetViews->GetCPUDescriptorHandleForHeapStart();
	D3D12_CPU_DESCRIPTOR_HANDLE cpuDSV_Ptr = descriptorHeaps[pipelineID].depthStencilViews->GetCPUDescriptorHandleForHeapStart();
	
	if (backBufferDraw)
	{
		// We assume for simplicity that presentation/final BBuff draws only render to a single target
		// Open to changing this in the future, but don't think that restriction should affect content *too* much
		// (MRT draws can & possibly should be moved to earlier phases in each pipeline)
		cpuRTV_Ptr.ptr += currBackBuffer * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		cmdList->OMSetRenderTargets(1, &cpuRTV_Ptr, TRUE, &cpuDSV_Ptr);
	}
	else
	{
		cmdList->OMSetRenderTargets(numRTV_Descriptors[pipelineID], &cpuRTV_Ptr, TRUE, &cpuDSV_Ptr);
	}

	// Bind geometry to the input assembler
	cmdList->IASetIndexBuffer(&indexBufferDescriptors[pipelineID]);
	cmdList->IASetVertexBuffers(0, 1, &vertexBufferDescriptors[pipelineID]); // Keeping to one VB per scene - no reason to take on the complexity of supporting multiple, yet
}

void DXWrapper::SubmitComputeExec(DataHandle<D3D_CMD_LIST> pipe_work, uint32_t dispX, uint32_t dispY, uint32_t dispZ, DataHandle<D3D_PSO> pso)
{
	const ComPtr<ID3D12GraphicsCommandList>& cmdList = cmdLists[pipe_work.index];
	cmdList->SetPipelineState(compute_psos[pso.index].Get());
	cmdList->Dispatch(dispX, dispY, dispZ);
}

void DXWrapper::SubmitGraphicsExec(DataHandle<D3D_CMD_LIST> work, uint32_t numNdces, DataHandle<D3D_PSO> pso, uint8_t pipelineID)
{
	const ComPtr<ID3D12GraphicsCommandList>& cmdList = cmdLists[work.index];
	if (writesToBackBuffer[pipelineID])
	{
		cmdList->SetPipelineState(gfx_psos[pso.index].psos[currBackBuffer].Get());
	}
	else
	{
		cmdList->SetPipelineState(gfx_psos[pso.index].psos[0].Get());
	}
	cmdList->RSSetViewports(1, &viewport);
	cmdList->RSSetScissorRects(1, &scissor);
	cmdList->IASetPrimitiveTopology(gfx_topologies[pso.index]);
	cmdList->DrawIndexedInstanced(numNdces, 1, 0, 0, 0); // No instancing support - too hard, we lazy
														 // (& software/simulated instancing could be faster, maybe)
}

void DXWrapper::CloseCmdList(DataHandle<D3D_CMD_LIST> cmds)
{
	cmdLists[cmds.index]->Close();
	cmdListsOpen[cmds.index] = false;
}

void DXWrapper::ResetCmdList(DataHandle<D3D_CMD_LIST> cmds)
{
	if (!cmdListsOpen[cmds.index])
	{
		cmdAllocators[cmds.index]->Reset();
	}
	else
	{
		cmdLists[cmds.index]->Close(); // We can't reset allocators on open command lists, and we can't/shouldn't close cmdlists that are closed already
		cmdAllocators[cmds.index]->Reset();
	}

	cmdLists[cmds.index]->Reset(cmdAllocators[cmds.index].Get(), nullptr);
	cmdListsOpen[cmds.index] = true;
}

void GPUSync()
{
	gfxQueue->Signal(syncGPU.Get(), 1);

	// We love busy spinners ^_^
	// Double-buffered work submission, eventually...
	while (syncGPU->GetCompletedValue() != 1)
	{
		/* Could do useful work here maybe */
	}

	syncGPU->Signal(0); // Reset fence value after syncs (shitty single-buffer sync for now)
}

static uint32_t numPipesIssued = 0;

void DXWrapper::IssueWork(DataHandle<D3D_CMD_LIST> work, bool issueSynchronous, uint8_t pipelineID) // Suspect sequential ExecuteCommandLists(...) calls with different cmdlists are automatically synchronized - removing this flag eventually
{
	// Clear uninitialized depth-stencils/render-targets before submitting GPU work
	for (uint32_t i = 0; i < XPlatConstants::maxResourcesPerPipeline; i++)
	{
		uint32_t ndx = i * pipelineID;
		ResourceViews currVariant = resources[i].curr_variant;
		if (resources[ndx].resrc != nullptr && !resources[i].initialized)
		{
			if (currVariant == ResourceViews::TEXTURE_DEPTH_STENCIL)
			{
				bgCmdList->ClearDepthStencilView(dsvDescriptorPtrs[depthStencilDescriptors[ndx].index], D3D12_CLEAR_FLAGS::D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAGS::D3D12_CLEAR_FLAG_STENCIL, 0, 0, 0, nullptr);
			}
			else if (currVariant == ResourceViews::TEXTURE_RENDER_TARGET)
			{
				FLOAT clearColour[4] = { 1.0f, 0.5f, 0.25f, 1.0f }; // Debug orange <3
				bgCmdList->ClearRenderTargetView(rtvDescriptorPtrs[depthStencilDescriptors[ndx].index], clearColour, 0, nullptr);
			}
		}
	}

	// Submit any pending background commands + transfer the given work-block to the gpu, then close/reset the background command-list
	bgCmdList->Close();

	ID3D12CommandList* cmds[2] = { bgCmdList.Get(), cmdLists[work.index].Get() };
	gfxQueue->ExecuteCommandLists(2, cmds);

	bgCmdList->Reset(bgCmdAlloc.Get(), nullptr);

	numPipesIssued++;
}

void DXWrapper::PresentLastFrame()
{
	// Sneakily transition the back-buffer back to PRESENT if we drew to it in the current frame
	if (dirtyBackBuffer)
	{
		D3D12_RESOURCE_BARRIER transition;
		transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;

		swapChain->GetBuffer(currBackBuffer, IID_PPV_ARGS(&transition.Transition.pResource));

		transition.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		transition.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
		transition.Transition.Subresource = 0;

		transition.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;

		bgCmdList->ResourceBarrier(1, &transition);
		bgCmdList->Close();

		ID3D12CommandList* bg = bgCmdList.Get();
		gfxQueue->ExecuteCommandLists(1, &bg);
		bgCmdList->Reset(bgCmdAlloc.Get(), nullptr);

		dirtyBackBuffer = false;
	}

	// Pace ourselves so we don't spam cmdlists before they have time to execute
	// Future versions will use multiple buffering instead & only synchronize in the worst case
	// (requires annoying resource duplication & management)
	{
		GPUSync();

		// Release any temporary resources consumed since the last work issuance + reset temp-resource counters
		// We already synchronized the background cmd-list, and temporary resources should never be touched by [Pipeline]s/external cmd-lists,
		// so clearing these without a separate wait should be fine
		for (uint32_t i = 0; i < numTmpResources; i++)
		{
			if (tmpResrcPool[i])
			{
				tmpResrcPool[i].Reset();
			}
		}
		numTmpResources = 0;
	}

	swapChain->Present(1, 0);
	currBackBuffer = (currBackBuffer + 1) % XPlatConstants::numBackBuffers;
	
	bgCmdList->Close();
	bgCmdAlloc->Reset();
	bgCmdList->Reset(bgCmdAlloc.Get(), nullptr);

	numPipesIssued = 0;
}
