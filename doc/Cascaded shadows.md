# Cascaded sun shadows

Sun shadows use a dedicated depth texture array, with four 1024-pixel layers by default. The legacy atlas-based CSM and
comparison switch are removed. Point and spot lights retain their atlas, allocation, bias settings, and filtering behavior.
Texture arrays require OpenGL 3.0 or `GL_EXT_texture_array` with layered framebuffer attachments.

## Implementation

`cascadedshadowmap` in `src/engine/renderlights.cpp` owns allocation, cleanup, split distances, projection fitting, culling,
and shader parameters. `rendercsmshadowmaps` renders world geometry, models, and item sprites into each layer. Transparent
world geometry writes transmission into a matching color array; both arrays are filtered together in deferred lighting.

Practical splits mix uniform and logarithmic distances between the greater of camera near distance and `csmnearplane`,
and the lesser of camera far distance and `csmfarplane`. Each succeeding slice includes the preceding transition region.
The active camera FOV and aspect ratio determine the eight slice corners, including zoom and environment-map views.
The minimap uses its orthographic world bounds.

Each projection uses a rotation-invariant bounding sphere for its XY extent. The radius is rounded up to 1/16 world unit,
with a guard for PCF, receiver bias, and snapping. The center snaps to a world-anchored texel grid using double-precision
origin arithmetic. Extents remain fixed during camera translation and rotation; FOV, aspect ratio, range, resolution,
and bias-setting changes can resize them. Light-space corner bounds determine depth coverage, extended toward the sun
for off-screen casters and bounded by the occupied world. Each cascade has its own depth transform.

`config/glsl/deferred.cfg` selects cascades by camera-space depth, then blends adjacent visibility results in the transition
band. The last cascade fades to unshadowed sunlight at the configured range. Bias uses a small constant expressed in world
texels, rasterizer slope offset, and an angle-dependent normal offset expressed in world texels. Surface normals come from
position derivatives rather than normal maps. Degenerate derivatives fall back to the G-buffer normal. Depth discontinuities
and MSAA edges still need visual review.

Filtering uses conventional PCF: one comparison with `smfilter 0`, four bilinear comparisons for a 3x3 footprint with
`smfilter 1` or `2`, and nine for a 5x5 footprint with `smfilter 3`. Receiver-plane correction adjusts comparisons at the tap
positions. Increasing filter quality does not increase bias. Sun shadows do not use PCSS or texture gather; `smgather`
continues to control the existing local-light filters. Removing their unused CSM receiver-plane arguments leaves their
sampling calculations unchanged.

## Controls

| Variable | Default | Meaning |
| --- | --- | --- |
| `csmshadowmap` | 1 | Enable sun shadows |
| `csmsplits` | 4 | Cascade count, 1–8 |
| `csmmaxsize` | 1024 | Resolution per layer, independent of local-light atlas size |
| `csmsplitweight` | 0.75 | Logarithmic split weight, 0–1 |
| `csmnearplane` | 1 | Lower bound on split near distance |
| `csmfarplane` | 1024 | Maximum sun-shadow distance |
| `csmtransition` | 0.1 | Transition width as a fraction of each cascade's depth interval |
| `csmcastermargin` | 1024 | Maximum extension toward the sun for off-screen casters |
| `csmconstantbias` | 0.03 | Constant receiver bias in world texels |
| `csmslopebias` | 1 | Rasterizer slope multiplier; slope is depth change per shadow texel |
| `csmnormalbias` | 0.4 | Maximum normal offset in world texels, reached at grazing angles |
| `csmcull` | 1 | Cull casters against each cascade's side planes |
| `csminoq` | 1 | Retained scheduling of sun shadows while occlusion queries are pending |
| `debugcsm` | 0 | 1: cascade tint; 2: transition tint; 3: depth-layer thumbnails |

All debug modes show split and transition distances, world extents, resolution, texel sizes, and bias values in the HUD.
In mode 1, colors identify cascades and fade toward white in the transition regions. Mode 2 highlights those regions orange.
Mode 3 shows all configured depth layers, ordered left to right and then upward from the bottom row.

`smdepthprec`, `smalphaprec`, `smalpha`, and `alphashadow` remain applicable. Resolution/count changes recreate the arrays;
renderer cleanup and precision/filter changes release them too.

Removed controls: `csmpradiustweak`, `csmdepthrange`, `csmdepthmargin`, `csmpolyfactor`, `csmpolyoffset`, `csmbias`,
`csmpolyfactor2`, `csmpolyoffset2`, and `csmbias2`. Remove these from personal configuration files if present. Their old bias
units are not compatible with the new controls. `avatarshadowbias` remains a local-light setting and no longer offsets sun
shadow lookups.

## Validation status

No build, shader compilation, app run, or tests were performed, as requested. Visual quality and performance are unverified.
For manual review, move and rotate slowly around fences, trees, voxel edges, and contact shadows; cross cascade boundaries;
try low sun angles, zoom, the minimap, transparent geometry, and MSAA. Compare filter levels and cascade counts, and check
point/spot shadows. Use the existing shadow-map timer to assess performance. Casters farther toward the sun than
`csmcastermargin` can be clipped; increase that range when the scene requires it.
