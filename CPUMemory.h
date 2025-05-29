#pragma once

#include <stdint.h>
#include <type_traits>
#include <concepts>
#include <assert.h>
#include <limits>

#undef min
#undef max

class CPUMemory
{
	public:
#ifdef MEM_MGR_TEST
		static constexpr uint64_t initAlloc = 1024 * 1024 * 1024; // ~1GiB
#else
		// Note the first 4MB or so are reserved for internal book-keeping
		// (buffers of allocations etc, needed if we want to support frees
		// within the allocator and not just off the end)
		static constexpr uint64_t initAlloc = 1024 * 1024 * 512; // ~512MiB
#endif

	private:
		using allocHandleMetaT0 = std::conditional_t<(initAlloc > UINT8_MAX), uint16_t, uint8_t>;
		using allocHandleMetaT1 = std::conditional_t<(initAlloc > UINT16_MAX), uint32_t, allocHandleMetaT0>;
		using allocHandleMetaT2 = std::conditional_t<(initAlloc > UINT32_MAX), uint64_t, allocHandleMetaT1> ;
		using sizeType = allocHandleMetaT2;

	public:
		using MemSize = sizeType;
		using MemOffset = sizeType;
		using AllocHandle = sizeType;

		static constexpr MemSize memSizeLimit = std::numeric_limits<sizeType>().max();
		static constexpr AllocHandle emptyAllocHandle = memSizeLimit;
		static constexpr MemOffset invalidMemOffset = static_cast<MemOffset>(emptyAllocHandle);

	private:
		static char* GetHandlePtr(AllocHandle handle);

		template<typename ptrType>
		static ptrType* GetHandlePtr(AllocHandle handle)
		{
			// There's probably a better way to implement this
			// Maybe something in C++23ish?
			return reinterpret_cast<ptrType*>(GetHandlePtr(handle));
		}

	public:
		struct ByteSpan
		{
			private:
				AllocHandle owningHandle = emptyAllocHandle;
				MemOffset offset = 0;
				MemSize length = 0;

				ByteSpan(AllocHandle _handle, MemOffset _offset, MemSize _length) : owningHandle(_handle), offset(_offset), length(_length) {}
			
			public:
				template<std::integral sizeTagType>
				void* Bytes(sizeTagType& lengthOut)
				{
					lengthOut = length;
					return GetHandlePtr(owningHandle) + offset;
				}

				bool HasDefinedElements()
				{
					return owningHandle != emptyAllocHandle && length != 0 && offset < length;
				}

				ByteSpan() : owningHandle(emptyAllocHandle), offset(0), length(0) {};

			friend CPUMemory;
		};

		template<typename type>
		struct SingleAllocHandle
		{
			using innerType = type;
			AllocHandle handle = emptyAllocHandle;
			SingleAllocHandle(AllocHandle _handle) : handle(_handle) {};

			innerType* operator->() const noexcept
			{
				return CPUMemory::GetHandlePtr<innerType>(handle);
			}

			innerType& operator*() const noexcept
			{
				return *CPUMemory::GetHandlePtr<innerType>(handle);
			}

			friend struct ByteSpan;
			ByteSpan GetByteSpan()
			{
				return ByteSpan(handle, 0, sizeof(innerType));
			}

			SingleAllocHandle() { handle = emptyAllocHandle; }
		};

		template<typename type>
		struct ArrayAllocHandle
		{
			using innerType = type;

			MemSize arrayLen = 0;
			AllocHandle handle = emptyAllocHandle;

			ArrayAllocHandle() : arrayLen(0), handle(emptyAllocHandle) {}
			ArrayAllocHandle(MemSize numElts, AllocHandle _handle) : arrayLen(numElts), handle(_handle) {}

			innerType& operator[](size_t elt) const
			{
				return CPUMemory::GetHandlePtr<innerType>(handle)[elt];
			}

			// For data atlassing, nested buffers, etc
			struct ArraySubsetHandle
			{
				public:
					using ownerElementType = type;

				private:
					MemSize lengthInElements = 0;
					MemOffset offsetInElements = 0;
					AllocHandle owningHandle = emptyAllocHandle;

				public:
					ownerElementType& operator[](size_t elt) const
					{
						assert(elt < static_cast<size_t>(lengthInElements));
						assert(owningHandle != emptyAllocHandle);

						return CPUMemory::GetHandlePtr<ownerElementType>(owningHandle)[offsetInElements + elt];
					}

					void ZeroData()
					{
						memset(CPUMemory::GetHandlePtr<ownerElementType>(owningHandle) + offsetInElements, 0x0, lengthInElements * sizeof(ownerElementType));
					}

					void FlushData()
					{
						memset(CPUMemory::GetHandlePtr<ownerElementType>(owningHandle) + offsetInElements, 0xff, lengthInElements * sizeof(ownerElementType));
					}

					void CopyDataFrom(void* src)
					{
						assert(owningHandle != emptyAllocHandle);
						assert(src != nullptr);
						memcpy(CPUMemory::GetHandlePtr<ownerElementType>(owningHandle) + offsetInElements, src, lengthInElements * sizeof(ownerElementType));
					}

					void CopyDataTo(void* dst)
					{
						assert(owningHandle != emptyAllocHandle);
						assert(dst != nullptr);
						memcpy(dst, CPUMemory::GetHandlePtr<ownerElementType>(owningHandle) + offsetInElements, lengthInElements * sizeof(ownerElementType));
					}

					int CompareData(void* other)
					{
						assert(owningHandle != emptyAllocHandle);
						assert(other != nullptr);
						return memcmp(CPUMemory::GetHandlePtr<ownerElementType>(owningHandle) + offsetInElements, other, lengthInElements * sizeof(ownerElementType));
					}

					ByteSpan GetByteSpan()
					{
						return ByteSpan(owningHandle, offsetInElements, lengthInElements * sizeof(ownerElementType));
					}

					template<typename type>
					void CopyDataFrom(ArrayAllocHandle<type> src)
					{
						assert(owningHandle != emptyAllocHandle);
						assert(src.handle != emptyAllocHandle);
						assert(src.arrayLen <= lengthInElements);

						CopyDataFrom(GetHandlePtr(src.handle));
					}

					template<typename type>
					void CopyDataTo(ArrayAllocHandle<type> dst)
					{
						assert(owningHandle != emptyAllocHandle);
						assert(dst.handle != emptyAllocHandle);
						assert(dst.arrayLen <= lengthInElements);

						CopyDataTo(GetHandlePtr(dst.handle));
					}

				friend ArrayAllocHandle<ownerElementType>;
			};

			void SubsetHandle(ArraySubsetHandle& subset, MemSize elementOffset, MemSize elementCount) const
			{
				assert((elementOffset + elementCount) <= arrayLen);

				subset.offsetInElements = elementOffset;
				subset.lengthInElements = elementCount;
				subset.owningHandle = handle;
			}

			ArraySubsetHandle operator+(MemOffset offset) const
			{
				assert(offset < arrayLen);
				assert(handle != emptyAllocHandle);

				ArraySubsetHandle subrange = {};
				SubsetHandle(subrange, offset, arrayLen - offset);

				return subrange;
			}

			ByteSpan GetByteSpan()
			{
				return ByteSpan(handle, 0, arrayLen * sizeof(innerType));
			}
		};

	private:
		static void ZeroData(AllocHandle handle, uint64_t size);
		static void FlushData(AllocHandle handle, uint64_t size);
	public:

		template<typename type>
		static void ZeroData(ArrayAllocHandle<type> arrayHandle)
		{
			ZeroData(arrayHandle.handle, arrayHandle.arrayLen * sizeof(type));
		}

		template<typename type>
		static void ZeroData(SingleAllocHandle<type> singleHandle)
		{
			ZeroData(singleHandle.handle, sizeof(type));
		}

		// Inverse of [ZeroData]; set every byte in the given block
		template<typename type>
		static void FlushData(ArrayAllocHandle<type> arrayHandle)
		{
			FlushData(arrayHandle.handle, arrayHandle.arrayLen * sizeof(type));
		}

		template<typename type>
		static void FlushData(SingleAllocHandle<type> singleHandle)
		{
			FlushData(singleHandle.handle, sizeof(type));
		}

		template<typename type>
		static void CopyData(ArrayAllocHandle<type> src, ArrayAllocHandle<type> dst)
		{
			const type* ptrSrc = CPUMemory::GetHandlePtr<type>(src.handle);
			type* ptrDst = CPUMemory::GetHandlePtr<type>(dst.handle);

			assert(src.arrayLen <= dst.arrayLen);

			memcpy(ptrDst, ptrSrc, src.arrayLen * sizeof(type));
		}

		template<typename type>
		static void CopyData(ArrayAllocHandle<type> src, void* dst)
		{
			assert(dst != nullptr);

			const type* ptrSrc = CPUMemory::GetHandlePtr<type>(src.handle);
			memcpy(dst, ptrSrc, src.arrayLen * sizeof(type));
		}

		template<typename type>
		static void CopyData(void* src, ArrayAllocHandle<type> dst)
		{
			assert(src != nullptr);
			assert(dst.handle != emptyAllocHandle);
			
			type* ptrDst = CPUMemory::GetHandlePtr<type>(dst.handle);
			memcpy(ptrDst, src, dst.arrayLen * sizeof(type));
		}

		template<typename type>
		static void CopyData(SingleAllocHandle<type> src, SingleAllocHandle<type> dst)
		{
			assert(src.handle != emptyAllocHandle);
			assert(dst.handle != emptyAllocHandle);

			const type* ptrSrc = CPUMemory::GetHandlePtr<type>(src.handle);
			type* ptrDst = CPUMemory::GetHandlePtr<type>(dst.handle);

			memcpy(ptrDst, ptrSrc, sizeof(type));
		}

		template<typename type>
		static void CopyData(SingleAllocHandle<type> src, void* dst)
		{
			assert(dst != nullptr);
			assert(src.handle != emptyAllocHandle);

			const type* ptrSrc = CPUMemory::GetHandlePtr<type>(src.handle);
			memcpy(dst, ptrSrc, sizeof(type));
		}

		template<typename type>
		static void CopyData(void* src, SingleAllocHandle<type> dst)
		{
			assert(src != nullptr);
			assert(dst.handle != emptyAllocHandle);

			type* ptrDst = CPUMemory::GetHandlePtr<type>(dst.handle);
			memcpy(ptrDst, src, sizeof(type));
		}

		static void CopyData(ByteSpan src, void* dst)
		{
			assert(dst != nullptr);
			assert(src.owningHandle != emptyAllocHandle);

			memcpy(dst, GetHandlePtr(src.owningHandle) + src.offset, src.length);
		}

		template<typename type>
		static void CopyData(ArrayAllocHandle<type> src, ArrayAllocHandle<type>::ArraySubsetHandle dst)
		{
			dst.CopyDataFrom(src);
		}

		template<typename type>
		static void CopyData(ArrayAllocHandle<type>::ArraySubsetHandle src, ArrayAllocHandle<type> dst)
		{
			src.CopyDataTo(dst);
		}

		template<typename type>
		static void CopyData(ByteSpan src, ArrayAllocHandle<type> dst)
		{
			assert(src.owningHandle != emptyAllocHandle);
			assert(dst.handle != emptyAllocHandle);
			assert(src.length <= dst.arrayLen * sizeof(type));

			memcpy(GetHandlePtr(dst.handle), GetHandlePtr(src.owningHandle) + src.offset, src.length);
		}

		template<typename type>
		static void CopyData(ArrayAllocHandle<type> src, ByteSpan dst)
		{
			CopyData(dst, src);
		}

		template<typename type>
		static int CompareData(ArrayAllocHandle<type> a, ArrayAllocHandle<type> b)
		{
			const type* ptrA = CPUMemory::GetHandlePtr<type>(a.handle);
			const type* ptrB = CPUMemory::GetHandlePtr<type>(b.handle);

			assert(a.arrayLen == b.arrayLen);
			return memcmp(ptrA, ptrB, a.arrayLen * sizeof(type));
		}

		template<typename type>
		static int CompareData(SingleAllocHandle<type> a, SingleAllocHandle<type> b)
		{
			const type* ptrA = CPUMemory::GetHandlePtr<type>(a.handle);
			const type* ptrB = CPUMemory::GetHandlePtr<type>(b.handle);

			assert(a.arrayLen == b.arrayLen);
			return memcmp(ptrA, ptrB, a.arrayLen * sizeof(type));
		}

		template<typename type>
		static int CompareData(ArrayAllocHandle<type> a, void* b)
		{
			assert(b != nullptr);

			type* ptrA = CPUMemory::GetHandlePtr<type>(a.handle);
			return memcmp(ptrA, b, a.arrayLen * sizeof(type));
		}

		template<typename type>
		static int CompareData(void* a, ArrayAllocHandle<type> b)
		{
			return CompareData(b, a);
		}

		template<typename type>
		static SingleAllocHandle<type> AllocateSingle()
		{
			return SingleAllocHandle<type>(AllocateRange(sizeof(type)));
		}

		template<typename arrayType, MemSize num>
		static ArrayAllocHandle<arrayType> AllocateArrayStatic()
		{
			ArrayAllocHandle<arrayType> arrayHandle;
			
			const uint64_t allocSize = sizeof(arrayType) * num;
			assert(allocSize < initAlloc); 
			
			arrayHandle.handle = AllocateRange(static_cast<MemSize>(allocSize));
			arrayHandle.arrayLen = num;
			return arrayHandle;
		}

		template<typename arrayType>
		static ArrayAllocHandle<arrayType> AllocateArray(MemSize num)
		{
			ArrayAllocHandle<arrayType> arrayHandle;
			
			const uint64_t allocSize = sizeof(arrayType) * num;
			assert(allocSize < initAlloc);

			arrayHandle.handle = AllocateRange(static_cast<MemSize>(allocSize));
			arrayHandle.arrayLen = num;
			return arrayHandle;
		}

		static void Init();
		static void DeInit();

	private:
		static void Free(AllocHandle handle);
	public:
		template<typename HandleType>
		static void Free(ArrayAllocHandle<HandleType> _handle)
		{
			Free(_handle.handle);
		}

		template<typename HandleType>
		static void Free(SingleAllocHandle<HandleType> _handle)
		{
			Free(_handle.handle);
		}

	private:
		static AllocHandle AllocateRange(MemSize rangeBytes);
};

// Scoped memory loan, useful for functions where we want to access a lot of memory quickly without making a permanent allocation
struct CPUMemoryLoan
{
	CPUMemoryLoan(uint32_t loanSize)
	{
		mem = CPUMemory::AllocateArray<char>(loanSize);
	}

	~CPUMemoryLoan()
	{
		CPUMemory::Free<char>(mem.handle);
	}

	CPUMemory::ArrayAllocHandle<char> mem = { 0, CPUMemory::emptyAllocHandle };
};
