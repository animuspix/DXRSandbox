#pragma once

#include <array>
#include <functional>
#include "..\Pipeline.h"
#include "FrameUtilities.h"

// Very simple frame abstraction (not a frame graph!) to bucket pipelines associated with the same render modes together
// The presentation pipeline is duplicated this way, but I think that's probably better than managing the complexity of having three frames feeding into the same
// final stage
template<uint32_t numStages, uint32_t frameID>
class Frame
{
private:
	// Each pass in the frame
	Pipeline pipes[numStages];
	static constexpr uint32_t id = static_cast<uint32_t>(frameID);

private:
	PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> cbufferHandle;

	// Implemented to separate complicated type logic from core
	// frame processing
	frameTypes::Utilities<id> utilities;

	template<uint32_t _id>
	struct ResourceShareRequest
	{
		ResourceViews variant;
		uint32_t resourceID;
		uint32_t sourceStage;
		uint32_t destinationStage;

		// Resolved by Finalize() (see below), used to help recover descriptor indices for shaders
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> handleAtDestination;
	};

	// Transient resource reuse between a subset of stages (for reuse across all stages, bind as a persistent resource instead)
	ResourceShareRequest<id> resrcShareRequests[XPlatConstants::maxResourcesPerPipeline] = {};
	uint32_t numResrcShareRequests = 0;

	struct ComputeShaderDesc
	{
		const char* name;
		uint32_t stage, dispatchX, dispatchY, dispatchZ;
	};

	struct GraphicsShaderDesc
	{
		const char* nameVS;
		const char* namePS;
		uint32_t stage;
		RasterSettings rasterSettings;
	};

	ComputeShaderDesc frameComputeShaders[XPlatConstants::maxNumComputeShaders];
	uint32_t numComputeShaders;

	GraphicsShaderDesc frameGraphicsShaders[XPlatConstants::maxNumGfxShaders];
	uint32_t numGraphicsShaders;

	struct TypelessResourceReference
	{
		bool persistent;
		ResourceViews variant;
		uint32_t resourceID;
	};

	struct ClearDesc
	{
		TypelessResourceReference resource;
	};

	// Keeping frame setup simple; just full-resource copies for now
	struct CopyDesc
	{
		TypelessResourceReference source;
		TypelessResourceReference dest;

		uint32_t width, height;
	};

	struct TransitionDesc
	{
		ResourceViews sourceView;
		ResourceViews destView;

		uint32_t destStage;
		uint32_t sourceStage;
		uint32_t resourceID;

		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> handleAtDestination;
	};

	TransitionDesc transitions[XPlatConstants::maxResourcesPerPipeline] = {};

	static constexpr uint32_t eventStageSize = 32;
	ClearDesc clearEvents[eventStageSize] = {};
	CopyDesc copyEvents[eventStageSize] = {};
	frameTypes::ShaderHandle<id> execEvents[eventStageSize] = {};

	uint32_t numExecEvents, numClearEvents, numCopyEvents, numTransitionEvents;

	enum PIPELINE_EVENT_TYPE
	{
		COPY_EVENT,
		CLEAR_EVENT,
		EXEC_EVENT
	};

	struct PipelineEventOrdinal
	{
		PIPELINE_EVENT_TYPE eventType;
		uint32_t stage;
	};

	PipelineEventOrdinal eventOrderLookup[eventStageSize];
	uint32_t numEventsTotal;

public:
	// Ordered resource keys, for correct decoding on the GPU
	// (using submission order alone would be inconsistent/unpredictable)
	struct SortedResourceKeys
	{
		std::array<uint32_t, XPlatConstants::maxNumRootConstants> constants[numStages];
		uint32_t numActiveConstants[numStages];
	};

private:

	// Has this frame been resolved into GPU bindings/events?
	bool finalized = false;

	// Have the pipelines attached to this frame been baked into submittable command-lists?
	bool baked = false;

	// Cache resource keys after baking, in case we need them during frame submission
	// (possible for dynamic pipelines)
	SortedResourceKeys cachedResourceKeys;

public:
	void init(std::array<bool, numStages> stageDynamism)
	{
		numComputeShaders = 0;
		numGraphicsShaders = 0;
		numExecEvents = 0;
		numTransitionEvents = 0;
		numEventsTotal = 0;
		numResrcShareRequests = 0;

		finalized = false;
		baked = false;

		for (uint32_t i = 0; i < numStages; i++)
		{
			pipes[i].init(stageDynamism[i]);
		}

		// Simple zero-init
		memset(cachedResourceKeys.constants, 0, sizeof(cachedResourceKeys));

		utilities.Init();
	}

public:

	void RegisterCBuffer(GPUResource<ResourceViews::CBUFFER>::resrc_desc desc)
	{
		// CBuffers are always shared, so register on the first pass and copy to the others
		cbufferHandle = pipes[0].RegisterCBuffer(desc, GPU_RESRC_ACCESS_PERMISSIONS_GENERIC::GENERIC_RESRC_ACCESS_DIRECT_READS);

		for (uint32_t i = 1; i < numStages; i++)
		{
			pipes[i].RegisterCBuffer(cbufferHandle);
		}
	}

	template<ResourceViews variant>
		requires DynamicResourceView<variant>
	frameTypes::StageBindingHandle<variant, id> RegisterPerStageResource(GPUResource<variant>::resrc_desc desc, uint32_t stage, Pipeline::ResourcePermSetSelector<variant>::permissionType permissions)
	{
		static_assert(variant != ResourceViews::CBUFFER, "See RegisterCBuffer for constant buffer registration; per-stage constants are unsupported in the RHI");

		return utilities.stageBindingRequests.append<variant>(desc, permissions, stage);
	}

	template<ResourceViews variant>
		requires DynamicResourceView <variant>
	frameTypes::PersistentBindingHandle<variant, id> RegisterPersistentResource(GPUResource<variant>::resrc_desc desc, Pipeline::ResourcePermSetSelector<variant>::permissionType permissions)
	{
		static_assert(variant != ResourceViews::CBUFFER, "See RegisterCBuffer for constant buffer registration, per-stage constants are unsupported in the RHI");

		return utilities.persistentBindingRequests.append<variant>(desc, permissions);
	}

private:
	template<ResourceViews variant>
	const uint32_t StageID_FromStageBinding(const frameTypes::StageBindingHandle<variant, id>& sourceHandle) const
	{
		return sourceHandle.bindingIndex / XPlatConstants::maxResourcesPerPipeline;
	}

public:

	struct ReboundResourceHandle
	{
		uint32_t id; // Just a standard ID into the transition/resource re-use buffers, not a clever UUID like the ones on Stage/PersistentBindingHandle(s)
		uint32_t destStage;
		bool hasTransitionBarrier;
	};

	template<ResourceViews sourceView, ResourceViews destView>
	ReboundResourceHandle TransitionResource(frameTypes::StageBindingHandle<sourceView, id> sourceHandle, uint32_t destinationStage)
	{
		const uint32_t sourceStage = StageID_FromStageBinding(sourceHandle);
		TransitionDesc desc;

		desc.resourceID = sourceHandle.resourceID;
		desc.destStage = destinationStage;
		desc.sourceStage = sourceStage;
		desc.sourceView = sourceView;
		desc.destView = destView;

		ReboundResourceHandle handle = {};
		handle.id = numTransitionEvents;
		handle.hasTransitionBarrier = true;
		handle.destStage = destinationStage;

		transitions[numTransitionEvents] = desc;
		numTransitionEvents++;

		return handle;
	}

	// Share resource across stages, without associating it with the whole frame
	template<ResourceViews view>
	ReboundResourceHandle ShareResource(frameTypes::StageBindingHandle<view, id> sourceHandle, uint32_t destinationStage)
	{
		assert(numResrcShareRequests < XPlatConstants::maxResourcesPerPipeline);
		resrcShareRequests[numResrcShareRequests].view = view;
		resrcShareRequests[numResrcShareRequests].srcStageBindingIndex = sourceHandle.bindingIndex;
		resrcShareRequests[numResrcShareRequests].destinationStage = destinationStage;

		ReboundResourceHandle handle = {};
		handle.id = numResrcShareRequests;
		handle.hasTransitionBarrier = false;
		handle.destStage = destinationStage;

		numResrcShareRequests++;

		return handle;
	}

private:

	struct BindingHandle
	{
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> pipelineLink;
		ResourceViews variant;
		uint32_t uniqueRequestID;
	};

	void PersistentBindingIterator(CPUMemory::ArrayAllocHandle<BindingHandle> exportHandleStream,
		CPUMemory::ArrayAllocHandle<PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC>> pipelineHandleStream,
		CPUMemory::ArrayAllocHandle<ResourceViews> variantStream,
		CPUMemory::ArrayAllocHandle<uint32_t> idStream,
		uint32_t numPersistentRequests)
	{
		// We don't want to commit actual memory-backed objects every time we access a persistent resource;
		// instead, commit them to the first pass in the frame, record the handles, and use those to
		// declare that we're reusing the persistent resources later on

		// Persistent resources are always available from the first pass/stage onwards; that's an intentional choice to
		// keep things simple (+ possibly easier on the GPU)

		// Commit resources to the first pass
		uint32_t handleCounter = 0; // For deinterleaving across recursive calls (likely possible to remove using some kind of loop transformation - a problem for another day)
		utilities.persistentBindingRequests.buffer.FulfilAllBuffers(pipelineHandleStream, idStream, variantStream, handleCounter, pipes[0], 0);

		// Declare re-use for the others
		for (uint32_t stageWalker = 1; stageWalker < numStages; stageWalker++)
		{
			for (uint32_t i = 0; i < numPersistentRequests; i++)
			{
				pipes[stageWalker].RegisterSharedResource(pipelineHandleStream[i], variantStream[i]);
			}
		}

		// Export handles + variants + IDs for future tasks
		for (uint32_t i = 0; i < numPersistentRequests; i++)
		{
			exportHandleStream[i].pipelineLink = pipelineHandleStream[i];
			exportHandleStream[i].variant = variantStream[i];
			exportHandleStream[i].uniqueRequestID = idStream[i];
		}
	}

	void StageBindingIterator(CPUMemory::ArrayAllocHandle<BindingHandle> exportHandleStream, uint32_t handleStreamOffset,
		CPUMemory::ArrayAllocHandle<PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC>> pipelineHandleStream,
		CPUMemory::ArrayAllocHandle<ResourceViews> variantStream,
		CPUMemory::ArrayAllocHandle<uint32_t> idStream,
		uint32_t stage)
	{
		uint32_t handleCounter = 0; // See above
		utilities.stageBindingRequests.buffer.FulfilAllBuffers(pipelineHandleStream, idStream, variantStream, handleCounter, pipes[stage], stage);

		// Export handles + variants + IDs for future tasks
		for (uint32_t i = 0; i < handleCounter; i++)
		{
			exportHandleStream[handleStreamOffset + i].pipelineLink = pipelineHandleStream[i];
			exportHandleStream[handleStreamOffset + i].variant = variantStream[i];
			exportHandleStream[handleStreamOffset + i].uniqueRequestID = idStream[i];
		}
	}

	struct ShaderBindingHandle
	{
		PipelineObjectHandle<PIPELINE_OBJ_TYPES::SHADER> shader;
		uint32_t stage;
	};

public:

	void Finalize()
	{
		// Scratch memory for pipeline bindings
		const uint32_t maxNumBindings = XPlatConstants::maxResourcesPerPipeline * XPlatConstants::maxNumPipelines;
		auto pipelineHandleStream = CPUMemory::AllocateArray<PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC>>(maxNumBindings);
		auto variantStream = CPUMemory::AllocateArray<ResourceViews>(maxNumBindings);
		auto idStream = CPUMemory::AllocateArray<uint32_t>(maxNumBindings);

		// Generate persistent resources, immediately bind handles to the first stage
		// Persistent handles are discarded at the end of this scope; not 100% sure whether to keep them around (for run-time changes, like streaming), or
		// leave them as-is
		// Most likely leaving for now, I can modify to support streaming/whatever when needed

		const uint32_t numPersistentRequests = utilities.persistentBindingRequests.CountRequests();
		CPUMemory::ArrayAllocHandle<BindingHandle> persistentBindingHandles = CPUMemory::AllocateArray<BindingHandle>(numPersistentRequests);

		PersistentBindingIterator(persistentBindingHandles,
			pipelineHandleStream,
			variantStream,
			idStream,
			numPersistentRequests);

		// Iterate per-stage bindings, apply persistent bindings to every stage, insert transitions
		// Static array instead of dynamic for simpler code, the excess memory shouldn't be too painful hopefullyy
		auto perStageBindingHandles = CPUMemory::AllocateArrayStatic<BindingHandle, XPlatConstants::maxResourcesPerPipeline* XPlatConstants::maxNumPipelines>();
		uint32_t perStageBindingOffset = 0;
		uint32_t stageHandleCounts[numStages] = {};

		// Handles to inserted transition events; useful for 

		for (uint32_t stage = 0; stage < numStages; stage++)
		{
			// Persistent/per-stage bindings
			StageBindingIterator(perStageBindingHandles,
				perStageBindingOffset,
				pipelineHandleStream,
				variantStream,
				idStream,
				stage);

			const uint32_t handleStartOffset = perStageBindingOffset;
			stageHandleCounts[stage] = utilities.stageBindingRequests.CountRequestsForStage(stage);
			perStageBindingOffset += stageHandleCounts[stage];

			// Deferred transition implementation
			// Loop through views -> loop through handles
			// Need a modified handle type, containing regular pipeline handles + resource view + frame-level binding index
			for (uint32_t i = 0; i < stageHandleCounts[stage]; i++)
			{
				const BindingHandle handle = perStageBindingHandles[i + handleStartOffset];
				for (uint32_t transitionCounter = 0; transitionCounter < numTransitionEvents; transitionCounter++)
				{
					TransitionDesc& transition = transitions[transitionCounter];
					if (transition.sourceStage == stage && transition.resourceID == handle.uniqueRequestID)
					{
						transition.handleAtDestination = pipes[transition.destStage].RegisterSharedResource(handle.pipelineLink, transition.destView);
					}
				}

				// Simple reuse (transient across stages, no transition required, non-persistent; bound only for a subset of stages)
				for (uint32_t reuseCounter = 0; reuseCounter < numResrcShareRequests; reuseCounter++)
				{
					ResourceShareRequest<id>& request = resrcShareRequests[reuseCounter];

					if (request.sourceStage == stage && request.resourceID == handle.uniqueRequestID)
					{
						request.handleAtDestination = pipes[request.destinationStage].RegisterSharedResource(handle.pipelineLink, request.variant);
					}
				}
			}

			// Resources all done for each stage, resolve bindings here : )
			pipes[stage].ResolveBindings();
		}

		// Shader registration
		// Stream-out pipeline shader handles from these
		ShaderBindingHandle csBindings[XPlatConstants::maxNumComputeShaders] = {};
		ShaderBindingHandle gfxBindings[XPlatConstants::maxNumGfxShaders] = {};

		for (uint32_t csID = 0; csID < numComputeShaders; csID++)
		{
			const ComputeShaderDesc& computeShader = frameComputeShaders[csID];
			auto handle = pipes[computeShader.stage].RegisterComputeShader(computeShader.name, computeShader.dispatchX, computeShader.dispatchY, computeShader.dispatchZ);

			csBindings[csID].shader = handle;
			csBindings[csID].stage = computeShader.stage;
		}

		for (uint32_t gfxShaderID = 0; gfxShaderID < numGraphicsShaders; gfxShaderID++)
		{
			const GraphicsShaderDesc& gfxShader = frameGraphicsShaders[gfxShaderID];
			auto handle = pipes[gfxShader.stage].RegisterGraphicsShader(gfxShader.nameVS, gfxShader.namePS, gfxShader.rasterSettings);

			gfxBindings[gfxShaderID].shader = handle;
			gfxBindings[gfxShaderID].stage = gfxShader.stage;
		}

		// Copies, clears, shader issues
		uint32_t copyCounter = 0;
		uint32_t clearCounter = 0;
		uint32_t execCounter = 0;

		for (uint32_t eventID = 0; eventID < numEventsTotal; eventID++)
		{
			PipelineEventOrdinal currentEvent = eventOrderLookup[eventID];

			if (currentEvent.eventType == PIPELINE_EVENT_TYPE::COPY_EVENT)
			{
				CopyDesc copy = copyEvents[copyCounter];
				CopyEvent pipelineCopyEvent = {};

				for (uint32_t i = 0; i < numStages; i++)
				{
					for (uint32_t handleID = 0; handleID < stageHandleCounts[i]; handleID++)
					{
						const BindingHandle handle = perStageBindingHandles[i * XPlatConstants::maxResourcesPerPipeline + handleID];

						if (copy.source.resourceID == handle.uniqueRequestID && copy.source.variant == handle.variant)
						{
							pipelineCopyEvent.src = handle.pipelineLink;
							pipelineCopyEvent.copyWidth = copy.width; // Need to add a function to access the correct DecodeXXX function per-view
							pipelineCopyEvent.copyHeight = copy.height;
						}

						if (copy.dest.resourceID == handle.uniqueRequestID && copy.dest.variant == handle.variant)
						{
							pipelineCopyEvent.dst = handle.pipelineLink;
						}
					}

					for (uint32_t handleID = 0; handleID < stageHandleCounts[i]; handleID++)
					{
						const BindingHandle handle = perStageBindingHandles[i * XPlatConstants::maxResourcesPerPipeline + handleID];

						if (copy.source.resourceID == handle.uniqueRequestID && copy.source.variant == handle.variant)
						{
							pipelineCopyEvent.src = handle.pipelineLink;
							pipelineCopyEvent.copyWidth = copy.width; // Need to add a function to access the correct DecodeXXX function per-view
							pipelineCopyEvent.copyHeight = copy.height;
						}

						if (copy.dest.resourceID == handle.uniqueRequestID && copy.dest.variant == handle.variant)
						{
							pipelineCopyEvent.dst = handle.pipelineLink;
						}
					}
				}

				pipes[currentEvent.stage].AppendCopy(pipelineCopyEvent);
				copyCounter++;
			}

			// Reimplement using the same strategy as above (scan bound resources for matches to the clear desc's resource description,
			// match required pipeline event resource handle just-in-time, append the resolved clear event)

			if (currentEvent.eventType == PIPELINE_EVENT_TYPE::CLEAR_EVENT)
			{
				ClearDesc clear = clearEvents[clearCounter];
				ClearEvent pipelineEvent = {};

				pipelineEvent.clearVal = 0.0f; // Open to change in the future


				pipes[currentEvent.stage].AppendClear(pipelineEvent);
				clearCounter++;
			}

			if (currentEvent.eventType == PIPELINE_EVENT_TYPE::EXEC_EVENT)
			{
				frameTypes::ShaderHandle<id> execEvent = execEvents[execCounter];
				if (execEvent.shaderType == SHADER_TYPES::COMPUTE)
				{
					pipes[execEvent.stage].AppendComputeExec(csBindings[execEvent.shaderID].shader);
				}
				else if (execEvent.shaderType == SHADER_TYPES::GRAPHICS)
				{
					pipes[execEvent.stage].AppendGFX_Exec(gfxBindings[execEvent.shaderID].shader);
				}

				// ...etc (raytracing, mesh, whatever)d

				execCounter++;
			}
		}

		// Export persistent resource keys
		for (uint32_t i = 0; i < numPersistentRequests; i++)
		{
			const BindingHandle handle = persistentBindingHandles[i];
			const uint32_t bindNdx = pipes[0].GetBindingIndex(handle.pipelineLink);
			utilities.persistentBindingRequests.buffer.UpdateResourceKey(handle.variant, handle.uniqueRequestID, bindNdx);
		}

		// Export per-stage resource keys
		uint32_t stageBindHandleWalker = 0;
		for (uint32_t stage = 0; stage < numStages; stage++)
		{
			// Write-out per-stage resource keys, pass to pipeline root constants
			for (uint32_t i = 0; i < stageHandleCounts[stage]; i++)
			{
				// VBUFFERs report placeholder binding indices (0xffffffff)
				// Should probably change things so that vbuffers don't propagate, since we use the input assembler still?
				// Not really sure

				const BindingHandle handle = perStageBindingHandles[stageBindHandleWalker + i];
				const uint32_t bindNdx = pipes[stage].GetBindingIndex(handle.pipelineLink);
				utilities.stageBindingRequests.buffer.UpdateResourceKey(handle.variant, handle.uniqueRequestID, bindNdx);
			}

			stageBindHandleWalker += stageHandleCounts[stage];
		}

		finalized = true;

		CPUMemory::Free(persistentBindingHandles);
		CPUMemory::Free(perStageBindingHandles);
		CPUMemory::Free(pipelineHandleStream);
		CPUMemory::Free(variantStream);
		CPUMemory::Free(idStream);
	}

	template<ResourceViews variant>
	const uint32_t GetGPUKeyForPersistentBinding(frameTypes::PersistentBindingHandle<variant, id> handle) const
	{
		// Hacky message courtesy VS copilot (tyyy)
		assert(finalized && "Frame must be finalized before retrieving GPU keys for resources");

		return utilities.persistentBindingRequests.buffer.RetrieveResourceKey(variant, handle.resourceID);
	}

	template<ResourceViews variant>
	const uint32_t GetGPUKeyForStageBinding(frameTypes::StageBindingHandle<variant, id> handle) const
	{
		// Hacky message courtesy VS copilot (tyyy)
		assert(finalized && "Frame must be finalized before retrieving GPU keys for resources");

		return utilities.stageBindingRequests.buffer.RetrieveResourceKey(variant, handle.resourceID);
	}

	const uint32_t GetGPUKeyForStageRebinding(ReboundResourceHandle handle) const
	{
		assert(finalized && "Frame must be finalized before retrieving GPU keys for resources");

		if (handle.hasTransitionBarrier)
		{
			TransitionDesc transition = transitions[handle.id];
			return pipes[transition.destStage].GetBindingIndex(transition.handleAtDestination);
		}
		else
		{
			ResourceShareRequest<id> shareReq = resrcShareRequests[handle.id];
			return pipes[shareReq.destinationStage].GetBindingIndex(shareReq.handleAtDestination);
		}
	}

	const uint32_t GetGPUKeyForCBuffer() const
	{
		// Hacky message courtesy VS copilot (tyyy)
		assert(finalized && "Frame must be finalized before retrieving GPU keys for resources");

		return pipes[0].GetBindingIndex(cbufferHandle);
	}

	void BakeCmdLists(SortedResourceKeys keys)
	{
		for (uint32_t i = 0; i < numStages; i++)
		{
			pipes[i].BakeCmdList(keys.constants[i].data(), keys.numActiveConstants[i]);
		}

		// Cache-off keys, for re-use on submission
		cachedResourceKeys = keys;
		baked = true;
	}

	void UpdateCBuffer(CPUMemory::ByteSpan bytes)
	{
		// Frame compute buffer is bound to all pipelines (as a persistent resource), simple & direct to pull the reference here from the first frame,
		// since that's where it would have first been registered (Finalize() traverses frame pipes/passes in order)
		pipes[0].DecodeCBufferHandle(cbufferHandle)->UpdateData(bytes);
	}

	frameTypes::ShaderHandle<id> RegisterComputeShader(uint32_t stage, const char* name, uint32_t dispatchX, uint32_t dispatchY, uint32_t dispatchZ)
	{
		ComputeShaderDesc& deferredShader = frameComputeShaders[numComputeShaders];
		deferredShader.dispatchX = dispatchX;
		deferredShader.dispatchY = dispatchY;
		deferredShader.dispatchZ = dispatchZ;
		deferredShader.name = name;
		deferredShader.stage = stage;

		frameTypes::ShaderHandle<id> handle = frameTypes::ShaderHandle<id>(stage, numComputeShaders, SHADER_TYPES::COMPUTE);
		numComputeShaders++;
		return handle;
	}

	frameTypes::ShaderHandle<id> RegisterGraphicsShader(uint32_t stage, const char* nameVS, const char* namePS, RasterSettings rasterSettings)
	{
		GraphicsShaderDesc& deferredShader = frameGraphicsShaders[numGraphicsShaders];
		deferredShader.nameVS = nameVS;
		deferredShader.namePS = namePS;
		deferredShader.rasterSettings = rasterSettings;
		deferredShader.stage = stage;

		frameTypes::ShaderHandle<id> handle = frameTypes::ShaderHandle<id>(stage, numGraphicsShaders, SHADER_TYPES::GRAPHICS);
		numGraphicsShaders++;
		return handle;
	}

	void RegisterShaderExec(frameTypes::ShaderHandle<id> shader)
	{
		execEvents[numExecEvents] = shader;
		numExecEvents++;

		eventOrderLookup[numEventsTotal].eventType = PIPELINE_EVENT_TYPE::EXEC_EVENT;
		eventOrderLookup[numEventsTotal].stage = shader.stage;
		numEventsTotal++;
	}

	template<ResourceViews variant>
	void RegisterClear(frameTypes::StageBindingHandle<variant, id> resourceToClear)
	{
		ClearDesc clear = {};
		clear.resource.resourceIndex = resourceToClear.bindingIndex;
		clear.resource.view = variant;

		clearEvents[numClearEvents] = clear;
		numClearEvents++;

		const uint32_t sourceStage = StageID_FromBinding(resourceToClear);
		eventOrderLookup[numEventsTotal].eventType = PIPELINE_EVENT_TYPE::CLEAR_EVENT;
		eventOrderLookup[numEventsTotal].stage = sourceStage;
		numEventsTotal++;
	}

	void EnableStaticSamplers(uint32_t stage)
	{
		pipes[stage].EnableStaticSamplers();
	}

	template<ResourceViews fromVariant, ResourceViews toVariant> requires BufferView<fromVariant>&& BufferView<toVariant>
	void RegisterBufferCopy(frameTypes::StageBindingHandle<fromVariant, id> from, frameTypes::StageBindingHandle<toVariant, id> to)
	{
		// No copies from between generic resources and cbuffers (not really needed yet, complicated to verify)
		if (fromVariant == ResourceViews::CBUFFER)
		{
			assert(toVariant == ResourceViews::CBUFFER);
		}
		else
		{
			assert(toVariant != ResourceViews::CBUFFER);
		}

		auto& srcRequest = RetrieveRequestFromHandle(from);
		auto& dstRequest = RetrieveRequestFromHandle(to);

		const uint32_t srcFootprint = srcRequest.getFootprint();
		assert(srcFootprint == dstRequest.getFootprint());

		CopyDesc copy = {};
		copy.source.resourceIndex = from.bindingIndex;
		copy.source.view = fromVariant;
		copy.source.resourceID = from.resourceID;

		copy.dest.resourceIndex = to.bindingIndex;
		copy.dest.view = toVariant;

		copy.width = srcFootprint;
		copy.height = 1; // D3D convention (buffer copies use rects with height 1)

		copyEvents[numCopyEvents] = copy;
		numCopyEvents++;

		const uint32_t sourceStage = StageID_FromBinding(from);
		eventOrderLookup[numEventsTotal].eventType = PIPELINE_EVENT_TYPE::COPY_EVENT;
		eventOrderLookup[numEventsTotal].stage = sourceStage;
		numEventsTotal++;
	}

	template<ResourceViews fromVariant, ResourceViews toVariant> requires (TextureView<fromVariant>&& TextureView<toVariant>)
		void RegisterTextureCopy(frameTypes::StageBindingHandle<fromVariant, id> from, frameTypes::StageBindingHandle<toVariant, id> to)
	{
		// Similar simplification to buffer copies; whole resources only
		////////////////////////////////////////////////////////////////

		// Thought I should match copy verification to whatever I do in DXWrapper, but turns out copies aren't implemented at all ^_^'
		// oops
		// going to require same stride & same footprints as a start
		// (means that src/dest need to be ~basically the same, minus binding/view differences)

		auto& srcRequest = RetrieveRequestFromHandle(from);
		auto& dstRequest = RetrieveRequestFromHandle(to);

		assert(srcRequest.getFootprint() == dstRequest.getFootprint());
		assert(srcRequest.getStride() == dstRequest.getStride());

		CopyDesc copy = {};
		copy.source.resourceIndex = from.bindingIndex;
		copy.source.view = fromVariant;

		copy.dest.resourceIndex = to.bindingIndex;
		copy.dest.view = toVariant;

		copy.width = srcRequest.getWidth();
		copy.height = srcRequest.getHeight();

		copyEvents[numCopyEvents] = copy;
		numClearEvents++;

		const uint32_t sourceStage = StageID_FromBinding(from);
		eventOrderLookup[numEventsTotal].eventType = PIPELINE_EVENT_TYPE::COPY_EVENT;
		eventOrderLookup[numEventsTotal].stage = sourceStage;
		numEventsTotal++;
	}

	void SubmitPipes(SortedResourceKeys keys)
	{
		for (int i = 0; i < numStages; i++)
		{
			pipes[i].SubmitCmdList(false, keys.constants[i].data(), keys.numActiveConstants[i]); // No back-end implementation for synchronous CLs (besides forced every-frame synchronization implemented in DXWrapper)
		}
	}

	void SubmitPipes()
	{
		for (int i = 0; i < numStages; i++)
		{
			pipes[i].SubmitCmdList(false, cachedResourceKeys.constants[i].data(), cachedResourceKeys.numActiveConstants[i]); // No back-end implementation for synchronous CLs (besides forced every-frame synchronization implemented in DXWrapper)
		}
	}
};