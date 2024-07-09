
#include <iostream>
#include <random>
#include <chrono>

#include "..\..\CPUMemory.h"

int main()
{
    CPUMemory::Init();

    static constexpr uint16_t numArrays = 255;

    // Generate a bunch of arrays
    CPUMemory::ArrayAllocHandle<char> arrays[numArrays] = {};
    for (uint32_t i = 0; i < numArrays; i++)
    {
        arrays[i] = CPUMemory::AllocateArrayStatic<char, numArrays>();
    }

    // Generate a bunch of one-off allocations
    struct blah
    {
        uint64_t contents[64] = {};
    };

    static constexpr uint8_t numBlahArrays = 128;
    CPUMemory::ArrayAllocHandle<CPUMemory::SingleAllocHandle<blah>> blah_array;
    blah_array = CPUMemory::AllocateArrayStatic<CPUMemory::SingleAllocHandle<blah>, numBlahArrays>();

    for (uint32_t i = 0; i < numBlahArrays; i++)
    {
        blah_array[i] = CPUMemory::AllocateSingle<blah>();
    }

    // Generate a bunch of randomly-sized arrays (random enough, anyway)
    std::ranlux48 ranlux(std::chrono::high_resolution_clock::now().time_since_epoch().count());

    static constexpr uint16_t numRandArrays = 65535;
    CPUMemory::ArrayAllocHandle<CPUMemory::ArrayAllocHandle<char>> randArrays = {};
    randArrays = CPUMemory::AllocateArrayStatic<CPUMemory::ArrayAllocHandle<char>, numRandArrays>();

    for (uint32_t i = 0; i < numRandArrays; i++)
    {
        randArrays[i] = CPUMemory::AllocateArray<char>(ranlux() % 1024);
    }

    // Perform random frees, up to a limit (...since I can't see how to make the random frees converge without introducing another array)
    uint32_t range = numArrays + numBlahArrays + numRandArrays;
    uint32_t freeCtr = 0;
    while (freeCtr < range)
    {
        printf("successful random free iterations = %u\n", freeCtr);

        const uint32_t arrayFreeIndex = ranlux() % numArrays;
        const uint32_t blahArrayFreeIndex = ranlux() % numBlahArrays;
        const uint32_t randArrayFreeIndex = ranlux() % numRandArrays;

        //printf("blah address = %llu\n", reinterpret_cast<uint64_t>(&blah_array[0]));
        //printf("blah owning handle = %u\n", blah_array.handle);
        //for (uint32_t i = 0; i < numBlahArrays; i++)
        //{
        //    printf("blah array handle %u = %u\n", i, blah_array[i].handle);
        //}

        //printf("freeing regular arrays, clearing index %u\n", arrayFreeIndex);
        CPUMemory::Free(arrays[arrayFreeIndex]);

        //printf("blah address = %llu\n", reinterpret_cast<uint64_t>(&blah_array[0]));
        //printf("blah owning handle = %u\n", blah_array.handle);
        //for (uint32_t i = 0; i < numBlahArrays; i++)
        //{
        //    printf("blah array handle %u = %u\n", i, blah_array[i].handle);
        //}

        //printf("freeing blah arrays, clearing index %u\n", blahArrayFreeIndex);
        CPUMemory::Free(blah_array[blahArrayFreeIndex]);
        CPUMemory::Free(randArrays[randArrayFreeIndex]);
        freeCtr++;

        // Short-term loan test
        CPUMemoryLoan loanTest(ranlux() % 65535);
    }

    // Clear remaining allocs
    for (uint32_t i = 0; i < range; i++)
    {
        if (i < numArrays)
        {
            CPUMemory::Free(arrays[i]);
        }

        else if (i < (numArrays + numBlahArrays))
        {
            CPUMemory::Free(blah_array[i % numBlahArrays]);
        }

        else if (i < (numArrays + numBlahArrays + numRandArrays))
        {
            CPUMemory::Free(randArrays[i % numRandArrays]);
        }
    }

    CPUMemory::Free(blah_array);
    CPUMemory::Free(randArrays);

    // If we got here without an exception, report success ^_^
    CPUMemory::DeInit();
    printf("memory mgr tests passed");
}
