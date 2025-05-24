#include "CPUMemory.h"
#include <memory>
#include "assert.h"

char* data = nullptr;

static constexpr uint64_t scratchFootprint = CPUMemory::initAlloc / 2; // We sacrifice half our memory for scratch
char* scratch = nullptr;

CPUMemory::MemOffset nextAllocOffset = CPUMemory::invalidMemOffset;

struct Alloc
{
	CPUMemory::MemOffset ptrOffset; // Look ma no pointers! This is a byte offset to the start of each alloc's owned data within our private buffer (data)
									// Distinct from the offset within the handle buffer to the handle info (what the internal/external handles point to)
	CPUMemory::MemSize size;

	// For handle updates on memory free
	// When pointers within the alloc buffer are freed, the pointers allocated after them are reordered,
	// making internal handles unstable and difficult to traverse without retracing all the reorders
	// performed by the allocator up to that point
	//
	// However, external handles never change (they *can't* change, or pointer invalidation would be
	// a constant issue, and memory as perceived by the client would only be loosely associated
	// with memory as perceived by the allocator)
	//
	// Previous versions of the allocator addressed the internal handle unreliability issue by directly
	// encoding pointers to the internal handles associated with every alloc, and chasing the pointers
	// during frees to correct those handles for data allocated beyond the alloc being freed; that
	// was simple and effective, but difficult to understand at a glance, and I eventually removed
	// those lines when I came back to the code after an extended break (assuming they were 
	// useless/cruft)
	//
	// Hopefully this solution is more straightforward and less likely to end up binned; we store
	// the unchanging external handles on each alloc, and use those to directly access & patch
	// [handleConvertExternalInternal], instead of going through the sneaky indirection we used 
	// before
	CPUMemory::AllocHandle externalHandle;
};

struct AllocBuffer
{
	static constexpr uint32_t maxNumAllocs = 131072;

	// Hopefully this is high enough ^_^'.
	// If we allocate often enough after startup to run through our alloc budget 4x we have other problems
	// *If* it becomes a problem we can always try to implement a clean-up scheme; just quite challenging
	// and invites the problems we had passing out pointers before (weird bugs, data expiration, can't
	// copy returned addresses, etc)
	// Something heuristic based off time since last use, maybe
	static constexpr uint32_t maxNumHandles = 262144;

	uint32_t numAllocs = 0;
	uint32_t numHandles = 0;

	Alloc allocSet[maxNumAllocs]; // This model severely constrains our total alloc count, but it's this or another [malloc], which...ew? idk
	CPUMemory::AllocHandle handleConvertExternalInternal[maxNumHandles];
};

void InitAllocBuffer(AllocBuffer* allocs);
Alloc FindHandleAlloc(AllocBuffer* allocs, CPUMemory::AllocHandle handle, CPUMemory::AllocHandle* outIndex);

CPUMemory::AllocHandle AddAlloc(AllocBuffer* allocs, CPUMemory::MemOffset destPtr, CPUMemory::MemSize size);
void RemoveAlloc(AllocBuffer* allocs, uint32_t ndx, CPUMemory::AllocHandle handle);

extern AllocBuffer* allocs = nullptr;

static constexpr CPUMemory::MemOffset clientDataOffset = sizeof(AllocBuffer);
static CPUMemory::MemSize memUsed = 0;

#ifdef MEM_MGR_TEST
//#define LOG_MEM_TESTS
#define CORRUPTION_VERIFICATION
#endif

char* swapMem = nullptr;

char* AddressFromOffset(CPUMemory::MemOffset offset)
{
	return data + offset;
}

char* CPUMemory::GetHandlePtr(AllocHandle handle)
{
	assert(handle < AllocBuffer::maxNumAllocs);

	uint32_t ndx = 0;
	return AddressFromOffset( FindHandleAlloc(allocs, handle, &ndx).ptrOffset );
}

void CPUMemory::ZeroData(AllocHandle handle, uint64_t size)
{
	memset(CPUMemory::GetHandlePtr<void>(handle), 0, size);
}

void CPUMemory::FlushData(AllocHandle handle, uint64_t size)
{
	memset(CPUMemory::GetHandlePtr<void>(handle), 0xff, size);
}

void CPUMemory::Init()
{
	data = reinterpret_cast<char*>(malloc(initAlloc));
	allocs = reinterpret_cast<AllocBuffer*>(data);
	scratch = data + (initAlloc - scratchFootprint);
	nextAllocOffset = clientDataOffset;
	memUsed = 0;

	// Initialize book-keeping
	InitAllocBuffer(allocs);
}

void CPUMemory::DeInit()
{
	free(data);
}

CPUMemory::AllocHandle CPUMemory::AllocateRange(CPUMemory::MemSize rangeBytes)
{
	// No benefit to accounting for alignment in addresses, since we re-alloc all over the place
	////////////////////////////////////////////////////////////////////////////////////////////

	// Cache the current allocator start, converted to an offset
	// Can probably straight-up convert nextAllocOffset to an offset to save some logic...
	CPUMemory::MemOffset offset = nextAllocOffset;// - reinterpret_cast<uint64_t>(data); // Probably UB ^_^'

	// Book-keeping ^_^
	AllocHandle handle = AddAlloc(allocs, offset, rangeBytes);

	// Offset future allocs by footprint + initial alignment
	nextAllocOffset += rangeBytes;

	// Update memory utilization tracker
	memUsed += rangeBytes;
	assert(memUsed < scratchFootprint);
	return handle;
}

void CPUMemory::Free(AllocHandle handle)
{	
	bool invalidFree = false;
	if (handle < AllocBuffer::maxNumAllocs)
	{
		if (allocs->handleConvertExternalInternal[handle] == CPUMemory::emptyAllocHandle)
		{
			invalidFree = true;
		}
	}
	else
	{
		invalidFree = true;
	}

	assert(!invalidFree);

	if (invalidFree)
	{
		return; // Handle either points to unallocated memory or memory that was already freed, so return early (and fail in debug mode, see assert above)
	}

	uint32_t allocNdx = 0;
	Alloc alloc = FindHandleAlloc(allocs, handle, &allocNdx);

	if (alloc.ptrOffset != CPUMemory::invalidMemOffset && alloc.size != 0)
	{
		RemoveAlloc(allocs, allocNdx, handle);
#ifdef MEM_MGR_TEST
#ifdef LOG_MEM_TESTS
		printf("Allocation freed successfully\n\n");
#endif
#endif
	}
	else
	{
#ifndef MEM_MGR_TEST
		assert(("Allocation either freed already, or not originally allocated with CPUMemory", false));
#else
#ifdef LOG_MEM_TESTS
		printf("Allocation either freed already, or not originally allocated with CPUMemory\nBad & asserted in active builds (it means we're freeing invalid pointers and might have a leak), but fine in tests (hitting this log shows we've correctly handled this case & not attempted an inaccessible free/double-free)\n\n");
#endif
#endif
	}
}

void InitAllocBuffer(AllocBuffer* allocs)
{
	allocs->numAllocs = 0;
	allocs->numHandles = 0;

	memset(allocs->allocSet, 0xff, sizeof(allocs));
	memset(allocs->handleConvertExternalInternal, CPUMemory::emptyAllocHandle, sizeof(AllocBuffer::handleConvertExternalInternal));
}

Alloc FindHandleAlloc(AllocBuffer* allocs, CPUMemory::AllocHandle handle, CPUMemory::AllocHandle* outIndex)
{
	assert(handle < AllocBuffer::maxNumHandles);

	CPUMemory::AllocHandle convertedHandle = allocs->handleConvertExternalInternal[handle];
	if (convertedHandle > allocs->numHandles || convertedHandle == CPUMemory::emptyAllocHandle)
	{
		Alloc alloc;
		alloc.ptrOffset = CPUMemory::invalidMemOffset;
		alloc.size = 0;
		return alloc;
	}
	else
	{
		*outIndex = convertedHandle; // Considering replacing these functions with operator overloads
		return allocs->allocSet[convertedHandle];
	}
}

CPUMemory::AllocHandle AddAlloc(AllocBuffer* allocs, CPUMemory::MemOffset ptrOffset, CPUMemory::MemSize size)
{
	assert(allocs->numAllocs < allocs->maxNumAllocs);

	Alloc alloc = { ptrOffset, size };
	allocs->allocSet[allocs->numAllocs] = alloc;

	CPUMemory::AllocHandle handle = allocs->numHandles;
	allocs->handleConvertExternalInternal[handle] = allocs->numAllocs;

	// Needed for frees; internal handles need to be updated when they occur, but can't be straightforwardly iterated because of
	// drift between external and internal orderings once repeated frees have occurred (so we keep thee external handle around,
	// and traverse the internal handles through the look-up we assigned above, instead)
	allocs->allocSet[allocs->numAllocs].externalHandle = handle;

	allocs->numAllocs++;
	allocs->numHandles++;

	assert(handle < AllocBuffer::maxNumHandles);

	return handle;
}

void RemoveAlloc(AllocBuffer* allocs, uint32_t ndx, CPUMemory::AllocHandle handle)
{
	Alloc ndxedAlloc = allocs->allocSet[ndx];
	allocs->handleConvertExternalInternal[handle] = CPUMemory::emptyAllocHandle;

	if (ndx != (allocs->numAllocs - 1))
	{
		// Memcpy backwards, update pointers (frees are not cheap!)
		///////////////////////////////////////////////////////////

		// Compute offsets, update active memory footprint
		uint64_t bytesShifting = 0;
		for (uint32_t i = (ndx + 1); i < allocs->numAllocs; i++)
		{
			const Alloc ithAlloc = allocs->allocSet[i];
			bytesShifting += ithAlloc.size;
		}

		// Copy data into scratch
		const Alloc nextAlloc = allocs->allocSet[ndx + 1];
		memcpy(scratch, AddressFromOffset(nextAlloc.ptrOffset), bytesShifting);

		// Memcpy & pointer updates
		for (uint32_t i = (ndx + 1); i < allocs->numAllocs; i++)
		{
			const Alloc currAlloc = allocs->allocSet[i];
			const Alloc prevAlloc = allocs->allocSet[i - 1];

			assert(currAlloc.size < scratchFootprint);
			allocs->allocSet[i].ptrOffset -= ndxedAlloc.size; // More efficient than pointer reassignment
		}

		// Copy data back from scratch into freed address
		memcpy(AddressFromOffset(ndxedAlloc.ptrOffset), scratch, bytesShifting);

		// Bubble-out the null alloc
		for (uint32_t i = ndx; i < (allocs->numAllocs - 1); i++)
		{
			std::swap(allocs->allocSet[i], allocs->allocSet[i + 1]);
		}

		// Update internal handles
		for (uint32_t i = (ndx + 1); i < allocs->numAllocs; i++)
		{
			allocs->handleConvertExternalInternal[allocs->allocSet[i].externalHandle]--;
		}

		// Update the next allocation address
		memUsed -= ndxedAlloc.size;
		nextAllocOffset = clientDataOffset + memUsed;

#ifdef CORRUPTION_VERIFICATION
		assert(memcmp(*nextAllocOffset - bytesShifting, scratch, bytesShifting) == 0);
#endif
	}
	else
	{
		nextAllocOffset = ndxedAlloc.ptrOffset;
		memUsed -= ndxedAlloc.size;
	}

	allocs->numAllocs--;
}
