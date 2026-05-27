---
description: Native 3D rendering in rooture via glDrawElements on TGLSAViewer. Use when the user wants 3D meshes, procedural geometry, torus/sphere rendering, or any GL scene that bypasses TEve.
---

# rooture-gl

Always invoke `rooture-lang` first for general language context.

---

## Overview

Rooture has a lightweight 3D engine (`rut_gl.cxx`) that renders triangle meshes
directly via `glDrawElements` on ROOT's `TGLSAViewer`, bypassing TEve entirely.
`RutGLMesh` subclasses `TGLLogicalShape` with a compiled vtable — no Cling JIT
per vertex.

## Builtins

| Builtin | Signature | Description |
|---------|-----------|-------------|
| `gl-viewer` | `(gl-viewer)` or `(gl-viewer "title")` | Create scene + standalone viewer, return context |
| `gl-mesh` | `(gl-mesh verts indices)` or `(gl-mesh verts indices normals)` | Create mesh from float32 vertices + int32 indices |
| `gl-add` | `(gl-add ctx mesh [r g b a])` | Adopt mesh into scene; returns `TGLPhysicalShape*` |
| `gl-redraw` | `(gl-redraw ctx)` | Trigger `RequestDraw` on viewer |
| `gl-reset-camera` | `(gl-reset-camera ctx)` | Reset camera to frame all objects |
| `gl-clear` | `(gl-clear ctx)` | Destroy all shapes in scene |
| `gl-auto-rotate` | `(gl-auto-rotate ctx [dt wphi wtheta atheta])` | Start auto-rotation |
| `gl-frame` | `(gl-frame ctx)` | Get `TGCompositeFrame*` (for screenshots) |

## Geometry column helpers

| Builtin | Signature | Description |
|---------|-----------|-------------|
| `col-interleave` | `(col-interleave cx cy cz)` | Interleave 2–4 same-length, same-dtype columns: `x0 y0 z0 x1 y1 z1 ...` |
| `col-gen-tri-grid` | `(col-gen-tri-grid nR nr)` | Triangle indices for nR×nr grid with wraparound (torus/sphere tessellation). Output: int32 column of 6·nR·nr indices. |

## Typical workflow

```scheme
;;; 1. Generate vertex positions via columnar ops
(def {idx} (col-iota nVerts))
(def {theta} (col-map-ptr theta-ptr idx "float"))  ; "float" output type!
(def {vx} (col-zip-ptr vx-ptr theta phi))
(def {vy} (col-zip-ptr vy-ptr theta phi))
(def {vz} (col-zip-ptr vz-ptr theta phi))

;;; 2. Interleave into packed vertex buffer
(def {vertices} (col-interleave vx vy vz))

;;; 3. Generate triangle indices
(def {tri-idx} (col-gen-tri-grid nR nr))

;;; 4. Create viewer, mesh, add to scene
(def {ctx} (gl-viewer "My Scene"))
(def {mesh} (gl-mesh vertices tri-idx normals))
(gl-add ctx mesh 0.2 0.6 0.9 1.0)
(gl-reset-camera ctx)
(gl-redraw ctx)
```

## GLSL shaders

Meshes support optional GLSL vertex+fragment shaders for GPU-side animation.
`RutGLMesh` disables ROOT's display-list caching (`fDLCache = kFALSE`), so
`DirectDraw` runs every frame — shaders see updated uniforms each time.

| Builtin | Signature | Description |
|---------|-----------|-------------|
| `gl-shader` | `(gl-shader mesh vert-src frag-src)` | Attach GLSL vertex + fragment shader sources (compiled lazily on first draw) |
| `gl-set-float` | `(gl-set-float mesh "name" value)` | Set a float uniform on the mesh's shader |

A built-in `uTime` uniform is auto-updated from the wall clock (seconds since
mesh creation) on every frame.  No need to call `gl-set-float` for it.

### Shader compilation

Shader sources are stored as strings.  Compilation is **deferred to the first
`DirectDraw` call** where the GL context is guaranteed active.  Uniform
locations are resolved at the same time.

### Example: traveling wave

```scheme
(def {vert-src} "
  uniform float uTime;
  uniform float uR;
  uniform float ur0;
  uniform float uAmp;
  varying vec3 vNormal;
  varying vec3 vPos;
  void main() {
    vec3 pos = gl_Vertex.xyz;
    float theta = atan(pos.y, pos.x);
    vec3 center = uR * vec3(cos(theta), sin(theta), 0.0);
    vec3 radial = pos - center;
    float dist = length(radial);
    vec3 dir = radial / max(dist, 0.001);
    float wave = sin(3.0 * theta - 4.0 * uTime)
               + sin(5.0 * theta + 3.0 * uTime) * 0.7;
    float r = ur0 + uAmp * wave * 0.5;
    vec3 newPos = center + r * dir;
    vPos = (gl_ModelViewMatrix * vec4(newPos, 1.0)).xyz;
    vNormal = gl_NormalMatrix * dir;
    gl_Position = gl_ModelViewProjectionMatrix * vec4(newPos, 1.0);
    gl_FrontColor = gl_Color;
  }
")

(gl-shader mesh vert-src frag-src)
(gl-set-float mesh "uR"   5.)
(gl-set-float mesh "ur0"  2.)
(gl-set-float mesh "uAmp" 0.8)
```

### Auto-rotate axis matters

`gl-auto-rotate` with `wphi` rotates around the z-axis — the same plane as
`theta = atan(y,x)`.  A single-frequency wave traveling in theta will look
like rigid rotation.  To see the wave clearly, either:
- Rotate around a **different axis** (`wtheta` instead of `wphi`)
- Use **multiple superposed waves** so the interference pattern evolves

## Critical notes

### col-map-ptr output dtype
When mapping an `int32` iota through a float-returning `jit-fn`, you **must**
pass `"float"` as the third argument to `col-map-ptr`.  Otherwise the output
inherits the input's `int32` dtype and the float return values are truncated.

```scheme
;;; correct — explicit output type
(def {theta} (col-map-ptr theta-ptr idx "float"))

;;; wrong — output is int32, float values truncated to 0
(def {theta} (col-map-ptr theta-ptr idx))
```

### Modulo in jit-fn
The `%` operator works in `jit-fn` bodies: `(% i 18)` compiles to `(i % 18)`.
The alias `rem` also works.

### gl-add returns TGLPhysicalShape*
The returned object supports normal rooture method dispatch:
```scheme
(def {phys} (gl-add ctx mesh 0.5 0.5 0.5 1.0))
(.Rotate phys origin axis angle)
(.SetDiffuseColor phys rgba)
```

### Screenshots
`TASImage::FromWindow` cannot capture the OpenGL framebuffer on macOS — GL
viewports appear blank grey in `get_window` screenshots.  The geometry renders
correctly on screen.  Capturing would require `glReadPixels`.

### Viewer defaults
`gl-viewer` hides the left editor panel automatically for a clean viewport.

## Demo

`examples/gl_torus.rut` — animated torus with 2592 vertices, 5184 triangles,
per-vertex normals, GLSL vertex shader with four superposed traveling waves
(constructive/destructive interference), and auto-rotation.  One
`glDrawElements` call for the entire mesh.
