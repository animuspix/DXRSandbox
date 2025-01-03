// MortonTesting.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <stdio.h>
#include <stdint.h>
#include <type_traits>

#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

template<uint8_t d>// : requires(d > 1 && d <= 3) // Need to google how to write these properly
struct pt
{
    uint8_t e[d] = {};
};

template<uint8_t d, uint8_t gridWidth, uint8_t gridArea>
void gridInit(pt<d>* grid)
{
    const uint8_t gridSize = d == 2 ? gridArea : gridArea * gridWidth;
    for (uint8_t i = 0; i < gridSize; i++)
    {
        if (d == 3)
        {
            const uint8_t slice = i / gridArea;
            const uint8_t sliceCoordinate = i % gridArea;

            grid[i].e[0] = sliceCoordinate % gridWidth;
            grid[i].e[1] = sliceCoordinate / gridWidth;
            grid[i].e[2] = slice;
        }
        else
        {
            grid[i].e[0] = i % gridWidth;
            grid[i].e[1] = i / gridWidth;
        }
    }
}

template<uint8_t dimension>
void mortonDecode(uint8_t mortonCode, uint8_t* decodeBuffer)
{
    for (uint8_t k = 0; k < 8; k++)
    {
        const uint8_t destBitNdx = k / dimension;
        const uint8_t destShift = k - destBitNdx;

        const uint8_t mask = 1 << k;
        const uint8_t n = (mortonCode & mask) >> destShift;

        decodeBuffer[k % dimension] |= n;
    }
}

void mortonVerify(uint8_t gridSize, uint8_t* mortonCodes, pt<2>* grid)
{
    printf("Verifying 2D morton codes\n");

    for (uint8_t i = 0; i < gridSize; i++)
    {
        uint8_t decodeBuffer[2] = {};
        mortonDecode<2>(mortonCodes[i], decodeBuffer);

        const bool decodeSuccess = (grid[i].e[0] == decodeBuffer[0]) && (grid[i].e[1] == decodeBuffer[1]);

        printf("Morton code %u, decoded coordinate (%u, %u), original coordinate (%u, %u): %s\n",
            mortonCodes[i], decodeBuffer[0], decodeBuffer[1], grid[i].e[0], grid[i].e[1], decodeSuccess ? "success" : "oops");
    }

    printf("Verification complete\n\n");
}

void mortonVerify(uint8_t gridSize, uint8_t* mortonCodes, pt<3>* grid)
{
    printf("Verifying 3D morton codes\n");

    for (uint8_t i = 0; i < gridSize; i++)
    {
        uint8_t decodeBuffer[3] = {};
        mortonDecode<3>(mortonCodes[i], decodeBuffer);

        const bool decodeSuccess = (grid[i].e[0] == decodeBuffer[0]) && (grid[i].e[1] == decodeBuffer[1]) && (grid[i].e[2] == decodeBuffer[2]);

        printf("Morton code %u, decoded coordinate (%u, %u, %u), original coordinate (%u, %u, %u): %s\n",
            mortonCodes[i], decodeBuffer[0], decodeBuffer[1], decodeBuffer[2], grid[i].e[0], grid[i].e[1], grid[i].e[2], decodeSuccess ? "success" : "oops");
    }

    printf("Verification complete\n\n");
}

int main()
{
    // 2D test suite
    ////////////////

    // Changing grid-width requires updating our expected value buffer (see below)
    // (and likely moving to u16 instead of u8)
    const uint8_t gridWidth2D = 4;
    const uint8_t gridArea2D = gridWidth2D * gridWidth2D;
    
    pt<2> grid2D[gridArea2D] = {};
    gridInit<2, gridWidth2D, gridArea2D>(grid2D);

    // Generate codes with simple interleaving
    uint8_t mortonCodes2D[gridArea2D] = {};

    // Find the actual max number of bits needed for coordinates
    DWORD firstBitSet = 0;
    BitScanReverse(&firstBitSet, gridWidth2D);
    const uint8_t coordBitDepth2D = firstBitSet;

    // Simple interleaf algorithm
    for (uint8_t i = 0; i < gridArea2D; i++)
    {
        // Ignore bits that will always be zero
        const uint8_t mask = (1 << coordBitDepth2D) - 1;

        // Load & mask the axes we want to interleaf
        const uint8_t left = grid2D[i].e[0] & mask;
        const uint8_t right = grid2D[i].e[1] & mask;
        const uint8_t pair[2] = { left, right };

        // Walk through the interleaved Morton code
        // For every bit
        // - Find the offset within either axis (k / 2, where k is 2x the bitdepth, so that the same bit indices are read from left and right)
        // - Create a flag to select the specific bits we want to interleave in each step (bitSel)
        // - Pick an axis (we alternate left and right), then select the bits we want, and shift back by the offset we calculated before to get a plain 1/0 value
        // - Use [k] to send the selected bit to the correct offset in the Morton code
        for (uint8_t k = 0; k < (coordBitDepth2D * 2); k++)
        {
            const uint8_t bitOffs = k / 2;
            const uint8_t bitSel = 1 << bitOffs;
            const uint8_t bit = (pair[k % 2] & bitSel) >> bitOffs;
            mortonCodes2D[i] |= bit << k;
        }
    }

    // Verify, by decoding & comparing to source
    mortonVerify(gridArea2D, mortonCodes2D, grid2D);

    // 3D test suite
    ////////////////

    // Depths >2 are impossible with u8s; should move to u16s or u32s and try 5x5x5 or 10x10x10 grids (obvi with different error reporting to maintain speed/reduce console spam)
    const uint8_t gridWidth3D = 5;
    const uint8_t gridArea3D = gridWidth3D * gridWidth3D;
    const uint8_t gridVolume = gridWidth3D * gridArea3D;

    pt<3> grid3D[gridVolume] = {};
    gridInit<3, gridWidth3D, gridArea3D>(grid3D);

    // Generate codes, more complex interleaving this time
    uint8_t mortonCodes3D[gridVolume] = {};

    // Find the actual max number of bits needed for coordinates
    BitScanReverse(&firstBitSet, gridWidth3D);
    const uint8_t coordBitDepth3D = firstBitSet + 1; // BSR returns an index; not sure how problematic that is, but worth keeping in mind

    for (uint8_t i = 0; i < gridVolume; i++)
    {
        // Ignore bits that will always be zero
        const uint8_t mask = (1 << coordBitDepth3D) - 1;

        // Load & mask the axes we want to interleaf
        const uint8_t x = grid3D[i].e[0] & mask;
        const uint8_t y = grid3D[i].e[1] & mask;
        const uint8_t z = grid3D[i].e[2] & mask;
        const uint8_t triple[3] = { x, y, z };

        // Walk through the interleaved Morton code
        const uint8_t kmax = coordBitDepth3D * 3;
        for (uint8_t k = 0; k < kmax; k++)
        {
            const uint8_t n = triple[k % 3];
            const uint8_t bitOffs = k / 3; // In the range 0...(coordBitDepth3D - 1)
            const uint8_t bitSel = 1 << bitOffs;
            const uint8_t bit = (n & bitSel) >> bitOffs;
            mortonCodes3D[i] |= bit << k;
        }
    }

    // Verify, using another known-good array
    mortonVerify(gridVolume, mortonCodes3D, grid3D);
}
