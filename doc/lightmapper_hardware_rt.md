# Hardware RT lightmap baking

`rendering/lightmapping/bake_performance/use_hardware_raytracing` defaults to
`true`. The lightmapper checks `SUPPORTS_RAY_QUERY` on its actual local baking
device. Supported devices use hardware triangle traversal; other devices keep
using the existing compute tracer. In this fork, ray-query support is currently
provided by Vulkan. This does not add RT support to the D3D12 or Metal drivers.

## Integration

- Existing RenderingDevice BLAS/TLAS APIs build one BLAS per unique model-space
  geometry and a TLAS instance per mesh, with its bake-space transform. Each
  BLAS has up to two indexed geometries: opaque and alpha-scissor triangles.
  Identical geometry shares its BLAS even with different transforms, UV packing
  or materials. A map indexed by instance, geometry and primitive translates
  hits back to the slice-sorted world-space triangle/material/UV records.
  Alpha-blended non-cutout surfaces retain their post-hit transmission behavior.
- RT build inputs contain only float3 positions and uint32 indices. The separate
  world-space attribute buffer remains necessary for rasterization, material
  lookup and shading. CPU geometry arrays are released
  after upload; output atlas metadata is retained.
- Compute shaders use `GL_EXT_ray_query` with a TLAS at set 0, binding 16, and
  the triangle-ID map at binding 17. The RT variant uses GLSL 4.60; the original
  software variant remains at 4.50.
  There is no new raygen/miss/hit shader pipeline or shader binding table.
- Vulkan skips its optional re-spirv optimizer for SPIR-V modules declaring
  ray-query or acceleration-structure types, which that optimizer does not yet
  support. The original module goes to Vulkan, including native specialization
  constants. Other modules retain their existing optimizer path; mixed-stage
  shaders retain an empty optimizer entry for each skipped stage.
- The C++ `RenderingDevice::tlas_build` API accepts an optional
  `p_ray_queries_only` argument. Only this mode permits instances without a hit
  SBT range and writes SBT offset zero. Existing calls default to `false` and
  retain their original SBT validation. Script bindings are unchanged.
- RenderingDevice's existing resource graph handles input upload, BLAS build,
  TLAS build and compute-read dependencies. The initial setup submit/sync is
  retained where needed. The TLAS and tracing-only resources are released after
  denoising's final unocclusion query, before dilation and seam repair. Temporary
  BLAS/input buffers are released on completion, cancellation and fallback.

## Geometry reuse between bakes

The independent local bake device retains only the latest scene's unique
BLAS and their minimal input buffers. A SHA-256 key covers model-space position
bytes, grouped indices and opaque/cutout counts. Geometry or cutout classification
changes invalidate an entry; transform changes rebuild only the TLAS. Threshold
or texture changes within the same classification use the current material data.
Removed geometry is evicted before replacement allocation. Devices created with
a private fallback context retain per-bake ownership instead of using this cache.

At every bake exit, a local-device checkpoint awaits GPU work, removes transient
resources and releases retained BLAS build scratch. Cached BLAS and their inputs
consume VRAM between bakes intentionally, avoiding repeated builds. The module
clears the cache on shutdown. Concurrent bakes never borrow an already active
cached device. Logs report instance count, unique BLAS, builds and cache hits.

## Preserved bake behavior

The replacement is at `trace_ray`, beneath direct lighting, shadowmask,
indirect bounces, sky/emission, probes, ambient occlusion and unocclusion. All
material evaluation, transparency retries, UV packing, rasterization, SH
coefficients, sampling counts, denoisers, seam repair and saved resources use the
existing code. The runtime renderer and hardware requirements of baked games do
not change.

The first ray query returns only cutout candidates to the shader, rejecting holes
before committing a hit. Other triangles are committed by hardware. Any-hit
visibility terminates at an accepted cutout or hardware-committed non-cutout.
Opaque intervals exclude the strict original ray endpoints by one float ULP.
Closest-hit results preserve geometric normal, barycentrics, material
culling and the existing front-face distance bias. If the nearest geometric hit
is a back face, a second query examines only the one-bias-unit tie range to find
front faces that should win. This avoids losing the existing behavior for
coincident front/back geometry. Floating-point intersection differences and
ambiguous equal-distance ties can still produce small output differences.

AO uses a scalar-opacity/position query and preserves the original distance
falloff and transparent-entry/exit accounting. An accepted cutout has alpha one,
so AO avoids fetching its material again. Other materials still sample opacity;
AO is not replaced with binary any-hit occlusion.

## Fallback and cost

Disabling the setting forces the original tracer. Unsupported devices skip RT
allocation. Input/AS allocation or build failures fall back automatically. Failed
RT shader parsing, pipeline creation or descriptor setup releases that attempt
and retries the software variant. Verbose output reports the selected backend;
setup failures also emit a warning.

Software grid/cluster data is built only for software tracing. A successful RT
setup omits software grid/cluster descriptors and allocations completely.
Tracing, raster and postprocessing sets use their own shader layouts. If RT shader, pipeline or binding setup fails,
the original vertices and triangles are downloaded from existing bake buffers
and the software grid is built on demand. Its bindings and raster uniform set
are rebuilt before retrying software shaders. Materials, UV packing and probes
are not regenerated. RT input/AS failures build the software grid directly.

`rendering/lightmapping/bake_performance/max_passes_per_submit` defaults to two
and is clamped to 1–8. Direct-light tiles, indirect-light passes and probe passes
can share a submission when RT is active. Every partial final batch is drained
before readback or resource reuse. The resource graph retains dependencies
between dispatches, and the work/sample count in each dispatch is unchanged.
Progress and cancellation are checked after completed batches. Set the value to
one for per-dispatch synchronization; software tracing always uses one. Larger
batches increase work between cancellation checks. An empty setup submit/sync
was removed; remaining uploads are ordered with the first lighting dispatch.

Device loss or a GPU execution failure after dispatch is not recoverable by
setup fallback. Bake speedups have not been measured.

## RT dispatch efficiency

`rendering/lightmapping/bake_performance/max_ray_passes_per_dispatch` defaults
to four and is clamped to 1–8. For hardware indirect lighting, several logical
ray passes share one compute dispatch and one evaluation of texel position,
normals and world-space footprint. Each logical pass restarts its original RNG
stream and writes its own rgba16f accumulation result. An image memory barrier
orders subsequent reads by the same invocation; uncovered invocations can return
without requiring a workgroup barrier. Samples, bounces, invalid-sample counters,
SH coefficients and material atlases are retained. Software uses one pass.

A conservative CPU mask derived from packed triangle UV bounds skips empty
indirect-light tiles without downloading geometry textures. Bounds include an
eight-pixel guard for raster coverage and partial workgroups. Non-finite UVs mark
the entire slice conservatively. The mask is built before CPU geometry is freed;
RT shader fallback ignores it. No occupancy readback or new GPU buffers are used.

Logs include rays per dispatch, occupied/total tiles, executed dispatches and
submissions. Four-ray passes fused four at a time require 32 rather than 128
indirect dispatches per occupied tile at 512 samples. This is a reduction in
command count, not a measured 4x speedup. Set the fusion setting to one for
comparison. Longer dispatches increase the time between cancellation checks.

## Comparing repeated indirect bakes

Each indirect bake prints `Lightmapper indirect start` and `Lightmapper indirect
end` lines. The start line records the backend, device, atlas dimensions/slices,
sample count, bounces, submission batch size and tracked memory. The end line
separates wall time spent recording/submitting/waiting for dispatches, progress
callbacks, and other work (including slice preparation and file/readback work).
`dispatch_wait` is not a GPU timestamp and includes scheduling/queue contention.
Tracked memory is the RenderingDevice accounting, not complete driver VRAM usage;
the GPU budget is a budget, not the amount of free memory.

Compare these lines from the first and second bake of an unchanged scene. A
backend change, different atlas/sample count, increased tracked renderer memory,
callback cost, or increased dispatch wait are separate hypotheses. A larger wait
alone does not identify thermal throttling, driver scheduling or memory pressure.
The diagnostics add no synchronization and do not change dispatch/sample counts.

## Material evaluation

Materials use the existing renderer-evaluated UV2 images, blitted into global
albedo and emission atlases. Both hardware RT and the software tracer sample
these atlases. There are no resident-material descriptor arrays, generated
texture-selection switches, source-image leases or shared native Vulkan devices.
The experimental `use_mesh_materials` setting and its implementation are removed.
Bake vertices use the original 32-byte position/normal/UV2 layout.

## CPU probe volume construction

Probe-volume generation remains CPU Delaunay tetrahedralization followed by BSP
construction, with the same saved tetrahedron indices and six-word BSP node
format. Quantization, R128 circumsphere predicates, interpolation coefficients
and runtime capture queries are unchanged.

The Delaunay duplicate pass indexes already-quantized positions, keeping each
position's last original index and inserting the retained points in their original
order. The temporary hash table is dropped before tetrahedron insertion.

Volume preparation indexes shared faces by sorted vertex IDs and records their
two incident tetrahedra temporarily. Faces with more than two incident tetrahedra
do not use adjacency shortcuts. Each oriented face plane is computed once using
its original vertex order, retaining the precision behavior of BSP candidates.
Approximate plane deduplication searches nearby normal/distance buckets, then
applies the original comparison and selects the lowest matching index. Distance
keys use a signed logarithm to accommodate relative tolerances at large world
coordinates; common early planes and small/non-finite cases retain a linear path.

BSP candidate scoring caches each point's original coplanar/over/under result per
plane generation, reusing it across incident tetrahedra. Verified neighboring
pairs can split on their shared face directly, and a perfect balanced split ends
candidate search. Both shortcuts keep the original tolerance and leaf encoding;
the internal tree may differ while the tetrahedra and interpolation are retained.
The pre-existing degenerate fallback now computes every tetrahedron center,
initializes the aggregate bounds and stores global rather than local leaf IDs.

`Lightmap probe volume` logs report probe/tetrahedron/shared-face/plane/node counts
and separate Delaunay, topology preparation and BSP wall times. Unchanged probe
positions retain the existing volume cache; verbose output identifies reuse.
All face, adjacency, plane-index and point-cache data is temporary and adds no
saved-resource or runtime-memory requirements.

## Validation without compilation

No engine, shader or helper program was compiled, and no GPU bake was executed.
Checks include source/API review, formatting/includes/whitespace, XML parsing,
preprocessor/delimiter checks, unchanged-software-grid-algorithm comparison,
5,000 instanced BLAS mapping cases, 5,000 geometry-key invalidation cases,
18 tracing/postprocessing descriptor partitions, 30,000 front/back/cutout hit reference cases, 10,000 AO
falloff/transmission cases, strict ray interval endpoints and 792 bounded-batch
reference cases. Reference cases validate the algorithms, not GPU execution. Material atlas and
raster code were compared against the original implementations.

Dispatch fusion was additionally checked with 1,408 sample-range partitions,
1,000 variable-RNG/half-precision accumulation reference cases and 10,000
conservative UV tile cases. These checks cover logical pass boundaries, tail
submission draining, invalid-sample counts and the unchanged 48-byte push ABI;
they do not establish GPU performance or shader compilation success.

CPU probe-volume changes were checked with 5,000 quantized-point index cases,
20,000 approximate-plane bucket boundary cases, 2,400 insertion-order plane
lookups, shared/non-manifold face ownership, point-cache generation wraparound,
and 2,160 interior queries through original/optimized reference BSPs. Delaunay
predicates and the insertion body, as well as saved BSP packing, were compared
against the original source. These are static/reference checks, not engine bake
measurements. Runtime validation should include large translated scenes, dense
probes, geometry close to coplanarity and rebakes with unchanged positions.

Remaining runtime validation: compare hardware on/off using opaque and
transparent materials, alpha scissor, all culling modes, overlapping front/back
faces, direct lights/projectors/area lights, sky/emission, directional lightmaps,
probes, shadowmask, AO, both bounce lookup modes and both denoisers. Compare raw
high-sample energy before denoising, then noise and elapsed time. Test cancellation,
repeated bakes and resource usage.

## Resource and preparation cleanup

A single `BakeDeviceGuard` releases transient device resources on every bake exit,
including cancellation and denoiser errors. The local-device cleanup retains only
cached BLAS and their input buffers. Intermediate resource releases remain in
place to reduce peak memory during denoising and postprocessing. Error paths no
longer duplicate texture/buffer/shader cleanup macros. Scratch release is a
private RenderingDevice implementation detail.

RT preparation preallocates index, instance, and primitive-map arrays and writes
through stable pointers. Cached BLAS inputs retain counts and indices needed for
hit mapping, but drop CPU position uploads immediately after hashing. Hit mapping
and geometry hash contents are unchanged. The unused probe-generation parameter
was removed from the internal acceleration-setup helper; the public bake API is
unchanged.

R11G11B10 packing reads source half bits directly, preserving the former clamp,
subnormal flush, NaN handling and mantissa rounding. Exhaustive Python reference
checks compared all 65,536 half patterns at both mantissa widths. Another 5,000
cases compare the preallocated RT map byte-for-byte with the former append-based
layout. Checks also cover bake exit paths, guard/context lifetime ordering and
intentional intermediate frees. No compilation or GPU timing was performed.
