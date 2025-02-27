// Copyright 2021 DeepMind Technologies Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>
#include <array>
#include <cmath>
#include <csetjmp>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#define TINYOBJLOADER_IMPLEMENTATION

#include <mujoco/mjmodel.h>
#include "cc/array_safety.h"
#include "engine/engine_crossplatform.h"
#include "engine/engine_file.h"
#include "engine/engine_macro.h"
#include "engine/engine_util_blas.h"
#include "engine/engine_util_errmem.h"
#include "engine/engine_util_solve.h"
#include "engine/engine_util_spatial.h"
#include "engine/engine_vfs.h"
#include "user/user_model.h"
#include "user/user_objects.h"
#include "user/user_util.h"
#include <tiny_obj_loader.h>

extern "C" {
#include "qhull_ra.h"
}

using std::string;
using std::vector;

// compute triangle area, surface normal, center
static mjtNum _triangle(mjtNum* normal, mjtNum* center,
                        const float* v1, const float* v2, const float* v3) {
  // center
  if (center) {
    for (int i=0; i<3; i++) {
      center[i] = (v1[i] + v2[i] + v3[i])/3;
    }
  }

  // normal = (v2-v1) cross (v3-v1)
  double b[3] = { v2[0]-v1[0], v2[1]-v1[1], v2[2]-v1[2] };
  double c[3] = { v3[0]-v1[0], v3[1]-v1[1], v3[2]-v1[2] };
  mju_cross(normal, b, c);

  // get length
  double len = mju_norm3(normal);

  // ignore small faces
  if (len<mjMINVAL) {
    return 0;
  }

  // normalize
  normal[0] /= len;
  normal[1] /= len;
  normal[2] /= len;

  // return area
  return len/2;
}

//------------------ class mjCMesh implementation --------------------------------------------------

// constructor
mjCMesh::mjCMesh(mjCModel* _model, mjCDef* _def) {
  // set defaults
  mjuu_setvec(refpos, 0, 0, 0);
  mjuu_setvec(refquat, 1, 0, 0, 0);
  mjuu_setvec(scale, 1, 1, 1);
  smoothnormal = false;
  file.clear();
  uservert.clear();
  usernormal.clear();
  usertexcoord.clear();
  userface.clear();
  userfacenormal.clear();
  userfacetexcoord.clear();
  useredge.clear();

  // clear internal variables
  mjuu_setvec(pos_surface, 0, 0, 0);
  mjuu_setvec(pos_volume, 0, 0, 0);
  mjuu_setvec(quat_surface, 1, 0, 0, 0);
  mjuu_setvec(quat_volume, 1, 0, 0, 0);
  mjuu_setvec(boxsz_surface, 0, 0, 0);
  mjuu_setvec(boxsz_volume, 0, 0, 0);
  mjuu_setvec(aabb, 1e10, 1e10, 1e10);
  mjuu_setvec(aabb+3, -1e10, -1e10, -1e10);
  nvert = 0;
  nnormal = 0;
  ntexcoord = 0;
  nface = 0;
  szgraph = 0;
  vert = NULL;
  normal = NULL;
  texcoord = NULL;
  face = NULL;
  facenormal = NULL;
  facetexcoord = NULL;
  graph = NULL;
  needhull = false;
  invalidorientation.first = -1;
  invalidorientation.second = -1;
  validarea = true;
  validvolume = true;
  valideigenvalue = true;
  validinequality = true;
  processed = false;

  // reset to default if given
  if (_def) {
    *this = _def->mesh;
  }

  // set model, def
  model = _model;
  def = (_def ? _def : (_model ? _model->defaults[0] : 0));
}



// destructor
mjCMesh::~mjCMesh() {
  file.clear();
  uservert.clear();
  usernormal.clear();
  usertexcoord.clear();
  userface.clear();
  userfacenormal.clear();
  userfacetexcoord.clear();
  useredge.clear();

  if (vert) mju_free(vert);
  if (normal) mju_free(normal);
  if (texcoord) mju_free(texcoord);
  if (face) mju_free(face);
  if (facenormal) mju_free(facenormal);
  if (facetexcoord) mju_free(facetexcoord);
  if (graph) mju_free(graph);
}



template <typename T> static T* VecToArray(std::vector<T>& vector,  bool clear = true){
  if (vector.empty())
    return nullptr;
  else {
    int n = (int)vector.size();
    T* cvec = (T*) mju_malloc(n*sizeof(T));
    memcpy(cvec, vector.data(), n*sizeof(T));
    if (clear) {
      vector.clear();
    }
    return cvec;
  }
}



// compiler
void mjCMesh::Compile(const mjVFS* vfs) {
  // load file
  if (!file.empty()) {
    // remove path from file if necessary
    if (model->strippath) {
      file = mjuu_strippath(file);
    }

    // load STL, OBJ or MSH
    string ext = mjuu_getext(file);
    if (!strcasecmp(ext.c_str(), ".stl")) {
      LoadSTL(vfs);
    } else if (!strcasecmp(ext.c_str(), ".obj")) {
      LoadOBJ(vfs);
    } else if (!strcasecmp(ext.c_str(), ".msh")) {
      LoadMSH(vfs);
    } else {
      throw mjCError(this, "Unknown mesh file type: %s", file.c_str());
    }
  }

  // copy user vertex
  if (!uservert.empty()) {
    // check repeated
    if (vert) {
      throw mjCError(this, "repeated vertex specification");
    }

    // check size
    if (uservert.size()<12) {
      throw mjCError(this, "at least 4 verices required");
    }
    if (uservert.size()%3) {
      throw mjCError(this, "vertex data must be a multiple of 3");
    }

    // copy from user
    nvert = (int)uservert.size()/3;
    vert = VecToArray(uservert, !file.empty());
  }

  // copy user normal
  if (!usernormal.empty()) {
    // check repeated
    if (normal) {
      throw mjCError(this, "repeated normal specification");
    }

    // check size
    if (usernormal.size()%3) {
      throw mjCError(this, "normal data must be a multiple of 3");
    }

    // copy from user
    nnormal = (int)usernormal.size()/3;
    normal = VecToArray(usernormal, !file.empty());
  }

  // copy user texcoord
  if (!usertexcoord.empty()) {
    // check repeated
    if (texcoord) {
      throw mjCError(this, "repeated texcoord specification");
    }

    // check size
    if (usertexcoord.size()%2) {
      throw mjCError(this, "texcoord must be a multiple of 2");
    }

    // copy from user
    ntexcoord = (int)usertexcoord.size()/2;
    texcoord = VecToArray(usertexcoord, !file.empty());
  }

  // copy user face
  if (!userface.empty()) {
    // check repeated
    if (face) {
      throw mjCError(this, "repeated face specification");
    }

    // check size
    if (userface.size()%3) {
      throw mjCError(this, "face data must be a multiple of 3");
    }

    // copy from user
    nface = (int)userface.size()/3;
    face = VecToArray(userface, !file.empty());

    // check vertices exist
    for (auto vertex_index : userface) {
      if (vertex_index>=nvert || vertex_index < 0) {
        throw mjCError(this, "found index in userface that exceeds uservert size.");
      }
    }

    // create half-edge structure (if mesh was in XML)
    if (useredge.empty()) {
      for (int i=0; i<nface; i++) {
        int v0 = userface[3*i+0];
        int v1 = userface[3*i+1];
        int v2 = userface[3*i+2];
        mjtNum normal[3];
        if (_triangle(normal, nullptr, vert+3*v0, vert+3*v1, vert+3*v2)>sqrt(mjMINVAL)) {
          useredge.push_back(std::pair(v0, v1));
          useredge.push_back(std::pair(v1, v2));
          useredge.push_back(std::pair(v2, v0));
        } else {
          // TODO(b/255525326)
        }
      }
    }
  }

  // check for inconsistent face orientations
  if (!useredge.empty()) {
    std::sort(useredge.begin(), useredge.end());
    auto iterator = std::adjacent_find(useredge.begin(), useredge.end());
    if (iterator != useredge.end()) {
      invalidorientation.first = iterator->first+1;
      invalidorientation.second = iterator->second+1;
    }
  }

  // require vertices
  if (!vert) {
    throw mjCError(this, "no vertices");
  }

  // make graph describing convex hull
  if ((model->convexhull && needhull) || !face) {
    MakeGraph();
  }

  // no faces: copy from convex hull
  if (!face) {
    CopyGraph();
  }

  // no normals: make
  if (!normal) {
    MakeNormal();
  }

  // copy user normal indices
  if (!userfacenormal.empty()) {
    // check repeated
    if (facenormal) {
      throw mjCError(this, "repeated facenormal specification");
    }

    if (userfacenormal.size()!=3*nface) {
      throw mjCError(this, "face data must have the same size as face normal data");
    }

    facenormal = VecToArray(userfacenormal, !file.empty());
  }

  // copy user texcoord
  if (!userfacetexcoord.empty()) {
    // check repeated
    if (facetexcoord) {
      throw mjCError(this, "repeated facetexcoord specification");
    }

    facetexcoord = VecToArray(userfacetexcoord, !file.empty());
  }

  // facenormal might not exist if usernormal was specified
  if (!facenormal) {
    facenormal = (int*) mju_malloc(3*nface*sizeof(int));
    memcpy(facenormal, face, 3*nface*sizeof(int));
  }

  // scale, center, orient, compute mass and inertia
  Process();
  processed = true;
}



// get position
double* mjCMesh::GetPosPtr(mjtMeshType type) {
  if (type==mjSHELL_MESH) {
    return pos_surface;
  } else {
    return pos_volume;
  }
}



// get orientation
double* mjCMesh::GetQuatPtr(mjtMeshType type) {
  if (type==mjSHELL_MESH) {
    return quat_surface;
  } else {
    return quat_volume;
  }
}



// set geom size to match mesh
void mjCMesh::FitGeom(mjCGeom* geom, double* meshpos) {
  // copy mesh pos into meshpos
  mjuu_copyvec(meshpos, GetPosPtr(geom->typeinertia), 3);

  // use inertial box
  if (!model->fitaabb) {
    // get inertia box type (shell or volume)
    double* boxsz = GetInertiaBoxPtr(geom->typeinertia);
    switch (geom->type) {
    case mjGEOM_SPHERE:
      geom->size[0] = (boxsz[0] + boxsz[1] + boxsz[2])/3;
      break;

    case mjGEOM_CAPSULE:
      geom->size[0] = (boxsz[0] + boxsz[1])/2;
      geom->size[1] = mjMAX(0, boxsz[2] - geom->size[0]/2);
      break;

    case mjGEOM_CYLINDER:
      geom->size[0] = (boxsz[0] + boxsz[1])/2;
      geom->size[1] = boxsz[2];
      break;

    case mjGEOM_ELLIPSOID:
    case mjGEOM_BOX:
      geom->size[0] = boxsz[0];
      geom->size[1] = boxsz[1];
      geom->size[2] = boxsz[2];
      break;

    default:
      throw mjCError(this, "invalid geom type in fitting mesh %s", name.c_str());
    }
  }

  // use aabb
  else {
    // find aabb box center
    double cen[3] = {(aabb[0]+aabb[3])/2, (aabb[1]+aabb[4])/2, (aabb[2]+aabb[5])/2};

    // add box center into meshpos
    meshpos[0] += cen[0];
    meshpos[1] += cen[1];
    meshpos[2] += cen[2];

    // compute depending on type
    switch (geom->type) {
    case mjGEOM_SPHERE:
      // find maximum distance
      geom->size[0] = 0;
      for (int i=0; i<nvert; i++) {
        double v[3] = {vert[3*i], vert[3*i+1], vert[3*i+2]};
        double dst = mjuu_dist3(v, cen);
        geom->size[0] = mjMAX(geom->size[0], dst);
      }
      break;

    case mjGEOM_CAPSULE:
    case mjGEOM_CYLINDER:
      // find maximum distance in XY, separately in Z
      geom->size[0] = 0;
      geom->size[1] = 0;
      for (int i=0; i<nvert; i++) {
        double v[3] = {vert[3*i], vert[3*i+1], vert[3*i+2]};
        double dst = sqrt((v[0]-cen[0])*(v[0]-cen[0]) +
                          (v[1]-cen[1])*(v[1]-cen[1]));
        geom->size[0] = mjMAX(geom->size[0], dst);

        // proceed with z: valid for cylinder
        double dst2 = fabs(v[2]-cen[2]);
        geom->size[1] = mjMAX(geom->size[1], dst2);
      }

      // special handling of capsule: consider curved cap
      if (geom->type==mjGEOM_CAPSULE) {
        geom->size[1] = 0;
        for (int i=0; i<nvert; i++) {
          // get distance in XY and Z
          double v[3] = {vert[3*i], vert[3*i+1], vert[3*i+2]};
          double dst = sqrt((v[0]-cen[0])*(v[0]-cen[0]) +
                            (v[1]-cen[1])*(v[1]-cen[1]));
          double dst2 = fabs(v[2]-cen[2]);

          // get spherical elevation at horizontal distance dst
          double h = geom->size[0] * sin(acos(dst/geom->size[0]));
          geom->size[1] = mjMAX(geom->size[1], dst2-h);
        }
      }
      break;

    case mjGEOM_ELLIPSOID:
    case mjGEOM_BOX:
      geom->size[0] = aabb[3] - cen[0];
      geom->size[1] = aabb[4] - cen[1];
      geom->size[2] = aabb[5] - cen[2];
      break;

    default:
      throw mjCError(this, "invalid fittype in mesh %s", name.c_str());
    }
  }

  // rescale size
  geom->size[0] *= geom->fitscale;
  geom->size[1] *= geom->fitscale;
  geom->size[2] *= geom->fitscale;
}



// comparison function for vertex sorting
quicksortfunc(vertcompare, context, el1, el2) {
  float* vert = (float*) context;
  float x1 = vert[3*(*(int*)el1)] + 1e-2*vert[1+3*(*(int*)el1)] + 1e-4*vert[2+3*(*(int*)el1)];
  float x2 = vert[3*(*(int*)el2)] + 1e-2*vert[1+3*(*(int*)el2)] + 1e-4*vert[2+3*(*(int*)el2)];

  if (x1 < x2) {
    return -1;
  } else if (x1 == x2) {
    return 0;
  } else {
    return 1;
  }
}

// remove repeated vertices
void mjCMesh::RemoveRepeated() {
  int repeated = 0;

  // allocate sort and redirection indices, set to identity
  auto index = std::unique_ptr<int[]>(new int[nvert]);
  auto redirect = std::unique_ptr<int[]>(new int[nvert]);
  for (int i=0; i < nvert; i++) {
    index[i] = redirect[i] = i;
  }

  // sort vertices
  mjQUICKSORT(index.get(), nvert, sizeof(int), vertcompare, vert);

  // find repeated vertices, set redirect
  for (int i=1; i < nvert; i++) {
    if (vert[3*index[i]] == vert[3*index[i-1]] &&
        vert[3*index[i]+1] == vert[3*index[i-1]+1] &&
        vert[3*index[i]+2] == vert[3*index[i-1]+2]) {
      redirect[index[i]] = index[i-1];
      repeated++;
    }
  }

  // compress vertices, change face data
  if (repeated) {
    // track redirections until non-redirected vertex, set
    for (int i=0; i<nvert; i++) {
      int j = i;
      while (redirect[j]!=j) {
        j = redirect[j];
      }
      redirect[i] = j;
    }

    // find good vertices, compress, reuse index to save compressed position
    int j = 0;
    for (int i=0; i<nvert; i++) {
      if (redirect[i]==i) {
        index[i] = j;
        memcpy(vert+3*j, vert+3*i, 3*sizeof(float));
        j++;
      } else {
        index[i] = -1;
      }
    }

    // recompute face data to reflect compressed vertices
    for (int i=0; i<3*nface; i++) {
      face[i] = index[redirect[face[i]]];

      // sanity check, SHOULD NOT OCCUR
      if (face[i]<0 || face[i]>=nvert-repeated) {
        throw mjCError(
            this, "error removing vertices from mesh '%s'", name.c_str());
      }
    }
  }

  // correct vertex count
  nvert -= repeated;

  // resize vert if any vertices were removed
  if (repeated) {
    float* old = vert;
    vert = (float*) mju_malloc(3*nvert*sizeof(float));
    memcpy(vert, old, 3*nvert*sizeof(float));
    mju_free(old);
  }
}


// load OBJ mesh
void mjCMesh::LoadOBJ(const mjVFS* vfs) {

  // make filename
  string filename = mjuu_makefullname(
      model->modelfiledir, model->meshdir, file);

  tinyobj::ObjReader objReader;
  char* buffer = nullptr;
  if (vfs) {
    int id = mj_findFileVFS(vfs, filename.c_str());
    if (id >= 0) {
      buffer = static_cast<char*>(vfs->filedata[id]);
      int buffer_sz = vfs->filesize[id];
      // TODO(etom): support .mtl files in the VFS case?
      objReader.ParseFromString(std::string(buffer, buffer_sz), std::string());
    }
  }

  // if not found in vfs, read from file
  if (!buffer) {
    objReader.ParseFromFile(filename);
  }

  if (!objReader.Valid()) {
    std::stringstream msg;
    msg << "could not parse OBJ file '" << filename << "': \n"
        << objReader.Error();
    throw mjCError(this, "%s", msg.str().c_str());
  }

  const auto& attrib = objReader.GetAttrib();
  uservert = attrib.vertices;  // copy from one std::vector to another
  usernormal = attrib.normals;
  usertexcoord = attrib.texcoords;

  if (!objReader.GetShapes().empty()) {
    const auto& mesh = objReader.GetShapes()[0].mesh;
    bool righthand = (scale[0]*scale[1]*scale[2] > 0);

    // iterate over mesh faces
    std::vector<tinyobj::index_t> face_indices;
    for (int face = 0, idx = 0; idx < mesh.indices.size();) {
      int nfacevert = mesh.num_face_vertices[face];
      if (nfacevert < 3 || nfacevert > 4) {
        throw mjCError(
            this, "only tri or quad meshes are supported for OBJ (file '%s')",
            filename.c_str());
      }

      face_indices.push_back(mesh.indices[idx]);
      face_indices.push_back(mesh.indices[idx + (righthand==1 ? 1 : 2)]);
      face_indices.push_back(mesh.indices[idx + (righthand==1 ? 2 : 1)]);

      if (nfacevert == 4) {
        face_indices.push_back(mesh.indices[idx]);
        face_indices.push_back(mesh.indices[idx + (righthand==1 ? 2 : 3)]);
        face_indices.push_back(mesh.indices[idx + (righthand==1 ? 3 : 2)]);
      }
      idx += nfacevert;
      ++face;
    }

    // for each vertex, store index, normal, and texcoord
    for (const auto& mesh_index : face_indices) {
      userface.push_back(mesh_index.vertex_index);

      if (!usernormal.empty()) {
        userfacenormal.push_back(mesh_index.normal_index);
      }

      if (!usertexcoord.empty()) {
        userfacetexcoord.push_back(mesh_index.texcoord_index);
      }
    }

    for (int i = 0; i < face_indices.size(); i += 3) {
      // add edges
      const float *v0 = uservert.data() + 3*face_indices[i+0].vertex_index;
      const float *v1 = uservert.data() + 3*face_indices[i+1].vertex_index;
      const float *v2 = uservert.data() + 3*face_indices[i+2].vertex_index;

      // only consider edges if the face contribution is significant
      mjtNum normal[3];
      if (_triangle(normal, nullptr, v0, v1, v2)>sqrt(mjMINVAL)) {
        useredge.push_back(std::pair(face_indices[i+0].vertex_index, face_indices[i+1].vertex_index));
        useredge.push_back(std::pair(face_indices[i+1].vertex_index, face_indices[i+2].vertex_index));
        useredge.push_back(std::pair(face_indices[i+2].vertex_index, face_indices[i+0].vertex_index));
      } else {
        // TODO(b/255525326)
      }
    }
  }

  // flip the second texcoord
  for (int i=1; i<usertexcoord.size()/2; i++) {
    usertexcoord[2*i+1] = 1-usertexcoord[2*i+1];
  }
}


// load STL binary mesh
void mjCMesh::LoadSTL(const mjVFS* vfs) {
  bool righthand = (scale[0]*scale[1]*scale[2]>0);

  // make filename
  string filename = mjuu_makefullname(model->modelfiledir, model->meshdir, file);

  // get file data in buffer
  char* buffer = 0;
  int buffer_sz = 0;
  bool own_buffer = false;
  if (vfs) {
    int id = mj_findFileVFS(vfs, filename.c_str());
    if (id>=0) {
      buffer = (char*)vfs->filedata[id];
      buffer_sz = vfs->filesize[id];
    }
  }

  // if not found in vfs, read from file
  if (!buffer) {
    buffer = (char*) mju_fileToMemory(filename.c_str(), &buffer_sz);
    own_buffer = true;
  }

  // still not found
  if (!buffer) {
    throw mjCError(this, "could not open STL file '%s'", filename.c_str());
  } else if (!buffer_sz) {
    if (own_buffer) {
      mju_free(buffer);
    }
    throw mjCError(this, "STL file '%s' is empty", filename.c_str());
  }

  // make sure there is enough data for header
  if (buffer_sz<84) {
    if (own_buffer) {
      mju_free(buffer);
    }

    throw mjCError(this, "invalid header in STL file '%s'", filename.c_str());
  }

  // get number of triangles, check bounds
  nface = *(unsigned int*)(buffer+80);
  if (nface<1 || nface>200000) {
    if (own_buffer) {
      mju_free(buffer);
    }

    throw mjCError(this,
                   "number of faces should be between 1 and 200000 in STL file '%s';"
                   " perhaps this is an ASCII file?", filename.c_str());
  }

  // check remaining buffer size
  if (nface*50 != buffer_sz-84) {
    if (own_buffer) {
      mju_free(buffer);
    }

    throw mjCError(this,
                   "STL file '%s' has wrong size; perhaps this is an ASCII file?",
                   filename.c_str());
  }

  // assign stl data pointer
  const char* stl = buffer + 84;

  // allocate face and vertex data
  face = (int*) mju_malloc(3*nface*sizeof(int));
  vert = (float*) mju_malloc(9*nface*sizeof(float));

  // add vertices and faces, including repeated for now
  for (int i=0; i<nface; i++) {
    for (int j=0; j<3; j++) {
      // get pointer to vertex coordiates
      float* v = (float*)(stl+50*i+12*(j+1));
      for (int k=0; k < 3; k++) {
        if (std::isnan(v[k]) || std::isinf(v[k])) {
          if (own_buffer) {
            mju_free(buffer);
          }

          throw mjCError(this, "STL file '%s' contains invalid vertices.",
                         filename.c_str());
        }
        // check if vertex coordinates can be cast to an int safely
        if (fabs(v[k])>pow(2, 30)) {
          if (own_buffer) {
            mju_free(buffer);
          }

          throw mjCError(this,
                        "vertex coordinates in STL file '%s' exceed maximum bounds",
                        filename.c_str());
        }
      }

      // add vertex address in face; change order if scale makes it lefthanded
      if (righthand || j==0) {
        face[3*i+j] = nvert;
      } else {
        face[3*i+3-j] = nvert;
      }

      // add vertex data
      memcpy(vert+3*nvert, v, 3*sizeof(float));
      nvert++;
    }
  }

  // free buffer if allocated here
  if (own_buffer) {
    mju_free(buffer);
  }

  RemoveRepeated();
}



// load MSH binary mesh
void mjCMesh::LoadMSH(const mjVFS* vfs) {
  bool righthand = (scale[0]*scale[1]*scale[2]>0);

  // make filename
  string filename = mjuu_makefullname(model->modelfiledir, model->meshdir, file);

  // get file data in buffer
  char* buffer = 0;
  int buffer_sz = 0;
  bool own_buffer = false;
  if (vfs) {
    int id = mj_findFileVFS(vfs, filename.c_str());
    if (id>=0) {
      buffer = (char*)vfs->filedata[id];
      buffer_sz = vfs->filesize[id];
    }
  }

  // if not found in vfs, read from file
  if (!buffer) {
    buffer = (char*) mju_fileToMemory(filename.c_str(), &buffer_sz);
    own_buffer = true;
  }

  // still not found
  if (!buffer) {
    throw mjCError(this, "could not open MSH file '%s'", filename.c_str());
  } else if (!buffer_sz) {
    if (own_buffer) {
      mju_free(buffer);
    }
    throw mjCError(this, "MSH file '%s' is empty", filename.c_str());
  }

  // make sure header is present
  if (buffer_sz<4*sizeof(int)) {
    if (own_buffer) {
      mju_free(buffer);
    }
    throw mjCError(this, "missing header in MSH file '%s'", filename.c_str());
  }

  // get sizes from header
  nvert = ((int*)buffer)[0];
  nnormal = ((int*)buffer)[1];
  ntexcoord = ((int*)buffer)[2];
  nface = ((int*)buffer)[3];

  // check sizes
  if (nvert<4 || nface<0 || nnormal<0 || ntexcoord<0 ||
      (nnormal>0 && nnormal!=nvert) ||
      (ntexcoord>0 && ntexcoord!=nvert)) {
    if (own_buffer) {
      mju_free(buffer);
    }
    throw mjCError(this, "invalid sizes in MSH file '%s'", filename.c_str());
  }

  // check file size
  if (buffer_sz != 4*sizeof(int) + 3*nvert*sizeof(float) + 3*nnormal*sizeof(float) +
      2*ntexcoord*sizeof(float) + 3*nface*sizeof(int)) {
    if (own_buffer) {
      mju_free(buffer);
    }
    throw mjCError(this, "unexpected file size in MSH file '%s'", filename.c_str());
  }

  // allocate and copy
  float* fdata = (float*)(((int*)buffer) + 4);
  if (nvert) {
    vert = (float*) mju_malloc(3*nvert*sizeof(float));
    memcpy(vert, fdata, 3*nvert*sizeof(float));
    fdata += 3*nvert;
  }
  if (nnormal) {
    normal = (float*) mju_malloc(3*nvert*sizeof(float));
    memcpy(normal, fdata, 3*nvert*sizeof(float));
    fdata += 3*nvert;
  }
  if (ntexcoord) {
    texcoord = (float*) mju_malloc(2*nvert*sizeof(float));
    memcpy(texcoord, fdata, 2*nvert*sizeof(float));
    fdata += 2*nvert;
  }
  if (nface) {
    face = (int*) mju_malloc(3*nface*sizeof(int));
    facenormal = (int*) mju_malloc(3*nface*sizeof(int));
    memcpy(face, fdata, 3*nface*sizeof(int));
    memcpy(facenormal, fdata, 3*nface*sizeof(int));
  }
  if  (nface && texcoord) {
    facetexcoord = (int*) mju_malloc(3*nface*sizeof(int));
    memcpy(facetexcoord, fdata, 3*nface*sizeof(int));
  }

  // rearange face data if left-handed scaling
  if (nface && !righthand) {
    for (int i=0; i<nface; i++) {
      int tmp = face[3*i+1];
      face[3*i+1] = face[3*i+2];
      face[3*i+2] = tmp;
    }
  }

  // free buffer if allocated here
  if (own_buffer) {
    mju_free(buffer);
  }
}



// apply transformations
void mjCMesh::Process() {
  for ( const auto type : { mjtMeshType::mjVOLUME_MESH, mjtMeshType::mjSHELL_MESH } ) {
    double CoM[3] = {0, 0, 0};
    double facecen[3] = {0, 0, 0};
    double area = 0;
    double inert[6] = {0, 0, 0, 0, 0, 0};

    double nrm[3];
    double cen[3];

    if (type==mjVOLUME_MESH) {
      // translate
      if (refpos[0]!=0 || refpos[1]!=0 || refpos[2]!=0) {
        // prepare translation
        float rp[3] = {(float)refpos[0], (float)refpos[1], (float)refpos[2]};

        // process vertices
        for (int i=0; i<nvert; i++) {
          vert[3*i] -= rp[0];
          vert[3*i+1] -= rp[1];
          vert[3*i+2] -= rp[2];
        }
      }

      // rotate
      if (refquat[0]!=1 || refquat[1]!=0 || refquat[2]!=0 || refquat[3]!=0) {
        // prepare rotation
        mjtNum quat[4] = {refquat[0], refquat[1], refquat[2], refquat[3]};
        mjtNum mat[9];
        mju_normalize4(quat);
        mju_quat2Mat(mat, quat);

        // process vertices
        for (int i=0; i<nvert; i++) {
          mjtNum p1[3], p0[3] = {vert[3*i], vert[3*i+1], vert[3*i+2]};
          mju_rotVecMatT(p1, p0, mat);
          vert[3*i] = (float) p1[0];
          vert[3*i+1] = (float) p1[1];
          vert[3*i+2] = (float) p1[2];
        }

        // process normals
        for (int i=0; i<nnormal; i++) {
          mjtNum n1[3], n0[3] = {normal[3*i], normal[3*i+1], normal[3*i+2]};
          mju_rotVecMatT(n1, n0, mat);
          normal[3*i] = (float) n1[0];
          normal[3*i+1] = (float) n1[1];
          normal[3*i+2] = (float) n1[2];
        }
      }

      // scale
      if (scale[0]!=1 || scale[1]!=1 || scale[2]!=1) {
        for (int i=0; i<nvert; i++) {
          vert[3*i] *= scale[0];
          vert[3*i+1] *= scale[1];
          vert[3*i+2] *= scale[2];
        }

        for (int i=0; i<nnormal; i++) {
          normal[3*i] *= scale[0];
          normal[3*i+1] *= scale[1];
          normal[3*i+2] *= scale[2];
        }
      }

      // normalize normals
      for (int i=0; i<nnormal; i++) {
        // compute length
        float len = normal[3*i]*normal[3*i] + normal[3*i+1]*normal[3*i+1] + normal[3*i+2]*normal[3*i+2];

        // rescale
        if (len>mjMINVAL) {
          float scl = 1/sqrtf(len);
          normal[3*i] *= scl;
          normal[3*i+1] *= scl;
          normal[3*i+2] *= scl;
        } else {
          normal[3*i] = 0;
          normal[3*i+1] = 0;
          normal[3*i+2] = 1;
        }
      }

      // find centroid of faces
      for (int i=0; i<nface; i++) {
        // check vertex indices
        for (int j=0; j<3; j++) {
          if (face[3*i+j]<0 || face[3*i+j]>=nvert) {
            throw mjCError(this, "vertex index out of range in %s (index = %d)", name.c_str(), i);
          }
        }

        // get area and center
        double a = _triangle(nrm, cen, vert+3*face[3*i], vert+3*face[3*i+1], vert+3*face[3*i+2]);

        // accumulate
        for (int j=0; j<3; j++) {
          facecen[j] += a*cen[j];
        }
        area += a;
      }

      // require positive area
      if (area < mjMINVAL) {
        validarea = false;
        return;
      }

      // finalize centroid of faces
      for (int j=0; j<3; j++) {
        facecen[j] /= area;
      }
    }

    // compute CoM and volume from pyramid volumes
    GetVolumeRef(type) = 0;
    for (int i=0; i<nface; i++) {
      // get area, normal and center
      double a = _triangle(nrm, cen, vert+3*face[3*i], vert+3*face[3*i+1], vert+3*face[3*i+2]);

      // compute and add volume
      const double vec[3] = {cen[0]-facecen[0], cen[1]-facecen[1], cen[2]-facecen[2]};
      double vol = type==mjSHELL_MESH ? a : mjuu_dot3(vec, nrm) * a / 3;

      // if legacy computation requested, then always positive
      if (!model->exactmeshinertia) {
        vol = fabs(vol);
      }

      // add pyramid com
      GetVolumeRef(type) += vol;
      for (int j=0; j<3; j++) {
        CoM[j] += vol*(cen[j]*3.0/4.0 + facecen[j]/4.0);
      }
    }

    // require positive volume
    if (GetVolumeRef(type) < mjMINVAL) {
      validvolume = false;
      return;
    }

    // finalize CoM, save as mesh center
    for (int j=0; j<3; j++) {
      CoM[j] /= GetVolumeRef(type);
    }
    mjuu_copyvec(GetPosPtr(type), CoM, 3);

    // re-center mesh at CoM
    if (type==mjVOLUME_MESH) {
      for (int i=0; i<nvert; i++) {
        for (int j=0; j<3; j++) {
          vert[3*i+j] -= CoM[j];
        }
      }
    }

    // accumulate products of inertia, recompute volume
    const int k[6][2] = {{0, 0}, {1, 1}, {2, 2}, {0, 1}, {0, 2}, {1, 2}};
    double P[6] = {0, 0, 0, 0, 0, 0};
    GetVolumeRef(type) = 0;
    for (int i=0; i<nface; i++) {
      float* D = vert+3*face[3*i];
      float* E = vert+3*face[3*i+1];
      float* F = vert+3*face[3*i+2];

      // get area, normal and center; update volume
      double a = _triangle(nrm, cen, D, E, F);
      double vol = type==mjSHELL_MESH ? a : mjuu_dot3(cen, nrm) * a / 3;

      // if legacy computation requested, then always positive
      if (!model->exactmeshinertia) {
        vol = fabs(vol);
      }

      // apply formula, accumulate
      GetVolumeRef(type) += vol;
      for (int j=0; j<6; j++) {
        P[j] += def->geom.density*vol /
                  (type==mjSHELL_MESH ? 12 : 20) * (
                  2*(D[k[j][0]] * D[k[j][1]] +
                    E[k[j][0]] * E[k[j][1]] +
                    F[k[j][0]] * F[k[j][1]]) +
                  D[k[j][0]] * E[k[j][1]]  +  D[k[j][1]] * E[k[j][0]] +
                  D[k[j][0]] * F[k[j][1]]  +  D[k[j][1]] * F[k[j][0]] +
                  E[k[j][0]] * F[k[j][1]]  +  E[k[j][1]] * F[k[j][0]]);
      }
    }

    // convert from products of inertia to moments of inertia
    inert[0] = P[1] + P[2];
    inert[1] = P[0] + P[2];
    inert[2] = P[0] + P[1];
    inert[3] = -P[3];
    inert[4] = -P[4];
    inert[5] = -P[5];

    // get quaternion and diagonal inertia
    mjtNum eigval[3], eigvec[9], quattmp[4];
    mjtNum full[9] = {
      inert[0], inert[3], inert[4],
      inert[3], inert[1], inert[5],
      inert[4], inert[5], inert[2]
    };
    mju_eig3(eigval, eigvec, quattmp, full);

    // check eigval - SHOULD NOT OCCUR
    if (eigval[2]<=0) {
      valideigenvalue = false;
      return;
    }
    if (eigval[0] + eigval[1] < eigval[2] ||
        eigval[0] + eigval[2] < eigval[1] ||
        eigval[1] + eigval[2] < eigval[0]) {
      validinequality = false;
      return;
    }

    // compute sizes of equivalent inertia box
    double mass = GetVolumeRef(type) * def->geom.density;
    double* boxsz = GetInertiaBoxPtr(type);
    boxsz[0] = sqrt(6*(eigval[1]+eigval[2]-eigval[0])/mass)/2;
    boxsz[1] = sqrt(6*(eigval[0]+eigval[2]-eigval[1])/mass)/2;
    boxsz[2] = sqrt(6*(eigval[0]+eigval[1]-eigval[2])/mass)/2;

    // copy quat
    for (int j=0; j<4; j++) {
      GetQuatPtr(type)[j] = type == mjVOLUME_MESH ? quattmp[j] : GetQuatPtr(mjVOLUME_MESH)[j];
    }

    // rotate vertices and normals into axis-aligned frame
    if (type==mjVOLUME_MESH) {
      double neg[4] = {quattmp[0], -quattmp[1], -quattmp[2], -quattmp[3]};
      double mat[9];
      mjuu_quat2mat(mat, neg);
      for (int i=0; i<nvert; i++) {
        // vertices
        const double vec[3] = {vert[3*i], vert[3*i+1], vert[3*i+2]};
        double res[3];
        mjuu_mulvecmat(res, vec, mat);
        for (int j=0; j<3; j++) {
          vert[3*i+j] = (float) res[j];

          // axis-aligned bounding box
          aabb[j+0] = mjMIN(aabb[j+0], res[j]);
          aabb[j+3] = mjMAX(aabb[j+3], res[j]);
        }
      }
      for (int i=0; i<nnormal; i++) {
        // normals
        const double nrm[3] = {normal[3*i], normal[3*i+1], normal[3*i+2]};
        double res[3];
        mjuu_mulvecmat(res, nrm, mat);
        for (int j=0; j<3; j++) {
          normal[3*i+j] = (float) res[j];
        }
      }
    }
  }
}


// check that the mesh is valid
void mjCMesh::CheckMesh() {
  if (!processed) {
    return;
  }
  if (invalidorientation.first>=0 || invalidorientation.second>=0)
    throw mjCError(this,
                   "faces of mesh '%s' have inconsistent orientation. Please check the "
                   "faces containing the vertices %d and %d.",
                   name.c_str(), invalidorientation.first, invalidorientation.second);
  if (!validarea)
    throw mjCError(this, "mesh surface area is too small: %s", name.c_str());
  if (!validvolume)
    throw mjCError(this, "mesh volume is too small: %s", name.c_str());
  if (!valideigenvalue)
    throw mjCError(this, "eigenvalue of mesh inertia must be positive: %s", name.c_str());
  if (!validinequality)
    throw mjCError(this, "eigenvalues of mesh inertia violate A + B >= C: %s", name.c_str());
}


// get inertia pointer
double* mjCMesh::GetInertiaBoxPtr(mjtMeshType type) {
  CheckMesh();
  return type==mjSHELL_MESH ? boxsz_surface : boxsz_volume;
}


double& mjCMesh::GetVolumeRef(mjtMeshType type) {
  CheckMesh();
  return type==mjSHELL_MESH ? surface : volume;
}


// make graph describing convex hull
void mjCMesh::MakeGraph(void) {
  int adr, ok, curlong, totlong, exitcode;
  double* data;
  facetT* facet, **facetp;
  vertexT* vertex, *vertex1, **vertex1p;
  char qhopt[10] = "qhull Qt";

  // graph not needed for small meshes
  if (nvert<4) {
    return;
  }

  // convert mesh data to double
  data = (double*) mju_malloc(3*nvert*sizeof(double));
  if (!data) {
    throw mjCError(this, "could not allocate data for qhull");
  }
  for (int i=0; i<3*nvert; i++) {
    data[i] = (double)vert[i];
  }

  qhT qh_qh;
  qhT* qh = &qh_qh;
  qh_zero(qh, stderr);

  // qhull basic init
  qh_init_A(qh, stdin, stdout, stderr, 0, NULL);

  // install longjmp error handler
  exitcode = setjmp(qh->errexit);
  qh->NOerrexit = false;
  if (!exitcode) {
    // actual init
    qh_initflags(qh, qhopt);
    qh_init_B(qh, data, nvert, 3, False);

    // construct convex hull
    qh_qhull(qh);
    qh_triangulate(qh);
    qh_vertexneighbors(qh);

    // allocate graph:
    //  numvert, numface, vert_edgeadr[numvert], vert_globalid[numvert],
    //  edge_localid[numvert+3*numface], face_globalid[3*numface]
    int numvert = qh->num_vertices;
    int numface = qh->num_facets;
    szgraph = 2 + 3*numvert + 6*numface;
    graph = (int*) mju_malloc(szgraph*sizeof(int));
    graph[0] = numvert;
    graph[1] = numface;

    // pointers for conveniece
    int* vert_edgeadr = graph + 2;
    int* vert_globalid = graph + 2 + numvert;
    int* edge_localid = graph + 2 + 2*numvert;
    int* face_globalid = graph + 2 + 3*numvert + 3*numface;

    // fill in graph data
    int i = adr = 0;
    ok = 1;
    FORALLvertices {
      // point id of this vertex, check
      int pid = qh_pointid(qh, vertex->point);
      if (pid<0 || pid>=nvert) {
        ok = 0;
        break;
      }

      // save edge address and global id of this vertex
      vert_edgeadr[i] = adr;
      vert_globalid[i] = pid;

      // process neighoring faces and their vertices
      int start = adr;
      FOREACHsetelement_(facetT, vertex->neighbors, facet) {
        int cnt = 0;
        FOREACHsetelement_(vertexT, facet->vertices, vertex1) {
          cnt++;

          // point id of face vertex, check
          int pid1 = qh_pointid(qh, vertex1->point);
          if (pid1<0 || pid1>=nvert) {
            ok = 0;
            break;
          }

          // if different from vertex id, try to insert
          if (pid!=pid1) {
            // check for previous record
            int j;
            for (j=start; j<adr; j++)
              if (pid1==edge_localid[j]) {
                break;
              }

            // not found: insert
            if (j>=adr) {
              edge_localid[adr++] = pid1;
            }
          }
        }

        // make sure we have triangle: SHOULD NOT OCCUR
        if (cnt!=3) {
          mju_error("Qhull did not return triangle");
        }
      }

      // insert separator, advance to next vertex
      edge_localid[adr++] = -1;
      i++;
    }

    // size check: SHOULD NOT OCCUR
    if (adr!=numvert+3*numface) {
      mju_error("Wrong size in convex hull graph");
    }

    // add triangle data, reorient faces if flipped
    adr = 0;
    FORALLfacets {
      int ii = 0;
      int ind[3] = {0, 1, 2};
      if (facet->toporient) {
        ind[0] = 1;
        ind[1] = 0;
      }

      // copy triangle data
      FOREACHsetelement_(vertexT, facet->vertices, vertex1) {
        // make sure we have triangle: SHOULD NOT OCCUR
        if (ii>=3) {
          mju_error("Qhull did not return triangle");
        }

        face_globalid[adr + ind[ii++]] = qh_pointid(qh, vertex1->point);
      }

      // advance to next triangle
      adr += 3;
    }

    // free all
    qh_freeqhull(qh, !qh_ALL);
    qh_memfreeshort(qh, &curlong, &totlong);
    mju_free(data);

    // bad graph: delete
    if (!ok) {
      szgraph = 0;
      mju_free(graph);
      graph = 0;
      mju_warning("Could not construct convex hull graph");
    }

    // replace global ids with local ids in edge data
    for (int i=0; i<numvert+3*numface; i++) {
      if (edge_localid[i]>=0) {
        // search vert_globalid for match
        int adr;
        for (adr=0; adr<numvert; adr++) {
          if (vert_globalid[adr]==edge_localid[i]) {
            edge_localid[i] = adr;
            break;
          }
        }

        // make sure we found a match: SHOULD NOT OCCUR
        if (adr>=numvert) {
          mju_error("Vertex id not found in convex hull");
        }
      }
    }
  }

  // longjmp error handler
  else {
    // free all
    qh_freeqhull(qh, !qh_ALL);
    qh_memfreeshort(qh, &curlong, &totlong);
    mju_free(data);
    if (graph) {
      mju_free(graph);
      szgraph = 0;
    }

    throw mjCError(this, "qhull error");
  }
}



// copy graph into face data
void mjCMesh::CopyGraph(void) {
  // only if face data is missing
  if (face) {
    return;
  }

  // get info from graph, allocate
  int numvert = graph[0];
  nface = graph[1];
  face = (int*) mju_malloc(3*nface*sizeof(int));

  // copy faces
  for (int i=0; i<nface; i++) {
    // address in graph
    int j = 2 + 3*numvert + 3*nface + 3*i;

    // copy
    face[3*i] = graph[j];
    face[3*i+1] = graph[j+1];
    face[3*i+2] = graph[j+2];
  }
}



// compute vertex normals
void mjCMesh::MakeNormal(void) {
  // only if normal data is missing
  if (normal) {
    return;
  }

  // allocate and clear normals
  nnormal = nvert;
  normal = (float*) mju_malloc(3*nnormal*sizeof(float));
  memset(normal, 0, 3*nnormal*sizeof(float));

  if (!facenormal) {
    facenormal = (int*) mju_malloc(3*nface*sizeof(int));
    memset(facenormal, 0, 3*nface*sizeof(int));
  }

  // loop over faces, accumulate vertex normals
  for (int i=0; i<nface; i++) {
    // get vertex ids
    int vertid[3];
    for (int j=0; j<3; j++) {
      vertid[j] = face[3*i+j];
    }

    // get triangle edges
    mjtNum vec01[3], vec02[3];
    for (int j=0; j<3; j++) {
      vec01[j] = vert[3*vertid[1]+j] - vert[3*vertid[0]+j];
      vec02[j] = vert[3*vertid[2]+j] - vert[3*vertid[0]+j];
    }

    // compute face normal
    mjtNum nrm[3];
    mju_cross(nrm, vec01, vec02);
    mjtNum area = mju_normalize3(nrm);

    // add normal to each vertex with weight = area
    for (int j=0; j<3; j++) {
      for (int k=0; k<3; k++) {
        normal[3*vertid[j]+k] += nrm[k]*area;
      }
      facenormal[3*i+j] = vertid[j];
    }
  }

  // remove large-angle faces
  if (!smoothnormal) {
    // allocate removal and clear
    float* nremove = (float*) mju_malloc(3*nnormal*sizeof(float));
    memset(nremove, 0, 3*nnormal*sizeof(float));

    // remove contributions from faces at large angles with vertex normal
    for (int i=0; i<nface; i++) {
      // get vertex ids
      int vertid[3];
      for (int j=0; j<3; j++) {
        vertid[j] = face[3*i+j];
      }

      // get triangle edges
      mjtNum vec01[3], vec02[3];
      for (int j=0; j<3; j++) {
        vec01[j] = vert[3*vertid[1]+j] - vert[3*vertid[0]+j];
        vec02[j] = vert[3*vertid[2]+j] - vert[3*vertid[0]+j];
      }

      // compute face normal
      mjtNum nrm[3];
      mju_cross(nrm, vec01, vec02);
      mjtNum area = mju_normalize3(nrm);

      // compare to vertex normal, subtract contribution if dot product too small
      for (int j=0; j<3; j++) {
        // normalized vertex normal
        mjtNum vnrm[3] = {normal[3*vertid[j]], normal[3*vertid[j]+1], normal[3*vertid[j]+2]};
        mju_normalize3(vnrm);

        // dot too small: remove
        if (mju_dot3(nrm, vnrm)<0.8) {
          for (int k=0; k<3; k++) {
            nremove[3*vertid[j]+k] += nrm[k]*area;
          }
        }
      }
    }

    // apply removal, free nremove
    for (int i=0; i<3*nnormal; i++) {
      normal[i] -= nremove[i];
    }
    mju_free(nremove);
  }

  // normalize normals
  for (int i=0; i<nnormal; i++) {
    // compute length
    float len = sqrtf(normal[3*i]*normal[3*i] +
                      normal[3*i+1]*normal[3*i+1] +
                      normal[3*i+2]*normal[3*i+2]);

    // divide by length
    if (len>mjMINVAL)
      for (int j=0; j<3; j++) {
        normal[3*i+j] /= len;
      } else {
        normal[3*i] = normal[3*i+1] = 0;
        normal[3*i+2] = 1;
    }
  }
}



//------------------ class mjCSkin implementation --------------------------------------------------

// constructor
mjCSkin::mjCSkin(mjCModel* _model) {
  // set model pointer
  model = _model;

  // clear data
  file.clear();
  material.clear();
  rgba[0] = rgba[1] = rgba[2] = 0.5f;
  rgba[3] = 1.0f;
  inflate = 0;
  group = 0;

  vert.clear();
  texcoord.clear();
  face.clear();

  bodyname.clear();
  bindpos.clear();
  bindquat.clear();
  vertid.clear();
  vertweight.clear();
  bodyid.clear();

  matid = -1;
}



// destructor
mjCSkin::~mjCSkin() {
  file.clear();
  material.clear();
  vert.clear();
  texcoord.clear();
  face.clear();
  bodyname.clear();
  bindpos.clear();
  bindquat.clear();
  vertid.clear();
  vertweight.clear();
  bodyid.clear();
}



// compiler
void mjCSkin::Compile(const mjVFS* vfs) {

  // load file
  if (!file.empty()) {
    // make sure data is not present
    if (!vert.empty() ||
        !texcoord.empty() ||
        !face.empty() ||
        !bodyname.empty() ||
        !bindpos.empty() ||
        !bindquat.empty() ||
        !vertid.empty() ||
        !vertweight.empty() ||
        !bodyid.empty()) {
      throw mjCError(this, "Data already exists, trying to load from skin file: %s", file.c_str());
    }

    // remove path from file if necessary
    if (model->strippath) {
      file = mjuu_strippath(file);
    }

    // load SKN
    string ext = mjuu_getext(file);
    if (!strcasecmp(ext.c_str(), ".skn")) {
      LoadSKN(vfs);
    } else {
      throw mjCError(this, "Unknown skin file type: %s", file.c_str());
    }
  }

  // make sure all data is present
  if (vert.empty() ||
      face.empty() ||
      bodyname.empty() ||
      bindpos.empty() ||
      bindquat.empty() ||
      vertid.empty() ||
      vertweight.empty()) {
    throw mjCError(this, "Missing data in skin");
  }

  // check mesh sizes
  if (vert.size()%3) {
    throw mjCError(this, "Vertex data must be multiple of 3");
  }
  if (!texcoord.empty() && texcoord.size()!=2*vert.size()/3) {
    throw mjCError(this, "Vertex and texcoord data incompatible size");
  }
  if (face.size()%3) {
    throw mjCError(this, "Face data must be multiple of 3");
  }

  // check bone sizes
  size_t nbone = bodyname.size();
  if (bindpos.size()!=3*nbone) {
    throw mjCError(this, "Unexpected bindpos size in skin");
  }
  if (bindquat.size()!=4*nbone) {
    throw mjCError(this, "Unexpected bindquat size in skin");
  }
  if (vertid.size()!=nbone) {
    throw mjCError(this, "Unexpected vertid size in skin");
  }
  if (vertweight.size()!=nbone) {
    throw mjCError(this, "Unexpected vertweight size in skin");
  }

  // resolve body names
  bodyid.resize(nbone);
  for (int i=0; i<nbone; i++) {
    mjCBase* pbody = model->FindObject(mjOBJ_BODY, bodyname[i]);
    if (!pbody) {
      throw mjCError(this, "unknown body '%s' in skin", bodyname[i].c_str());
    }
    bodyid[i] = pbody->id;
  }

  // resolve material name
  mjCBase* pmat = model->FindObject(mjOBJ_MATERIAL, material);
  if (pmat) {
    matid = pmat->id;
  } else if (!material.empty()) {
      throw mjCError(this, "unkown material '%s' in skin", material.c_str());
  }

  // set total vertex weights to 0
  vector<float> vw;
  size_t nvert = vert.size()/3;
  vw.resize(nvert);
  fill(vw.begin(), vw.end(), 0.0f);

  // accumulate vertex weights from all bones
  for (int i=0; i<nbone; i++) {
    // make sure bone has vertices and sizes match
    size_t nbv = vertid[i].size();
    if (vertweight[i].size()!=nbv || nbv==0) {
      throw mjCError(this, "vertid and vertweight must have same non-zero size in skin");
    }

    // accumulate weights in global array
    for (int j=0; j<nbv; j++) {
      // get index and check range
      int jj = vertid[i][j];
      if (jj<0 || jj>=nvert) {
        throw mjCError(this, "vertid %d out of range in skin", NULL, jj);
      }

      // accumulate
      vw[jj] += vertweight[i][j];
    }
  }

  // check coverage
  for (int i=0; i<nvert; i++) {
    if (vw[i]<=mjMINVAL) {
      throw mjCError(this, "vertex %d must have positive total weight in skin", NULL, i);
    }
  }

  // normalize vertex weights
  for (int i=0; i<nbone; i++) {
    for (int j=0; j<vertid[i].size(); j++) {
      vertweight[i][j] /= vw[vertid[i][j]];
    }
  }

  // normalize bindquat
  for (int i=0; i<nbone; i++) {
    mjtNum quat[4] = {
      (mjtNum)bindquat[4*i],
      (mjtNum)bindquat[4*i+1],
      (mjtNum)bindquat[4*i+2],
      (mjtNum)bindquat[4*i+3]
    };
    mju_normalize4(quat);

    bindquat[4*i]   = (float) quat[0];
    bindquat[4*i+1] = (float) quat[1];
    bindquat[4*i+2] = (float) quat[2];
    bindquat[4*i+3] = (float) quat[3];
  }
}



// load skin in SKN BIN format
void mjCSkin::LoadSKN(const mjVFS* vfs) {
  // make filename
  string filename = mjuu_makefullname(model->modelfiledir, model->meshdir, file);

  // get file data in buffer
  char* buffer = NULL;
  int buffer_sz = 0;
  bool own_buffer = false;
  if (vfs) {
    int id = mj_findFileVFS(vfs, filename.c_str());
    if (id>=0) {
      buffer = (char*)vfs->filedata[id];
      buffer_sz = vfs->filesize[id];
    }
  }

  // if not found in vfs, read from file
  if (!buffer) {
    buffer = (char*) mju_fileToMemory(filename.c_str(), &buffer_sz);
    own_buffer = true;
  }

  // still not found
  if (!buffer) {
    throw mjCError(this, "could not open SKN file '%s'", filename.c_str());
  } else if (!buffer_sz) {
    if (own_buffer) {
      mju_free(buffer);
    }
    throw mjCError(this, "SKN file '%s' is empty", filename.c_str());
  }

  // make sure header is present
  if (buffer_sz<16) {
    if (own_buffer) {
      mju_free(buffer);
    }
    throw mjCError(this, "missing header in SKN file '%s'", filename.c_str());
  }

  // get sizes from header
  int nvert = ((int*)buffer)[0];
  int ntexcoord = ((int*)buffer)[1];
  int nface = ((int*)buffer)[2];
  int nbone = ((int*)buffer)[3];

  // negative sizes not allowed
  if (nvert<0 || ntexcoord<0 || nface<0 || nbone<0) {
    if (own_buffer) {
      mju_free(buffer);
    }
    throw mjCError(this, "negative size in header of SKN file '%s'", filename.c_str());
  }

  // make sure we have data for vert, texcoord, face
  if (buffer_sz < 16 + 12*nvert + 8*ntexcoord + 12*nface) {
    if (own_buffer) {
      mju_free(buffer);
    }
    throw mjCError(this, "insufficient data in SKN file '%s'", filename.c_str());
  }

  // data pointer and counter
  float* pdata = (float*)(buffer+16);
  int cnt = 0;

  // copy vert
  if (nvert) {
    vert.resize(3*nvert);
    memcpy(vert.data(), pdata+cnt, 3*nvert*sizeof(float));
    cnt += 3*nvert;
  }

  // copy texcoord
  if (ntexcoord) {
    texcoord.resize(2*ntexcoord);
    memcpy(texcoord.data(), pdata+cnt, 2*ntexcoord*sizeof(float));
    cnt += 2*ntexcoord;
  }

  // copy face
  if (nface) {
    face.resize(3*nface);
    memcpy(face.data(), pdata+cnt, 3*nface*sizeof(int));
    cnt += 3*nface;
  }

  // allocate bone arrays
  bodyname.clear();
  bindpos.resize(3*nbone);
  bindquat.resize(4*nbone);
  vertid.resize(nbone);
  vertweight.resize(nbone);

  // read bones
  for (int i=0; i<nbone; i++) {
    // check size
    if (buffer_sz/4-4-cnt < 18) {
      if (own_buffer) {
        mju_free(buffer);
      }
      throw mjCError(this, "insufficient data in SKN file '%s', bone %d", filename.c_str(), i);
    }

    // read name
    char txt[40];
    strncpy(txt, (char*)(pdata+cnt), 39);
    txt[39] = '\0';
    cnt += 10;
    bodyname.push_back(txt);

    // read bindpos
    memcpy(bindpos.data()+3*i, pdata+cnt, 3*sizeof(float));
    cnt += 3;

    // read bind quat
    memcpy(bindquat.data()+4*i, pdata+cnt, 4*sizeof(float));
    cnt += 4;

    // read vertex count
    int vcount = *(int*)(pdata+cnt);
    cnt += 1;

    // check for negative
    if (vcount<1) {
      if (own_buffer) {
        mju_free(buffer);
      }
      throw mjCError(this, "vertex count must be positive in SKN file '%s', bone %d",
                     filename.c_str(), i);
    }

    // check size
    if (buffer_sz/4-4-cnt < 2*vcount) {
      if (own_buffer) {
        mju_free(buffer);
      }
      throw mjCError(this, "insufficient vertex data in SKN file '%s', bone %d",
                     filename.c_str(), i);
    }

    // read vertid
    vertid[i].resize(vcount);
    memcpy(vertid[i].data(), (int*)(pdata+cnt), vcount*sizeof(int));
    cnt += vcount;

    // read vertweight
    vertweight[i].resize(vcount);
    memcpy(vertweight[i].data(), (int*)(pdata+cnt), vcount*sizeof(int));
    cnt += vcount;
  }

  // free buffer if allocated here
  if (own_buffer) {
    mju_free(buffer);
  }

  // check final size
  if (buffer_sz != 16+4*cnt) {
    throw mjCError(this, "unexpected buffer size in SKN file '%s'", filename.c_str());
  }
}
