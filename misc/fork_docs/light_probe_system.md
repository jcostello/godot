# Light probe system implementation

## Stage 1: persistent, editable placement

Create a LightProbeSystem and add LightProbeVolume children. Select a volume,
set its Size and Spacing, and use Generate Grid in the 3D toolbar. Each volume
automatically uses its nearest LightProbeSystem ancestor, including through
intermediary grouping nodes. There is no manual System property.

Use the six box handles to resize the generation volume, with the same snapping,
Alt-centered resizing, cancellation and Undo/Redo as ReflectionProbe. This changes
the box, not the generated layout; Generate Grid remains explicit.

Probes are displayed only while their volume or nearest ancestor system is
selected. Selecting a system shows its enabled volumes; selecting LightmapGI
shows no probe spheres or tetrahedron edges. The probe toolbar provides Wireframe
and Baked Lighting modes. Baked Lighting uses actual capture SH coefficients,
normalized by bake exposure and evaluated with current camera exposure. Invalid
points remain red and points without a matching baked position stay wireframe.
Selected points keep a wire highlight over the illuminated sphere.

The preview uses a shared sphere mesh, a MultiMesh and a packed RGBA-float SH
texture per volume instead of separate meshes/materials per probe. Negative SH
coefficients and HDR energy are retained. Each system resolves its nearest
LightmapGI ancestor. There is no stored source path or scene-wide fallback.
Old `_bake_source` properties are accepted on load but ignored. Existing scenes
must place systems below their LightmapGI; their editable and baked resources
are retained when reparenting.

Select individual point subgizmos in the 3D view and use the usual transform
controls to move them. Box selection supports editing several points together.
Add Probe creates a point at the volume origin and selects it. Remove Selected
Probes removes only selected points. Toolbar operations and gizmo movement
support Undo/Redo. When custom or moved probes exist, Generate Grid asks whether to keep them or
replace all positions, with Cancel available. Both options are undoable. Preserved
moved probes keep their original slot reserved so generation does not recreate a
second point there. Changing generation settings alone never regenerates it.

LightProbeVolumeData stores positions, stable per-volume integer IDs, and the
next-ID allocator and generated-position provenance in one validated snapshot. Edits, removal, Undo/Redo and saving
preserve surviving IDs. The allocator never rolls back within a resource
instance, preventing accidental reuse after undo. Resource revisions and system
revisions are monotonic within the current session; they are not bake hashes.
Resources are local to scenes and always duplicated when their volume node is
duplicated. Sharing a resource explicitly still shares edits intentionally.

Axes smaller than spacing generate one position at their center. Other axes
include both box endpoints with ceil(size / spacing) intervals, so the actual
spacing is never greater than requested. Equality generates two endpoints.
Depending on the dimensions, this produces a single probe, a centered row, a
plane, or a volume. Geometry filtering then discards invalid generated points
without shifting them; explicitly preserved custom or moved probes remain. A 65,536-position limit is checked before
allocation. Non-finite input, duplicate IDs, invalid ID ranges and malformed
snapshots are rejected before replacing any existing data.

The system gathers enabled linked volumes in deterministic path order and
transforms their points into its own local coordinates. The snapshot retains
local IDs and indices into a unique volume-path table, including coincident
placements. This avoids storing a NodePath for every probe. Placement changes
are coalesced through a deferred signal rather than triggering expensive work
inside mouse-drag callbacks. Node references use ObjectID for safe deletion and
scene switching. Node paths identify volume namespaces in this first stage;
renaming or reparenting a volume invalidates the system revision.

## Bake-time validation and dynamic capture

LightmapGI defaults to baking lightmaps and probes together. The bake toolbar
also provides Lightmap Only and Probes Only modes. Probe positions come
only from enabled LightProbeVolume nodes under LightProbeSystem nodes in the
bake scope. Without these nodes no probes are baked. The legacy automatic
subdivision, corner probes and LightmapProbe collection have been removed. Old
stored subdivision settings are ignored on load. Each system belongs to its nearest
LightmapGI ancestor and receives its own LightProbeData capture, independently of
texture-lightmap storage and other systems.

A bake-local CPU BVH snapshot covers static visible meshes, including meshes
without UV2, and generated bake meshes. Probes are checked in world coordinates.
Closed, consistently oriented components reject interior positions; open
components only enforce surface distance. Surface Clearance defaults to 0.05
world units, with a minimum numerical tolerance of 0.0002. Transparent/cutout
BaseMaterial3D surfaces are skipped because their opacity cannot be inferred
without texture sampling. ShaderMaterial surfaces are conservatively opaque.
Malformed, self-intersecting, or inconsistently oriented geometry cannot provide
a reliable solid interior; meshes should use closed manifold components when
interior rejection is required. Conflicting ray results reject the probe.

Rejected positions retain their IDs and remain selectable, movable and removable.
Their wire spheres turn red, the volume has a scene-tree configuration warning,
and its toolbar reports counts by reason. Scripts can query individual reasons.
There is no modal validation warning. Editing placement retains the last colors
by stable ID, including invalid probes, and labels the summary as stale;
validation runs again at the next bake. Explicit grid generation also filters new
points against the same geometry checks; interactive dragging does not.
Geometry edits are picked up at the next bake; previous colors describe the last
validation and do not monitor live mesh edits.

Only accepted positions reach the lightmapper and Delaunay capture.
This makes their baked lighting available to dynamic meshes through LightmapGI.
Fewer than four non-coplanar valid positions cannot form an interpolation volume;
lightmap textures can still bake and a console warning explains missing coverage.

Generate Grid builds one geometry snapshot for the edited scene, then excludes
new generated points inside closed static opaque components, near surfaces, or
with ambiguous interior tests. Preserving custom/moved probes keeps their exact
positions even if invalid; the bake revalidates them. Filtering is cancellable
and commits only a complete unchanged-layout snapshot. Surface/transparent/open
mesh limitations match bake validation. Older saved layouts without provenance
are conservatively treated as custom when regenerating.

Remove Invalid Probes removes the current IDs marked invalid by the last check.
It intentionally uses the visible red markers, including moved stale markers.
Undo restores both placement and last-result colors; rebaking replaces the result
snapshot. This remains transient per-volume state and is not saved in the resource.

## Incremental capture topology

After a successful bake, a small placement edit attempts a local topology update
at the next bake. Adding an interior point replaces its Delaunay cavity. Removing
an interior point retriangulates its incident tetrahedra from their neighbors.
Moving a point removes its old position and inserts its new position. Position
and index remapping also support Undo/Redo and probe-array reorderings.

An update is accepted only when the cavity boundary, volume, non-degeneracy and
empty-circumsphere checks pass. Hull/extent changes, quantized duplicates, large
edits (over 32 removals/insertions), large removal neighborhoods or uncertain
geometry fall back to a full solve. Failed attempts do not publish partial
results. This optimizes topology at bake time; editing never triggers geometry
validation or replaces active baked lighting. Lighting samples are still rebaked;
this does not introduce an independent or incremental lighting baker.

New-system captures store tetrahedra with an empty legacy BSP. Both RenderingDevice
and Compatibility renderers use a shared runtime BVH over a flat cell array, rebuilt
whenever the capture data changes. The BVH is reconstructed from tetrahedra when
loading a resource; no raw pointers or tree nodes are serialized.
Barycentric interpolation samples containing tetrahedra. Outside coverage,
lighting extends from the closest tetrahedron surface. Captures without any
tetrahedra blend up to four nearest probes with inverse-squared-distance weights,
supporting rows, planes and single probes. Connectivity and lookup do not test
geometry. Previously saved BSP trees are ignored; those captures use the same
tetrahedra lookup. These captures require this engine branch to use their runtime BVH.

Dynamic-object capture coefficients are divided by the LightProbeData bake
exposure before captures are blended. Forward+, Mobile and Compatibility then
apply the current camera exposure in the material shader, independently of the
sky exposure. Existing captures need no rebake for this correction. Baked AO and
lightmap color adjustments remain separate from dynamic-object capture lighting.

Capture arrays are retained independently of atlas unloading so a repeated bake
can reuse topology, including across editor restarts through saved capture data.
The previous capture resource is reused to retain the runtime BVH. Unchanged
empty tetrahedron sets (rows, planes or a single probe) are also reusable; empty
topology alone does not require reconstruction. CPU capture,
user mappings and flags are backed up for restoration on failed publication.

To measure a first topology bake, select a LightProbeSystem or one of its volumes
and press **Clear Probe Cache** in the probe toolbar. This clears reuse for the
capture associated with that system. The next bake bypasses saved topology and rebuilds its
runtime BVH. Existing baked lighting stays available; the flag is transient and
is consumed only by a successful bake that includes probes, so cancellation/failure keeps it pending.
This button targets probe topology, not RT geometry or shader caches.

## Partial bakes

The LightmapGI toolbar offers Lightmap + Probes, Lightmap Only and Probes Only.
All bake controls belong to LightmapGI. Probe toolbars provide only placement,
preview, validation and cache tools. Each system belongs to its nearest
LightmapGI ancestor, independent of the scene geometry bake scope. Nested
LightmapGI nodes own their own systems. Probe ray counts use the project quality
preset selected by LightmapGI.

Lightmap Only skips placement validation, probe rays and topology updates. It
retains existing capture bounds, positions, topology and SH lighting. Existing
captures are marked stale in their own LightProbeData resource; a tree configuration warning
on each associated system asks for a probe refresh, without opening a popup.
Probes Only retains texture resources, shadowmasks, directional packing and mesh
atlas assignments. It skips lightmap indirect integration, AO, denoising,
dilation, seam blending and texture export. Scene geometry, material atlases and
direct-light lookup preparation are still required by the shared tracer. It does
not require an earlier texture bake, but still requires a LightmapGI node and
static geometry with UV2 for the current backend.

Texture lightmaps and probes have independent resources and exposure references.
Probe-only bakes save .tres without changing .lmbake, textures, shadowmasks
or mesh mappings. Lightmap-only bakes retain the exact SH/positions/topology and
probe exposure, updating only their stale flag. No SH rebasing is needed.

The save dialog captures the requested mode and target by ObjectID; switching
selection does not redirect a pending request. Scene changes cancel its pending
dialog. Failed/canceled publication restores in-memory capture data, mappings,
texture references, exposure and stale status; probe cache invalidation remains
pending until a successful probe bake. Preview refresh and new bake associations
are published only after a successful resource save.

## Independent probe resource and runtime ownership

LightProbeData (.tres) owns positions, signed HDR SH, tetrahedra, bounds,
interior mode, position hash, bake exposure and stale status. It owns a renderer
capture RID and uses the existing RD/Compatibility interpolation. Editable
LightProbeVolumeData remains separate. New lightmap resources store no probe
arrays; the legacy payload is retained only while required for old captures.
LightmapGIData stores texture exposure independently.

Each system receives a separate resource and topology. Resource change signals
refresh each subscribing system directly; no shared host election or registry
is needed. All of its volumes feed
that topology; different systems never share tetrahedra. Lighting rays for all
systems still use one scene preparation and tracing session. Results are split
by stable insertion ranges, and each system reuses its own topology cache.
Capture bounds follow that system's accepted positions, including a small margin
for thin or single-point layouts. Overlapping system captures may blend in the
renderer. A system without a LightmapGI ancestor submits no capture.
Legacy resources shared between systems are split on the next successful probe
bake. Capture transforms remain stored relative to systems.
Quantized placement/ID/volume/clearance signatures mark edited layouts stale;
lightmap-only bakes also mark stale. Geometry/light edits still need a new bake;
there is no live scene-wide material/geometry dependency watcher.

Existing embedded captures migrate on load into a shared in-memory LightProbeData
without writing files. A session-only migration reference supports repeated
instances of old packed scenes. Legacy tetrahedra feed the runtime BVH. The old
renderer capture is disabled after transfer to avoid double lighting. The next
bake saves the independent resource and scene links; save the scene afterward.

Capture candidates validate on CPU and upload only after publication. Grouped
metadata saves first back up every destination, restoring files on a save error.
Probe resources and node associations publish only after all resource saves
succeed; existing RIDs are retained to reuse unchanged BVH cells. This protects
resource metadata; texture-image export retains its existing bake behavior.
The topology cache invalidation request belongs to LightProbeData and is consumed
only after a successful probe bake. Lightmap-only saves do not consume it.

## Ownership and verification

LightmapGI is the only bake coordinator. LightProbeSystem handles its lighting
resource, cache and warnings; LightProbeVolume handles placement. The three bake
modes retain separate texture/probe exposure and resource persistence.

No compilation or editor/GPU execution was performed. Static and reference checks
do not replace validation in the running editor.

Scene compatibility with custom/master: probe bakes use the standard .tres
extension so an older editor can load them as missing resources. Probe nodes and
volume resources remain serialized with their original class names and can be
represented by editor placeholders; their lighting is unavailable in that editor.
Saving a scene in this branch embeds any legacy .probebake capture still referenced
by its probe systems, without deleting the old bake file or requiring a rebake.
This does not provide probe support in older exported games. Saving placeholders
back from an older editor has not been verified.
