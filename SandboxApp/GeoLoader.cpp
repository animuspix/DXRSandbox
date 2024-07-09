#include "GeoLoader.h"
#include "..\CPUMemory.h"
#include "Materials.h"

#include <fstream>
#include <filesystem>

constexpr uint64_t maxNumVts = 0;
constexpr uint64_t maxNumNdces = 0;

struct DXRS_Header
{
	char sig[4] = { 'D', 'X', 'R', 'S' };
	uint64_t numVts;
	uint64_t numNdces;

	uint64_t spectralTexFootprint;
	uint16_t spectralTexWidth, spectralTexHeight;
	uint8_t scatteringFunction;

	uint64_t roughnessTexFootprint;
	uint16_t roughnessTexWidth, roughnessTexHeight;
};

struct DXRS_Vertex3D
{
	DirectX::XMFLOAT3 xyz;
	DirectX::XMFLOAT3 n;
	float u, v;
};

enum OBJ_ATTRIB_DECODE_MODE
{
	POS,
	UV,
	NORMAL,
	FACE
};

struct OBJ_BinaryFace
{
	uint32_t pos_uv_normal[3] = {};
};

struct OBJ_AttribVtNdxPair
{
	// Attributes are triangulated, vertex indices are kept and used to match attribute packages (pos, uv, normal) with attributes (uv/normal)
	// Geometry is triangulated separately
	uint64_t attrib = 0, vt = 0; 
};

// Geometry abstractions for processing
// (normal resolution in particular)
struct TriMeta
{
	uint32_t indices[3];
	vec4 normal;
};

struct VertMeta
{
	uint32_t connectedTris[16] = {};
	uint32_t numConnectedTris = 0;
};

// Can most likely optimize this somewhat using getline on initial load, rather than separating load + pass

void GeoLoader::LoadObj(const char* path, MeshLoadParams params)
{
	// Open stream to data, get file size
	std::fstream strm(path);
	uint64_t pathBytes = std::filesystem::file_size(path);

	// Load model data
	CPUMemory::ArrayAllocHandle<char> modelData = CPUMemory::AllocateArray<char>(pathBytes);
	strm.read(&modelData[0], pathBytes);
	const size_t strmLen = strm.gcount(); // Varying newlines across platforms mean reported file size can be larger (but probably not smaller) than actual readable size

	// Allocate temporary storage for vertex elements
	uint8_t vertStride = 0;
	uint32_t vertsFront = 0;
	auto verts = CPUMemory::AllocateArray<float>(pathBytes);

	uint8_t uvStride = 0;
	uint32_t uvsFront = 0;
	auto uvs = CPUMemory::AllocateArray<float>(pathBytes);

	uint8_t normalsStride = 0;
	uint32_t normalsFront = 0;
	auto normals = CPUMemory::AllocateArray<float>(pathBytes);

	uint8_t facesStride = 0;
	uint32_t facesFront = 0;
	auto faces = CPUMemory::AllocateArray<OBJ_BinaryFace>(pathBytes);

	// Sort candidates for each element into temp storage
	OBJ_ATTRIB_DECODE_MODE decodeMode = {};
	uint32_t objCursor = 0;

	bool loadFailed = false;
	char err[256] = {};
	while (objCursor < strmLen && !loadFailed)
	{
		// Scan current line
		uint32_t scanCursor = 0;
		char line[32] = {};
		while (modelData[objCursor + scanCursor] != '\n')
		{
			line[scanCursor] = modelData[objCursor + scanCursor];
			scanCursor++;

			assert(scanCursor + objCursor < strmLen);
		}

		// Check first two characters, decide decode mode
		bool decoding = false;
		uint8_t* attribStride = nullptr;
		if (line[0] == 'v' && line[1] == ' ')
		{
			decoding = true;
			decodeMode = POS;
			attribStride = &vertStride;
		}
		else if (line[0] == 'v' && line[1] == 't')
		{
			decoding = true;
			decodeMode = UV;
			attribStride = &uvStride;
		}
		else if (line[0] == 'v' && line[1] == 'n')
		{
			decoding = true;
			decodeMode = NORMAL;
			attribStride = &normalsStride;
		}
		else if (line[0] == 'f' && line[1] == ' ')
		{
			decoding = true;
			decodeMode = FACE;
			attribStride = &facesStride;
		}
		else if (line[0] == 'l')
		{
			sprintf_s(err, "DXRSandbox does not support OBJ files with polyline attributes; failed to load OBJ\n");
			OutputDebugStringA(err);
			loadFailed = true;
			break;
		}
		else if (memcmp(line, "cstype", 7) == 0)
		{
			sprintf_s(err, "DXRSandbox does not support OBJ files with curved geometry; failed to load OBJ\n");
			OutputDebugStringA(err);
			loadFailed = true;
			break;
		}

		if (decoding)
		{
			constexpr uint8_t attribDigits = 16; // Hopefully we don't need more than 16 total digits
			char attrib[attribDigits] = {};

			// Offset attribute reads past the pos/uv/normal/face line-header
			uint8_t lineHeadOffs = (decodeMode == POS || decodeMode == FACE) ? 2 : 3;

			uint8_t attribCtr = 0;
			uint8_t attribIter = lineHeadOffs;
			uint8_t attribWriter = 0;

			while (attribIter <= scanCursor)
			{
				// Copy in current attribute to [attrib]
				attrib[attribWriter] = line[attribIter];

				// Delimit individual attributes when either we hit the delimit character, or we run out of characters in the current attribute
				if (attrib[attribWriter] == ' ' || attribWriter == (attribDigits - 1) || attrib[attribWriter] == '\0')
				{
					attrib[attribWriter] = '\0';

					// After delimiting, binarize & stash the current attribute
					if (decodeMode != FACE)
					{
						char* strtofEnd = {};
						const float attrValue = strtof(attrib, &strtofEnd);
						if (decodeMode == POS)
						{
							verts[vertsFront] = attrValue;
							vertsFront++;
						}
						else if (decodeMode == UV)
						{
							uvs[uvsFront] = attrValue;
							uvsFront++;
						}
						else // if (decodeMode == NORMAL)
						{
							normals[normalsFront] = attrValue;
							normalsFront++;
						}
					}
					else
					{
						OBJ_BinaryFace& binFace = faces[facesFront];

						uint8_t faceCrawler = 0; // Step past initial character + space
						uint8_t ndcesCrawled = 0;
						uint8_t ndxWriter = 0;
						char ndx[attribDigits / 2] = {};

						while (ndcesCrawled < 3)
						{
							ndx[ndxWriter] = attrib[faceCrawler];

							if (ndx[ndxWriter] == '/' || ndx[ndxWriter] == '\0') // Non-trivial to clip indices here if we run out of digits - kinda just hoping that doesn't happen ^_^'
							{
								ndx[ndxWriter] = '\0';
								binFace.pos_uv_normal[ndcesCrawled] = atoi(ndx);
								ndcesCrawled++;
								ndxWriter = 0;
							}
							else
							{
								ndxWriter++;
							}

							faceCrawler++;
						}

						facesFront++;
					}

					attribCtr++;
					attribWriter = 0;
				}
				else
				{
					attribWriter++;
				}

				attribIter++;
			}

			// Update attribute stride for the current property
			const uint8_t lastAttribStride = *attribStride;
			*attribStride = attribCtr;

			if (attribStride == &facesStride && *attribStride > 4)
			{
				sprintf_s(err, "DXRSandbox does not support OBJ files with more than 4 edges/face (quads are triangulated); failed to load OBJ\n");
				OutputDebugStringA(err);

				loadFailed = true;
			}
			else if (attribStride == &facesStride && *attribStride != lastAttribStride && lastAttribStride != 0)
			{
				sprintf_s(err, "DXRSandbox does not support OBJ files with varying edges/face; failed to load OBJ\n");
				OutputDebugStringA(err);

				loadFailed = true;
			}
		}

		objCursor += scanCursor + 1;
	}

	// Failure case! Use a concrete triangle if OBJ imports don't work out
	// (i.e. unexpected geometry)
	if (loadFailed)
	{
		params.outVerts[0].pos = float4(-0.5f, -0.5f, 0.0f, 0.0f);
		params.outVerts[1].pos = float4(0.0f, 0.5f, 0.0f, 0.0f);
		params.outVerts[2].pos = float4(0.5f, -0.5f, 0.0f, 0.0f);

		params.outVerts[0].mat = float4(-0.5f, -0.5f, params.inMaterialID, static_cast<uint8_t>(SCATTERING_FUNCTIONS::OREN_NAYAR)); // OBJ imports are always white + smooth + diffuse
		params.outVerts[1].mat = float4(0.0f, 0.5f, params.inMaterialID, params.outVerts[0].mat.w);
		params.outVerts[2].mat = float4(0.5f, -0.5f, params.inMaterialID, params.outVerts[0].mat.w);

		params.outVerts[0].normals = float4(0.0f, 0.0f, -1.0f, 0.0f);
		params.outVerts[1].normals = float4(0.0f, 0.0f, -1.0f, 0.0f);
		params.outVerts[2].normals = float4(0.0f, 0.0f, -1.0f, 0.0f);

		*params.outNumVts = 3;

		params.outNdces[0] = 0;
		params.outNdces[1] = 1;
		params.outNdces[2] = 2;

		*params.outNumNdces = 3;
	}
	else
	{
#ifdef _DEBUG
		char indicesPrintable[128] = {};
		char verticesPrintable[128] = {};
#endif

		// Fill-in positions (these vertlists are de-duplicated by default!)
		for (uint32_t i = 0; i < vertsFront; i += vertStride)
		{
			params.outVerts[i / vertStride].pos = float4(verts[i], verts[i + 1], verts[i + 2], 0.0f);

#ifdef _DEBUG
			sprintf_s(verticesPrintable, "file source vertex %u = (%.f, %.f, %.f)\n", i / vertStride, verts[i], verts[i + 1], verts[i + 2]);
			OutputDebugStringA(verticesPrintable);
#endif

		}
		*params.outNumVts = vertsFront / vertStride;

		// Load indices for geometry + attributes (facesStride will indicate whether they index quads or tris)
		auto uvNdces = CPUMemory::AllocateArray<OBJ_AttribVtNdxPair>(facesFront * 2);
		uint64_t numUVNdces = facesFront;

		for (uint32_t i = 0; i < facesFront; i++)
		{
			params.outNdces[i] = faces[i].pos_uv_normal[0] - 1; // OBJ indices are one-based, for some reason >.>

			uvNdces[i] = { faces[i].pos_uv_normal[1] - 1, params.outNdces[i] };
		}

		*params.outNumNdces = facesFront;

#ifdef _DEBUG
		for (uint32_t i = 0; i < facesFront; i += facesStride)
		{
			if (facesStride == 4)
			{
				sprintf_s(indicesPrintable, "source geometry indices (%u-%u) = (%zu, %zu, %zu, %zu)\n", i, i + 4, params.outNdces[i], params.outNdces[i + 1], params.outNdces[i + 2], params.outNdces[i + 3]);
			}
			else
			{
				sprintf_s(indicesPrintable, "source geometry indices (%u-%u) = (%zu, %zu, %zu)\n", i, i + 3, params.outNdces[i], params.outNdces[i + 1], params.outNdces[i + 2]);
			}

			OutputDebugStringA(indicesPrintable);
		}
#endif

		// Split quads if needed
		if (facesStride == 4)
		{
			auto triNdces = CPUMemory::AllocateArray<uint64_t>(facesFront * 2);
			auto uvTriNdces = CPUMemory::AllocateArray<OBJ_AttribVtNdxPair>(facesFront * 2);

			uint32_t triNdcesFront = 0;
			uint32_t uvTriNdcesFront = 0;

			for (uint32_t i = 0; i < facesFront; i += 4)
			{
				const uint64_t geoQuad[4] = { params.outNdces[i], params.outNdces[i + 1], params.outNdces[i + 2], params.outNdces[i + 3] };				
				const OBJ_AttribVtNdxPair uvQuad[4] = { uvNdces[i], uvNdces[i + 1], uvNdces[i + 2], uvNdces[i + 3] };
				
				// How splitting should work
				// "fun" fact: OBJ winding order is backwards (right-handed)!
				// We quietly flip the indices below
				// 
				//    0____1
				//    |\  B|
				// Q =| \  |
				//    |A \ |
				//   3|___\|2
				//

				// Triangulate geometric indices
				const uint triA[3] = { geoQuad[0], geoQuad[1], geoQuad[2] };
				const uint triB[3] = { geoQuad[2], geoQuad[3], geoQuad[0] };

				triNdces[triNdcesFront] = triA[2];
				triNdces[triNdcesFront + 1] = triA[1];
				triNdces[triNdcesFront + 2] = triA[0];

				triNdces[triNdcesFront + 3] = triB[2];
				triNdces[triNdcesFront + 4] = triB[1];
				triNdces[triNdcesFront + 5] = triB[0];

				triNdcesFront += 6;

				// Triangulate UVs
				const OBJ_AttribVtNdxPair uvTriA[3] = { uvQuad[0], uvQuad[1], uvQuad[2] };
				const OBJ_AttribVtNdxPair uvTriB[3] = { uvQuad[2], uvQuad[3], uvQuad[0] };

				OBJ_AttribVtNdxPair* attrib = &uvTriNdces[0];

				attrib[uvTriNdcesFront] = uvTriA[0];
				attrib[uvTriNdcesFront + 1] = uvTriA[1];
				attrib[uvTriNdcesFront + 2] = uvTriA[2];

				attrib[uvTriNdcesFront + 3] = uvTriB[0];
				attrib[uvTriNdcesFront + 4] = uvTriB[1];
				attrib[uvTriNdcesFront + 5] = uvTriB[2];

				uvTriNdcesFront += 6;
			}

			CPUMemory::CopyData(triNdces, params.outNdces);
			CPUMemory::CopyData(uvTriNdces, uvNdces);

			*params.outNumNdces = triNdcesFront;
			numUVNdces = uvTriNdcesFront;

			CPUMemory::Free(triNdces);
			CPUMemory::Free(uvTriNdces);

#ifdef _DEBUG
			for (uint32_t i = 0; i < triNdcesFront; i += 3)
			{
				sprintf_s(indicesPrintable, "triangulated geometry indices (%u-%u) = (%zu, %zu, %zu)\n", i, i + 3, params.outNdces[i], params.outNdces[i + 1], params.outNdces[i + 2]);
				OutputDebugStringA(indicesPrintable);
			}

			for (uint32_t i = 0; i < uvTriNdcesFront; i += 3)
			{
				sprintf_s(indicesPrintable, "triangulated uv indices (%u-%u) = (%zu, %zu, %zu)\n", i, i + 3, uvNdces[i].attrib, uvNdces[i + 1].attrib, uvNdces[i + 2].attrib);
				OutputDebugStringA(indicesPrintable);
			}
#endif
		}
		else
		{
			// OBJ winding order is backwards to what we expect (see above), so fix that up here
			// No other retopo needed for tri meshes ^_^
			for (uint32_t i = 0; i < facesFront; i+= 3)
			{
				uint64_t& ndx0 = params.outNdces[i];
				uint64_t& ndx2 = params.outNdces[i + 2];
				std::swap(ndx0, ndx2);
			}
		}

		// Instead of extracting vertex normals and hoping they're physical, not broken by the import, etc, we can generate them ourselves
		// All we need is loaded geometry + loaded indices
		// Vertex normals are not physical and violate conservation of energy in certain cases, but they're very useful for representing smooth surfaces without ultra high-poly detail
		// (why aren't they physical? Because the interpolated normals can indicate curvature that doesn't exist in the - planar - mesh, causing a disconnect between shading & actual
		// intersections; this can be ignored, or addressed with subdivision surfaces)
		
		// iterate through triangles
		// ...for each triangle, take all triangles sharing an edge and generate their face normals from the cross-product
		// ...for each vertex in each triangle, assign their vertex normal as the average of above face normals

		// Abstract indices into triangles, and resolve face normals
		const uint32_t numTris = *params.outNumNdces / 3;
		auto triMetaBuff = CPUMemory::AllocateArray<TriMeta>(numTris);
		for (uint32_t i = 0; i < *params.outNumNdces; i += 3)
		{
			const uint32_t indices[3] = { params.outNdces[i], params.outNdces[i + 1], params.outNdces[i + 2] };
			float4 corners[3] = { params.outVerts[indices[0]].pos,
					  params.outVerts[indices[1]].pos,
					  params.outVerts[indices[2]].pos };

			vec4 u = vec4Subtract(vec4FromFloat4(&corners[0]), vec4FromFloat4(&corners[1]));
			vec4 v = vec4Subtract(vec4FromFloat4(&corners[0]), vec4FromFloat4(&corners[2]));
			vec4 n = normalize(cross(u, v));

			triMetaBuff[i / 3].indices[0] = indices[0];
			triMetaBuff[i / 3].indices[1] = indices[1];
			triMetaBuff[i / 3].indices[2] = indices[2];
			triMetaBuff[i / 3].normal = n;
		}

		// Compute adjacent tris for each face
		auto vertMetaBuff = CPUMemory::AllocateArray<VertMeta>(*params.outNumVts);
		for (uint32_t i = 0; i < *params.outNumVts; i++)
		{
			VertMeta& vt = vertMetaBuff[i];
			vt.numConnectedTris = 0;

			for (uint32_t j = 0; j < numTris; j++)
			{
				if (i == j) continue;
				else
				{
					const TriMeta jthTri = triMetaBuff[j];

					if (jthTri.indices[0] == i || jthTri.indices[1] == i || jthTri.indices[2] == i)
					{
						vt.connectedTris[vt.numConnectedTris] = j;
						vt.numConnectedTris++;
					}
				}
			}
		}

		// Average face normals for tris touching each vertex, then store
		for (uint32_t i = 0; i < *params.outNumVts; i++)
		{
			VertMeta& metaVert = vertMetaBuff[i];
			Geo::Vertex3D& vert = params.outVerts[i];

			vec4 nAvg = vec4({ 0.0f, 0.0f, 0.0f, 0.0f });
			for (uint32_t j = 0; j < metaVert.numConnectedTris; j++)
			{
				TriMeta tri = triMetaBuff[metaVert.connectedTris[j]];
				nAvg = vec4Add(nAvg, tri.normal);
			}

			float denom = static_cast<float>(metaVert.numConnectedTris);
			nAvg = vec4Div(nAvg, vec4({ denom, denom, denom, denom }));
			nAvg = normalize(nAvg);

			float4FromVec4(&vert.normals, nAvg);
		}
		
		// Load data for UVs (all we want besides position & normals from OBJ - idea is to use OBJ for import, DXRS for continuing edits)
		// Possible corruption here when separate UV/normal indices land on the same vert - most likely won't address that for now, but
		// definitely thinking about it, maybe something for next week - worth testing if corruption appears in practice, too, I don't
		// think it *should* so long as UVs and normals don't contain sudden changes/edges (= are continuous)
		for (uint32_t i = 0; i < numUVNdces; i++)
		{			
			memcpy(&params.outVerts[uvNdces[i].vt].mat, &uvs[uvNdces[i].attrib * uvStride], sizeof(float) * uvStride);
			params.outVerts[uvNdces[i].vt].mat.z = params.inMaterialID;
			params.outVerts[uvNdces[i].vt].mat.w = static_cast<uint8_t>(SCATTERING_FUNCTIONS::OREN_NAYAR); // OBJ imports are always white + smooth + diffuse
		}

		CPUMemory::Free(uvNdces);
	}

	// Free unneeded geometry allocations
	CPUMemory::Free(modelData);
	CPUMemory::Free(verts);
	CPUMemory::Free(uvs);
	CPUMemory::Free(normals);
	CPUMemory::Free(faces);

	// Generate placeholder spectral texture (assumed white)
	*params.outSpectralTexWidth = 1024;
	*params.outSpectralTexHeight = 1024;
	*params.outSpectralTexFootprint = sizeof(MaterialSPD_Piecewise) * *params.outSpectralTexWidth * *params.outSpectralTexHeight;

	*params.outSpectralTexAddr = CPUMemory::AllocateArray<MaterialSPD_Piecewise>(*params.outSpectralTexWidth * *params.outSpectralTexHeight);
	CPUMemory::FlushData(*params.outSpectralTexAddr); // BLINDING, Spectralon white, yay

	// Generate placeholder roughness texture (assumed smooth)
	*params.outRoughnessTexWidth = 1024;
	*params.outRoughnessTexHeight = 1024;
	*params.outRoughnessFootprint = sizeof(float) * *params.outRoughnessTexWidth * *params.outRoughnessTexHeight;

	*params.outRoughnessTexAddr = CPUMemory::AllocateArray<float>(*params.outRoughnessTexWidth * *params.outRoughnessTexHeight);
	CPUMemory::ZeroData(*params.outRoughnessTexAddr);

	// Return/end function - all data loaded/generated
	//////////////////////////////////////////////////
}

void GeoLoader::LoadDXRS(const char* path, MeshLoadParams params)
{
	std::fstream strm(path, std::ios_base::binary);
	const uint64_t pathBytes = std::filesystem::file_size(path);

	auto fileLocal = CPUMemory::AllocateArray<char>(pathBytes);
	strm.read(&fileLocal[0], pathBytes);

	uint64_t fOffset = 0;

	// Load header
	DXRS_Header header = {};
	memcpy(&header, &fileLocal[0], sizeof(header));
	*(params.outNumVts) = header.numVts;
	*(params.outNumNdces) = header.numNdces;

	*(params.outSpectralTexFootprint) = header.spectralTexFootprint;
	*(params.outSpectralTexWidth) = header.spectralTexWidth;
	*(params.outSpectralTexHeight) = header.spectralTexHeight;

	*(params.outRoughnessFootprint) = header.roughnessTexFootprint;
	*(params.outRoughnessTexWidth) = header.roughnessTexWidth;
	*(params.outRoughnessTexHeight) = header.roughnessTexHeight;

	// Allocate memory for spectral/roughness textures
	*params.outSpectralTexAddr = CPUMemory::AllocateArray<MaterialSPD_Piecewise>(header.spectralTexWidth * header.spectralTexHeight);
	*params.outRoughnessTexAddr = CPUMemory::AllocateArray<float>(header.roughnessTexWidth * header.roughnessTexHeight);

	fOffset += sizeof(header);

	// Copy-out verts
	uint64_t vtCtr = header.numVts;
	char* verts = &(fileLocal + fOffset)[0];
	while (vtCtr > 0)
	{
		const uint64_t vtNdx = header.numVts - vtCtr;
		DXRS_Vertex3D vt = reinterpret_cast<DXRS_Vertex3D*>(verts)[vtNdx];

		params.outVerts[vtNdx].pos.x = vt.xyz.x;
		params.outVerts[vtNdx].pos.x = vt.xyz.y;
		params.outVerts[vtNdx].pos.z = vt.xyz.z;
		params.outVerts[vtNdx].pos.w = 0;

		params.outVerts[vtNdx].mat.x = vt.u;
		params.outVerts[vtNdx].mat.x = vt.v;
		params.outVerts[vtNdx].mat.z = params.inMaterialID;
		params.outVerts[vtNdx].mat.w = header.scatteringFunction;

		params.outVerts[vtNdx].normals.x = vt.n.x;
		params.outVerts[vtNdx].normals.x = vt.n.y;
		params.outVerts[vtNdx].normals.z = vt.n.z;
		params.outVerts[vtNdx].normals.w = 0;

		vtCtr--;
		fOffset += sizeof(DXRS_Vertex3D);
	}

	// Copy-out ndces
	char* ndces = &(fileLocal + fOffset)[0];
	uint64_t ndxFootprint = sizeof(uint32_t) * header.numNdces;
	memcpy(params.outNdces, ndces, ndxFootprint);
	fOffset += ndxFootprint;

	// Copy-out spectral data
	char* spectra = &(fileLocal + fOffset)[0];
	CPUMemory::CopyData(spectra, *params.outSpectralTexAddr);
	fOffset += header.spectralTexFootprint;

	// Copy-out roughness data
	char* roughness = &(fileLocal + fOffset)[0];
	CPUMemory::CopyData(roughness, *params.outRoughnessTexAddr);
	fOffset += header.roughnessTexFootprint;
}
