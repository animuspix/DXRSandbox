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
		static constexpr uint64_t initAlloc = 1024 * 1024 * 512; // ~512MiB
#endif

	private:
		using allocHandleMetaT0 = std::conditional_t<(initAlloc > UINT8_MAX), uint16_t, uint8_t>;
		using allocHandleMetaT1 = std::conditional_t<(initAlloc > UINT16_MAX), uint32_t, allocHandleMetaT0>;
		using allocHandleMetaT2 = std::conditional_t<(initAlloc > UINT32_MAX), uint64_t, allocHandleMetaT1> ;
		using internalAllocHandleType = allocHandleMetaT2;

	public:
		using AllocHandle = internalAllocHandleType;
		static constexpr AllocHandle emptyAllocHandle = std::numeric_limits<internalAllocHandleType>().max();

	private:
		static char* GetHandlePtr(AllocHandle handle);

		template<typename ptrType>
		static ptrType* GetHandlePtr(AllocHandle handle)
		{
			return reinterpret_cast<ptrType*>(GetHandlePtr(handle));
		}

	public:
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

			SingleAllocHandle() { handle = emptyAllocHandle; }
		};

		template<typename type>
		struct ArrayAllocHandle
		{
			using innerType = type;

			uint64_t arrayLen = 0;
			AllocHandle handle = emptyAllocHandle;
			size_t dataOffset = 0; // For data atlassing, nested buffers, etc

			ArrayAllocHandle() : arrayLen(0), handle(emptyAllocHandle), dataOffset(0) {}
			ArrayAllocHandle(uint64_t numElts, AllocHandle _handle, size_t _dataOffset = 0) : arrayLen(numElts), handle(_handle), dataOffset(_dataOffset) {}

			innerType* operator->() const
			{
				return CPUMemory::GetHandlePtr<innerType>(handle) + dataOffset;
			}

			innerType& operator*() const
			{
				return *(CPUMemory::GetHandlePtr<innerType>(handle) + dataOffset);
			}

			innerType& operator[](size_t elt) const
			{
				return (CPUMemory::GetHandlePtr<innerType>(handle) + dataOffset)[elt];
			}

			template<std::integral offsetType>
			ArrayAllocHandle<type> operator+(offsetType offset) const
			{
				auto ret = *this;
				ret.dataOffset = offset;
				return ret;
			}

			ArrayAllocHandle<uint8_t> GetBytesHandle()
			{
				return ArrayAllocHandle<uint8_t>(arrayLen * sizeof(innerType), handle, dataOffset);
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
			const type* ptrSrc = CPUMemory::GetHandlePtr<type>(src.handle);
			memcpy(dst, ptrSrc, src.arrayLen * sizeof(type));
		}

		template<typename type>
		static void CopyData(void* src, ArrayAllocHandle<type> dst)
		{
			type* ptrDst = CPUMemory::GetHandlePtr<type>(dst.handle);
			memcpy(ptrDst, src, dst.arrayLen * sizeof(type));
		}

		template<typename type>
		static void CopyData(SingleAllocHandle<type> src, SingleAllocHandle<type> dst)
		{
			const type* ptrSrc = CPUMemory::GetHandlePtr<type>(src.handle);
			type* ptrDst = CPUMemory::GetHandlePtr<type>(dst.handle);

			memcpy(ptrDst, ptrSrc, sizeof(type));
		}

		template<typename type>
		static void CopyData(SingleAllocHandle<type> src, void* dst)
		{
			const type* ptrSrc = CPUMemory::GetHandlePtr<type>(src.handle);
			memcpy(dst, ptrSrc, sizeof(type));
		}

		template<typename type>
		static void CopyData(void* src, SingleAllocHandle<type> dst)
		{
			type* ptrDst = CPUMemory::GetHandlePtr<type>(dst.handle);
			memcpy(ptrDst, src, sizeof(type));
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
			type* ptrA = CPUMemory::GetHandlePtr<type>(a.handle);
			return memcmp(ptrA, b, a.arrayLen * sizeof(type));
		}

		template<typename type>
		static int CompareData(void* a, ArrayAllocHandle<type> b)
		{
			type* ptrB = CPUMemory::GetHandlePtr(b.handle);
			return memcmp(a, ptrB, b.arrayLen * sizeof(type));
		}

		template<typename type>
		static SingleAllocHandle<type> AllocateSingle()
		{
			return SingleAllocHandle<type>(AllocateRange(sizeof(type)));
		}

		template<typename arrayType, uint64_t num>
		static ArrayAllocHandle<arrayType> AllocateArrayStatic()
		{
			ArrayAllocHandle<arrayType> arrayHandle;
			arrayHandle.handle = AllocateRange(sizeof(arrayType) * num);
			arrayHandle.arrayLen = num;
			return arrayHandle;
		}

		template<typename arrayType>
		static ArrayAllocHandle<arrayType> AllocateArray(uint64_t num)
		{
			ArrayAllocHandle<arrayType> arrayHandle;
			arrayHandle.handle = AllocateRange(sizeof(arrayType) * num);
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
			assert(_handle.dataOffset == 0); // Offset handles are pointers into another allocation and can't be freed themselves
			Free(_handle.handle);
		}

		template<typename HandleType>
		static void Free(SingleAllocHandle<HandleType> _handle)
		{
			Free(_handle.handle);
		}

	private:
		static AllocHandle AllocateRange(uint64_t rangeBytes);

		// To enable loans without exposing de-allocs (icky to use with linear allocators like this)
		friend struct CPUMemoryLoan;
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
		CPUMemory::Free(mem.handle);
	}

	CPUMemory::ArrayAllocHandle<char> mem = { 0, CPUMemory::emptyAllocHandle };
};
