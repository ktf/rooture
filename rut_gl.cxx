/// @file rut_gl.cxx
/// Native 3D engine for rooture — bypasses TEve, renders triangle meshes
/// directly via glDrawElements on ROOT's TGLSAViewer.  Optional GLSL vertex
/// shaders for GPU-side animation (e.g. oscillating torus).

#include "rooture.h"
#include <array>

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "TVirtualPad.h"
#include "TGLSAViewer.h"
#include "TGLFormat.h"
#include "TGLAutoRotator.h"
#include "TGLScene.h"
#include "TGLLogicalShape.h"
#include "TGLPhysicalShape.h"
#include "TGLRnrCtx.h"
#include "TGLBoundingBox.h"
#include "TGLUtil.h"
#include "TNamed.h"
#include "TGFrame.h"
#include <chrono>

// ---------------------------------------------------------------------------
// Shader helpers
// ---------------------------------------------------------------------------

static GLuint compile_shader(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(s, sizeof(log), nullptr, log);
    rut_print("[gl-shader] compile error: %s\n", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

static GLuint link_program(GLuint vs, GLuint fs) {
  GLuint p = glCreateProgram();
  glAttachShader(p, vs);
  glAttachShader(p, fs);
  glLinkProgram(p);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetProgramInfoLog(p, sizeof(log), nullptr, log);
    rut_print("[gl-shader] link error: %s\n", log);
    glDeleteProgram(p);
    return 0;
  }
  return p;
}

// ---------------------------------------------------------------------------
// RutGLMesh — compiled TGLLogicalShape that renders via glDrawElements
// ---------------------------------------------------------------------------

class RutGLMesh : public TGLLogicalShape {
  RutColumnPtr fVertices;   // float32, interleaved xyz (3N floats)
  RutColumnPtr fIndices;    // int32, triangle indices (3T ints)
  RutColumnPtr fNormals;    // float32, per-vertex normals (3N floats), may be null
  size_t       fNVerts;
  size_t       fNTris;

  // Shader state — sources stored until first DirectDraw (GL context active)
  std::string fVertSrc, fFragSrc;
  mutable GLuint fProgram  = 0;
  mutable GLint  fTimeLoc  = -1;
  mutable bool   fShaderReady = false;
  std::chrono::steady_clock::time_point fTimeStart;
  mutable std::unordered_map<std::string, float> fPendingUniforms;
  mutable std::unordered_map<std::string, std::pair<GLint, float>> fUniforms;
  mutable std::unordered_map<std::string, std::array<float,16>> fPendingMat4;
  mutable std::unordered_map<std::string, std::pair<GLint, std::array<float,16>>> fMat4Uniforms;

public:
  RutGLMesh(TObject* id, RutColumnPtr v, RutColumnPtr i, RutColumnPtr n)
    : TGLLogicalShape(id),
      fVertices(std::move(v)), fIndices(std::move(i)), fNormals(std::move(n))
  {
    fNVerts = fVertices->n / 3;
    fNTris  = fIndices->n / 3;
    fDLCache = kFALSE;  // no display-list caching — we draw live each frame
    fTimeStart = std::chrono::steady_clock::now();
    UpdateBBox();
  }

  void SetShaderSource(const char* vs, const char* fs) {
    fVertSrc = vs;
    fFragSrc = fs;
    fShaderReady = false;  // compile on next DirectDraw
  }

  void SetUniformFloat(const char* name, float val) {
    fPendingUniforms[name] = val;
  }

  void SetUniformMat4(const char* name, const std::array<float,16>& m) {
    fPendingMat4[name] = m;
  }

  void DirectDraw(TGLRnrCtx& /*rnrCtx*/) const override {
    // Lazy shader compilation — GL context is guaranteed current here
    if (!fShaderReady && !fVertSrc.empty()) {
      GLuint vs = compile_shader(GL_VERTEX_SHADER, fVertSrc.c_str());
      GLuint fs = vs ? compile_shader(GL_FRAGMENT_SHADER, fFragSrc.c_str()) : 0;
      if (vs && fs) {
        fProgram = link_program(vs, fs);
        if (fProgram) fTimeLoc = glGetUniformLocation(fProgram, "uTime");
      }
      if (vs) glDeleteShader(vs);
      if (fs) glDeleteShader(fs);
      // Resolve pending uniform locations
      for (auto& [name, val] : fPendingUniforms) {
        GLint loc = fProgram ? glGetUniformLocation(fProgram, name.c_str()) : -1;
        fUniforms[name] = {loc, val};
      }
      fPendingUniforms.clear();
      for (auto& [name, val] : fPendingMat4) {
        GLint loc = fProgram ? glGetUniformLocation(fProgram, name.c_str()) : -1;
        fMat4Uniforms[name] = {loc, val};
      }
      fPendingMat4.clear();
      fShaderReady = true;
    }

    const float* vp = (const float*)fVertices->data;
    const int*   ip = (const int*)fIndices->data;

    if (fProgram) {
      glUseProgram(fProgram);
      // Auto-update uTime from wall clock
      auto now = std::chrono::steady_clock::now();
      float t = std::chrono::duration<float>(now - fTimeStart).count();
      if (fTimeLoc >= 0) {
        glUniform1f(fTimeLoc, t);
      }
      // Set user uniforms
      for (auto& [name, locval] : fUniforms) {
        if (locval.first >= 0)
          glUniform1f(locval.first, locval.second);
      }
      for (auto& [name, locval] : fMat4Uniforms) {
        if (locval.first >= 0)
          glUniformMatrix4fv(locval.first, 1, GL_TRUE, locval.second.data());
      }
    }

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, vp);

    if (fNormals && fNormals->n >= fNVerts * 3) {
      const float* np = (const float*)fNormals->data;
      glEnableClientState(GL_NORMAL_ARRAY);
      glNormalPointer(GL_FLOAT, 0, np);
    }

    glDrawElements(GL_TRIANGLES, (GLsizei)(fNTris * 3),
                   GL_UNSIGNED_INT, ip);

    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);

    if (fProgram) glUseProgram(0);
  }

private:
  void UpdateBBox() {
    const float* vp = (const float*)fVertices->data;
    float lo[3] = { vp[0], vp[1], vp[2] };
    float hi[3] = { vp[0], vp[1], vp[2] };
    for (size_t k = 1; k < fNVerts; k++) {
      for (int d = 0; d < 3; d++) {
        float c = vp[k * 3 + d];
        if (c < lo[d]) lo[d] = c;
        if (c > hi[d]) hi[d] = c;
      }
    }
    fBoundingBox.SetAligned(TGLVertex3(lo[0], lo[1], lo[2]),
                            TGLVertex3(hi[0], hi[1], hi[2]));
  }
};

// ---------------------------------------------------------------------------
// RutGLContext — scene + viewer + ID counter
// ---------------------------------------------------------------------------

struct RutGLContext {
  TGLScene*    scene   = nullptr;
  TGLSAViewer* viewer  = nullptr;
  UInt_t       nextID  = 1;
  std::vector<TNamed*> meshIDs;  // prevent GC of ID objects
};

static RutGLContext* gl_ctx_of(lval* v) {
  return (RutGLContext*)v->obj;
}

// ---------------------------------------------------------------------------
// Builtins
// ---------------------------------------------------------------------------

// (gl-viewer) or (gl-viewer "title")
static lval* builtin_gl_viewer(lenv* /*e*/, lval* a) {
  const char* title = "rooture GL";
  if (a->count == 1) {
    LASSERT_TYPE("gl-viewer", a, 0, LVAL_STR);
    title = a->cell[0]->str;
  } else {
    LASSERT(a, a->count == 0,
            "'gl-viewer' takes 0 or 1 arguments, got %d", a->count);
  }

  auto* ctx = new RutGLContext;
  std::string t(title);
  lval_del(a);

  rut_dispatch_work([&]{
    ctx->scene  = new TGLScene;
    ctx->viewer = new TGLSAViewer((TVirtualPad*)nullptr, (TGLFormat*)nullptr);
    ctx->viewer->AddScene(ctx->scene);
    ctx->viewer->Show();
    ctx->viewer->GetFrame()->SetWindowName(t.c_str());
    // Hide the left editor panel for a cleaner viewport
    ctx->viewer->GetLeftVerticalFrame()->UnmapWindow();
    ctx->viewer->GetFrame()->Layout();
  });

  lval* v = lval_tobj(ctx, nullptr);
  return v;
}

// (gl-mesh verts indices) or (gl-mesh verts indices normals)
static lval* builtin_gl_mesh(lenv* /*e*/, lval* a) {
  LASSERT(a, a->count == 2 || a->count == 3,
          "'gl-mesh' expects 2 or 3 arguments, got %d", a->count);
  LASSERT_TYPE("gl-mesh", a, 0, LVAL_COLUMN);
  LASSERT_TYPE("gl-mesh", a, 1, LVAL_COLUMN);

  auto& verts = *(RutColumnPtr*)a->cell[0]->obj;
  auto& idx   = *(RutColumnPtr*)a->cell[1]->obj;

  LASSERT(a, verts->dtype == COL_FLOAT32,
          "'gl-mesh' vertices must be float32");
  LASSERT(a, idx->dtype == COL_INT32,
          "'gl-mesh' indices must be int32");
  LASSERT(a, verts->n % 3 == 0,
          "'gl-mesh' vertices length must be a multiple of 3, got %zu", verts->n);
  LASSERT(a, idx->n % 3 == 0,
          "'gl-mesh' indices length must be a multiple of 3, got %zu", idx->n);

  RutColumnPtr normals;
  if (a->count == 3) {
    LASSERT_TYPE("gl-mesh", a, 2, LVAL_COLUMN);
    normals = *(RutColumnPtr*)a->cell[2]->obj;
    LASSERT(a, normals->dtype == COL_FLOAT32,
            "'gl-mesh' normals must be float32");
  }

  // Create a unique TNamed as the logical shape's external object / ID key
  static std::atomic<int> s_meshCounter{0};
  char name[64];
  snprintf(name, sizeof(name), "rut_mesh_%d", s_meshCounter.fetch_add(1));
  TNamed* idObj = new TNamed(name, "");

  RutGLMesh* mesh = nullptr;
  rut_dispatch_work([&]{
    mesh = new RutGLMesh(idObj, verts, idx, normals);
  });

  lval_del(a);
  lval* v = lval_tobj(mesh, nullptr);
  return v;
}

// (gl-shader mesh vert-src frag-src) — compile and attach GLSL shaders
static lval* builtin_gl_shader(lenv* /*e*/, lval* a) {
  LASSERT_NUM("gl-shader", a, 3);
  LASSERT_TYPE("gl-shader", a, 0, LVAL_TOBJ);
  LASSERT_TYPE("gl-shader", a, 1, LVAL_STR);
  LASSERT_TYPE("gl-shader", a, 2, LVAL_STR);
  auto* mesh = (RutGLMesh*)a->cell[0]->obj;
  const char* vs_src = a->cell[1]->str;
  const char* fs_src = a->cell[2]->str;
  GLuint prog = 0;
  std::string vs_s(vs_src), fs_s(fs_src);
  lval_del(a);
  mesh->SetShaderSource(vs_s.c_str(), fs_s.c_str());
  return lval_sexpr();
}

// (gl-set-float mesh name value) — set a float uniform on mesh's shader
static lval* builtin_gl_set_float(lenv* /*e*/, lval* a) {
  LASSERT_NUM("gl-set-float", a, 3);
  LASSERT_TYPE("gl-set-float", a, 0, LVAL_TOBJ);
  LASSERT_TYPE("gl-set-float", a, 1, LVAL_STR);
  auto* mesh = (RutGLMesh*)a->cell[0]->obj;
  const char* name = a->cell[1]->str;
  float val;
  if (a->cell[2]->type == LVAL_FLOAT)      val = (float)a->cell[2]->floating;
  else if (a->cell[2]->type == LVAL_NUM)    val = (float)a->cell[2]->num;
  else { lval_del(a); return lval_err("'gl-set-float' value must be a number"); }
  std::string n(name);
  lval_del(a);
  mesh->SetUniformFloat(n.c_str(), val);
  return lval_sexpr();
}

// (gl-set-mat4 mesh name {v0 v1 ... v15}) — set a mat4 uniform from 16 floats
static lval* builtin_gl_set_mat4(lenv* /*e*/, lval* a) {
  LASSERT_NUM("gl-set-mat4", a, 3);
  LASSERT_TYPE("gl-set-mat4", a, 0, LVAL_TOBJ);
  LASSERT_TYPE("gl-set-mat4", a, 1, LVAL_STR);
  LASSERT_TYPE("gl-set-mat4", a, 2, LVAL_QEXPR);
  LASSERT(a, a->cell[2]->count == 16,
    "'gl-set-mat4' needs 16 values, got %d", a->cell[2]->count);
  auto* mesh = (RutGLMesh*)a->cell[0]->obj;
  const char* name = a->cell[1]->str;
  std::array<float,16> m;
  for (int i = 0; i < 16; i++) {
    lval* v = a->cell[2]->cell[i];
    if (v->type == LVAL_FLOAT)      m[i] = (float)v->floating;
    else if (v->type == LVAL_NUM)   m[i] = (float)v->num;
    else { lval_del(a); return lval_err("'gl-set-mat4' values must be numbers"); }
  }
  std::string n(name);
  lval_del(a);
  mesh->SetUniformMat4(n.c_str(), m);
  return lval_sexpr();
}

// (gl-add ctx mesh [r g b a])
static lval* builtin_gl_add(lenv* /*e*/, lval* a) {
  LASSERT(a, a->count == 2 || a->count == 6,
          "'gl-add' expects 2 or 6 arguments (ctx mesh [r g b a]), got %d", a->count);
  LASSERT_TYPE("gl-add", a, 0, LVAL_TOBJ);
  LASSERT_TYPE("gl-add", a, 1, LVAL_TOBJ);

  auto* ctx  = gl_ctx_of(a->cell[0]);
  auto* mesh = (RutGLMesh*)a->cell[1]->obj;

  Float_t rgba[4] = {0.6f, 0.6f, 0.6f, 1.0f};
  if (a->count == 6) {
    for (int i = 0; i < 4; i++) {
      lval* c = a->cell[2 + i];
      if (c->type == LVAL_FLOAT)
        rgba[i] = (Float_t)c->floating;
      else if (c->type == LVAL_NUM)
        rgba[i] = (Float_t)c->num;
      else {
        lval_del(a);
        return lval_err("'gl-add' color component %d must be a number", i);
      }
    }
  }

  TGLPhysicalShape* phys = nullptr;
  UInt_t id = ctx->nextID++;
  rut_dispatch_work([&]{
    ctx->scene->BeginUpdate();
    ctx->scene->AdoptLogical(*mesh);
    TGLMatrix identity;
    phys = new TGLPhysicalShape(id, *mesh, identity, kFALSE, rgba);
    ctx->scene->AdoptPhysical(*phys);
    ctx->scene->EndUpdate();
  });

  // Keep ID object alive
  ctx->meshIDs.push_back((TNamed*)mesh->ID());

  lval_del(a);
  return lval_tobj(phys, TClass::GetClass("TGLPhysicalShape"));
}

// (gl-redraw ctx)
static lval* builtin_gl_redraw(lenv* /*e*/, lval* a) {
  LASSERT_NUM("gl-redraw", a, 1);
  LASSERT_TYPE("gl-redraw", a, 0, LVAL_TOBJ);
  auto* ctx = gl_ctx_of(a->cell[0]);
  lval_del(a);
  rut_dispatch_work([ctx]{
    ctx->viewer->RequestDraw();
  });
  return lval_sexpr();
}

// (gl-reset-camera ctx)
static lval* builtin_gl_reset_camera(lenv* /*e*/, lval* a) {
  LASSERT_NUM("gl-reset-camera", a, 1);
  LASSERT_TYPE("gl-reset-camera", a, 0, LVAL_TOBJ);
  auto* ctx = gl_ctx_of(a->cell[0]);
  lval_del(a);
  rut_dispatch_work([ctx]{
    ctx->viewer->ResetCurrentCamera();
  });
  return lval_sexpr();
}

// (gl-clear ctx)
static lval* builtin_gl_clear(lenv* /*e*/, lval* a) {
  LASSERT_NUM("gl-clear", a, 1);
  LASSERT_TYPE("gl-clear", a, 0, LVAL_TOBJ);
  auto* ctx = gl_ctx_of(a->cell[0]);
  lval_del(a);
  rut_dispatch_work([ctx]{
    ctx->scene->BeginUpdate();
    ctx->scene->DestroyPhysicals();
    ctx->scene->DestroyLogicals();
    ctx->scene->EndUpdate();
    ctx->viewer->RequestDraw();
  });
  for (auto* n : ctx->meshIDs) delete n;
  ctx->meshIDs.clear();
  ctx->nextID = 1;
  return lval_sexpr();
}

// (gl-auto-rotate ctx [dt wphi wtheta atheta])
static lval* builtin_gl_auto_rotate(lenv* /*e*/, lval* a) {
  LASSERT(a, a->count >= 1 && a->count <= 5,
          "'gl-auto-rotate' expects 1-5 args, got %d", a->count);
  LASSERT_TYPE("gl-auto-rotate", a, 0, LVAL_TOBJ);
  auto* ctx = gl_ctx_of(a->cell[0]);
  double dt     = 0.01;
  double wphi   = 0.5;
  double wtheta = 0.0;
  double atheta = 0.0;
  auto getf = [&](int i) -> double {
    if (a->cell[i]->type == LVAL_FLOAT) return a->cell[i]->floating;
    return (double)a->cell[i]->num;
  };
  if (a->count > 1) dt     = getf(1);
  if (a->count > 2) wphi   = getf(2);
  if (a->count > 3) wtheta = getf(3);
  if (a->count > 4) atheta = getf(4);
  lval_del(a);
  rut_dispatch_work([ctx, dt, wphi, wtheta, atheta]{
    auto* ar = ctx->viewer->GetAutoRotator();
    ar->SetDt(dt);
    ar->SetWPhi(wphi);
    ar->SetWTheta(wtheta);
    ar->SetATheta(atheta);
    ar->Start();
  });
  return lval_sexpr();
}

// (gl-frame ctx) → TGCompositeFrame* (for get_window screenshots)
static lval* builtin_gl_frame(lenv* /*e*/, lval* a) {
  LASSERT_NUM("gl-frame", a, 1);
  LASSERT_TYPE("gl-frame", a, 0, LVAL_TOBJ);
  auto* ctx = gl_ctx_of(a->cell[0]);
  lval_del(a);
  TGCompositeFrame* frame = nullptr;
  rut_dispatch_work([ctx, &frame]{
    frame = ctx->viewer->GetFrame();
  });
  return lval_tobj(frame, TClass::GetClass("TGCompositeFrame"));
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void lenv_add_builtins_gl(lenv* e) {
  lenv_add_builtin(e, "gl-viewer",       builtin_gl_viewer);
  lenv_add_builtin(e, "gl-mesh",         builtin_gl_mesh);
  lenv_add_builtin(e, "gl-shader",       builtin_gl_shader);
  lenv_add_builtin(e, "gl-set-float",    builtin_gl_set_float);
  lenv_add_builtin(e, "gl-set-mat4",     builtin_gl_set_mat4);
  lenv_add_builtin(e, "gl-add",          builtin_gl_add);
  lenv_add_builtin(e, "gl-redraw",       builtin_gl_redraw);
  lenv_add_builtin(e, "gl-reset-camera", builtin_gl_reset_camera);
  lenv_add_builtin(e, "gl-clear",        builtin_gl_clear);
  lenv_add_builtin(e, "gl-auto-rotate",  builtin_gl_auto_rotate);
  lenv_add_builtin(e, "gl-frame",        builtin_gl_frame);
}
