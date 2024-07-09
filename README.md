# DXRSandbox

DXRSandbox is mostly a sandbox for learning about DXR and testing DXR techniques, but I also plan to use it as a general sandbox for GPU graphics experiments (building these engines is hard!).

The project should have three rendering pathways; compute-only using software AS setup and raytracing, hybrid using hardware AS (through ray queries) for secondary rays and rasterization for primary, and "ray-tracing" using DXR shader tables for everything.
After implementing all three pipelines (first major goal), I would like to move towards the following:

- Brush-based material editor with its own filetype for models (DXRS), supporting spectral colors and PBR materials (oren-nayar diffuse, smith/ggx glassy & metallic), using imgui for editor UI
- Full support for scene editing (+ scene export)
- Realistic camera behaviour + atmospheric scattering for environment light + good filter behaviour on camera movement (either using reprojection with image flow, hard, or filter reset, quick/dirty)
- Expose the camera's color-matching function for easy user color-grading

In the very long term I would like to create a compute library that takes inline kernels written with C++ and transforms them into runtime-compiled HLSL - that would sit above this and basically access its compute features through an abstraction layer.
But it's an extreme long-distance goal, I want to get the editor sorted before I start on that.
