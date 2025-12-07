#pragma once

#include "..\ResourceEnums.h"
#include "..\CPUMemory.h"
#include <stdint.h>

namespace frameTypes
{
	// Going to need handles to identify transitioning resources, enable resource reuse across frame types
	template<ResourceViews variant, uint32_t _id>
	struct StageBindingHandle
	{
		StageBindingHandle(uint32_t lookup, uint32_t uniqueID) : bindingIndex(lookup), resourceID(uniqueID) {}

		uint32_t bindingIndex;
		uint32_t resourceID;
		static constexpr uint32_t ownerID = _id;
		static constexpr ResourceViews view = variant;
	};

	template<ResourceViews variant, uint32_t _id>
	struct PersistentBindingHandle
	{
		PersistentBindingHandle(uint32_t lookup, uint32_t uniqueID) : bindingIndex(lookup), resourceID(uniqueID) {}

		const uint32_t bindingIndex;
		const uint32_t resourceID;
		static constexpr uint32_t ownerID = _id;
		static constexpr ResourceViews view = variant;
	};

	template<uint32_t frameID>
	class Utilities
	{
		template<ResourceViews _variant>
		struct Request
		{
			Request() {}

			typename GPUResource<_variant>::resrc_desc desc;
			static constexpr ResourceViews variant = _variant;
			Pipeline::ResourcePermSetSelector<_variant>::permissionType permissions;
			uint32_t bindID;

			PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC> Fulfil(Pipeline& pipe)
			{
				return pipe.RegisterResource(desc, permissions);
			}
		};

		static constexpr uint32_t persistentIDMask = 0xff000000;

		template<bool persistent>
		struct BindingRequestBuffer
		{
			static constexpr uint32_t maxRequests = persistent ? XPlatConstants::maxResourcesPerPipeline :
				XPlatConstants::maxResourcesPerPipeline * XPlatConstants::maxNumPipelines;

			template<ResourceViews variant>
			struct RequestBufferTyped
			{
				Request<variant> requests[maxRequests] = {};
				uint32_t		 gpuKeys[maxRequests] = {};

			private:
				static constexpr uint32_t numCounters = persistent ? 1 : XPlatConstants::maxNumPipelines;
				uint32_t counters[numCounters] = {};

				const uint32_t GetRequestLookup(uint32_t stage, uint32_t counter) const
				{
					return XPlatConstants::maxResourcesPerPipeline * stage + counter;
				}

			public:
				using Handle = std::conditional_t<persistent, PersistentBindingHandle<variant, frameID>, StageBindingHandle<variant, frameID>>;

				void init()
				{
					memset(counters, 0, sizeof(uint32_t) * numCounters);
					memset(requests, 0, sizeof(Request<variant>) * maxRequests);
				}

				const uint32_t PeekRequestCounter(uint32_t stage = 0) const
				{
					return counters[stage];
				}

				const uint32_t SumRequestCounters() const
				{
					uint32_t numRequests = 0;

					for (uint32_t i = 0; i < numCounters; i++)
					{
						numRequests += counters[i];
					}

					return numRequests;
				}

				Handle append(decltype(Request<variant>::desc) desc, decltype(Request<variant>::permissions) permissions, uint32_t bindID, uint32_t stage = 0)
				{
					const uint32_t counter = counters[stage];
					const uint32_t lookup = GetRequestLookup(stage, counter);

					requests[lookup].desc = desc;
					requests[lookup].permissions = permissions;
					requests[lookup].bindID = bindID;
					counters[stage]++;

					return { lookup, bindID };
				}

				void Fulfil(CPUMemory::ArrayAllocHandle<PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC>> handleBuffer,
					CPUMemory::ArrayAllocHandle<uint32_t> uniqueIDBuffer,
					CPUMemory::ArrayAllocHandle<ResourceViews> variantBuffer,
					uint32_t& handleFront, Pipeline& pipe, uint32_t stage = 0)
				{
					const uint32_t baseLookup = GetRequestLookup(stage, 0);

					for (uint32_t i = 0; i < counters[stage]; i++)
					{
						handleBuffer[handleFront] = requests[baseLookup + i].Fulfil(pipe);
						uniqueIDBuffer[handleFront] = requests[baseLookup + i].bindID;
						variantBuffer[handleFront] = requests[baseLookup + i].variant;
						handleFront++;
					}
				}

				void AssignGPUKey(ResourceViews expectedVariant, uint32_t uniqueRequestID, uint32_t gpuKey)
				{
					if (variant == expectedVariant)
					{
						AssignGPUKey(uniqueRequestID, gpuKey);
					}
				}

				void AssignGPUKey(uint32_t uniqueRequestID, uint32_t gpuKey)
				{
					uint32_t iter = 0;
					bool requestFound = false;

					if (persistent)
					{
						while (iter < counters[0])
						{
							//const uint32_t bindID = requests[iter].bindID ^ persistentIDMask;
							//const uint32_t requestID = uniqueRequestID ^ persistentIDMask;
							if (requests[iter].bindID == uniqueRequestID)
							{
								requestFound = true;
								gpuKeys[iter] = gpuKey;
								break;
							}

							iter++;
						}

						assert(requestFound && "Attempted to assign GPU key to non-existent persistent resource request");
					}
					else
					{
						for (uint32_t i = 0; i < numCounters; i++)
						{
							while (iter < counters[i])
							{
								const uint32_t lookup = GetRequestLookup(i, iter);
								if (requests[lookup].bindID == uniqueRequestID)
								{
									requestFound = true;
									gpuKeys[lookup] = gpuKey;
									break;
								}

								iter++;
							}

							if (requestFound) break;

							iter = 0;
						}

						assert(requestFound && "Attempted to assign GPU key to non-existent per-stage resource request");
					}
				}

				const uint32_t RetrieveGPUKey(ResourceViews expectedVariant, uint32_t uniqueRequestID) const
				{
					if (variant == expectedVariant)
					{
						return RetrieveGPUKey(uniqueRequestID);
					}
					else
					{
						return 0;
					}
				}

				const uint32_t RetrieveGPUKey(uint32_t uniqueRequestID) const
				{
					uint32_t iter = 0;
					bool requestFound = false;
					uint32_t retrievedKey = UINT32_MAX;
					if (persistent)
					{
						while (iter < counters[0])
						{
							if (requests[iter].bindID == uniqueRequestID)
							{
								requestFound = true;
								retrievedKey = gpuKeys[iter];
								break;
							}

							iter++;
						}
					}
					else
					{
						for (uint32_t i = 0; i < numCounters; i++)
						{
							while (iter < counters[i])
							{
								const uint32_t lookup = GetRequestLookup(i, iter);
								if (requests[lookup].bindID == uniqueRequestID)
								{
									requestFound = true;
									retrievedKey = gpuKeys[lookup];
									break;
								}

								iter++;
							}
							iter = 0;

							if (requestFound) break;
						}
					}

					assert(requestFound && "Attempted to retrieve GPU key for non-existent resource request");
					return retrievedKey;
				}
			};

		private:
			using BufferTuple = std::tuple<RequestBufferTyped<ResourceViews::VBUFFER>,
				RequestBufferTyped<ResourceViews::IBUFFER>,
				RequestBufferTyped<ResourceViews::STRUCTBUFFER_RW>,
				RequestBufferTyped<ResourceViews::CBUFFER>,
				RequestBufferTyped<ResourceViews::TEXTURE_DIRECT_WRITE>,
				RequestBufferTyped<ResourceViews::TEXTURE_SUPPORTS_SAMPLING>,
				RequestBufferTyped<ResourceViews::TEXTURE_RENDER_TARGET>,
				RequestBufferTyped<ResourceViews::TEXTURE_DEPTH_STENCIL>,
				RequestBufferTyped<ResourceViews::RT_ACCEL_STRUCTURE>>;

			static constexpr uint32_t maxNdx = std::tuple_size<BufferTuple>::value;

		public:

			template<ResourceViews variant>
			RequestBufferTyped<variant>& RetrieveRequestBuffer() requires DynamicResourceView<variant>
			{
				// ResourceViews don't map 1:1 to DynamicResourceView/s, or to the variants supported by perTypeBuffers
				// Should be possible to implement a [consteval] function with the corrected mapping; locally implementing an ok-ish fix for now
				static constexpr uint32_t variantCast = static_cast<uint32_t>(variant);
				return std::get<variantCast>(perTypeBuffers);
			}

			const uint32_t CountRequestsForStage(uint32_t stage = 0) const
			{
				uint32_t numRequests = 0;
				std::apply([&](auto&... bufferTypes) {
					((numRequests += bufferTypes.PeekRequestCounter(stage)), ...);
					}, perTypeBuffers);

				return numRequests;
			}

			const uint32_t CountRequestsTotal() const
			{
				uint32_t numRequests = 0;
				std::apply([&](auto&... bufferTypes) {
					((numRequests += bufferTypes.SumRequestCounters()), ...);
					}, perTypeBuffers);

				return numRequests;
			}

			void FulfilAllBuffers(CPUMemory::ArrayAllocHandle<PipelineObjectHandle<PIPELINE_OBJ_TYPES::RESRC>> handleBuffer,
				CPUMemory::ArrayAllocHandle<uint32_t> uniqueIDBuffer,
				CPUMemory::ArrayAllocHandle<ResourceViews> variantBuffer,
				uint32_t& handleFront, Pipeline& pipe, uint32_t stage = 0)
			{
				std::apply([&](auto&... bufferTypes) {
					((bufferTypes.Fulfil(handleBuffer, uniqueIDBuffer, variantBuffer, handleFront, pipe, stage)), ...);
					}, perTypeBuffers);
			}

			void UpdateResourceKey(ResourceViews variant, uint32_t uniqueRequestID, uint32_t gpuKey)
			{
				std::apply([&](auto&... bufferTypes) {
					((bufferTypes.AssignGPUKey(variant, uniqueRequestID, gpuKey)), ...);
					}, perTypeBuffers);
			}

			const uint32_t RetrieveResourceKey(ResourceViews variant, uint32_t uniqueRequestID) const
			{
				uint32_t gpuKey = 0;

				// Should probably implement as a linear search instead of this pseudo-fold, but this works for now
				std::apply([&](auto&... bufferTypes) {
					((gpuKey |= bufferTypes.RetrieveGPUKey(variant, uniqueRequestID)), ...); // Non-matching variants will return 0, only matching variant buffer will contribute
					}, perTypeBuffers);

				return gpuKey;
			}

			void init()
			{
				perTypeBuffers = std::make_tuple(RequestBufferTyped<ResourceViews::VBUFFER>(),
					RequestBufferTyped<ResourceViews::IBUFFER>(),
					RequestBufferTyped<ResourceViews::STRUCTBUFFER_RW>(),
					RequestBufferTyped<ResourceViews::CBUFFER>(),
					RequestBufferTyped<ResourceViews::TEXTURE_DIRECT_WRITE>(),
					RequestBufferTyped<ResourceViews::TEXTURE_SUPPORTS_SAMPLING>(),
					RequestBufferTyped<ResourceViews::TEXTURE_RENDER_TARGET>(),
					RequestBufferTyped<ResourceViews::TEXTURE_DEPTH_STENCIL>(),
					RequestBufferTyped<ResourceViews::RT_ACCEL_STRUCTURE>());

				std::apply([](auto&... bufferTypes) {
					((bufferTypes.init()), ...);
					}, perTypeBuffers);
			}

		private:
			BufferTuple perTypeBuffers;
		};

		using PerStageRequests = BindingRequestBuffer<false>;
		using PersistentRequests = BindingRequestBuffer<true>;

		static constexpr uint32_t maxRequestsTotal = PerStageRequests::maxRequests + PersistentRequests::maxRequests;

		struct StageBindingRequestBuffer
		{
			PerStageRequests buffer;

			void init()
			{
				buffer.init();
			}

			template<ResourceViews variant>
			StageBindingHandle<variant, frameID> append(GPUResource<variant>::resrc_desc desc, Pipeline::ResourcePermSetSelector<variant>::permissionType permissions, uint32_t stage) requires DynamicResourceView<variant>
			{
				const uint32_t requestID = (frameID * maxRequestsTotal) + CountRequestsTotal();
				return buffer.RetrieveRequestBuffer<variant>().append(desc, permissions, requestID, stage);
			}

			const uint32_t CountRequestsForStage(uint32_t stage)
			{
				return buffer.CountRequestsForStage(stage);
			}

			const uint32_t CountRequestsTotal()
			{
				return buffer.CountRequestsTotal();
			}
		};

		struct PersistentBindingRequestBuffer
		{
			PersistentRequests buffer;

			void init()
			{
				buffer.init();
			}

			static constexpr uint32_t maxRequests = decltype(buffer)::maxRequests;

			template<ResourceViews variant>
			PersistentBindingHandle<variant, frameID> append(GPUResource<variant>::resrc_desc desc, Pipeline::ResourcePermSetSelector<variant>::permissionType permissions) requires DynamicResourceView<variant>
			{
				const uint32_t requestID = buffer.CountRequestsTotal() | persistentIDMask; // Beeg mask to avoid clashes with per-stage bindings

				return buffer.RetrieveRequestBuffer<variant>().append(desc, permissions, requestID);
			}

			const uint32_t CountRequests()
			{
				return buffer.CountRequestsTotal();
			}
		};

	public:
		StageBindingRequestBuffer stageBindingRequests;
		PersistentBindingRequestBuffer persistentBindingRequests;

		void Init()
		{
			stageBindingRequests.init();
			persistentBindingRequests.init();
		}
	};

	template<uint32_t _id>
	struct ShaderHandle
	{
		uint32_t stage;
		uint32_t shaderID;
		SHADER_TYPES shaderType;
		static constexpr uint32_t ownerID = _id;

		ShaderHandle() {}
		ShaderHandle(uint32_t _stage, uint32_t _shaderID, SHADER_TYPES _shaderType) : stage(_stage), shaderID(_shaderID), shaderType(_shaderType) {}
	};
};