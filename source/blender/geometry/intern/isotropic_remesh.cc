/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>
#include <array>
#include <cfloat>
#include <limits>
#include <vector>

#include "BLI_array.hh"
#include "BLI_index_mask.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_math_geom.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_string_ref.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "DNA_mesh_types.h"

#include "BKE_attribute.hh"
#include "BKE_bvhutils.hh"
#include "BKE_mesh.h"
#include "BKE_mesh.hh"
#include "BKE_mesh_sample.hh"

#include "GEO_isotropic_remesh.hh"

/* Vendored MIT isotropic remesher (extern/isotropicremesher). */
#include "isotropichalfedgemesh.h"
#include "isotropicremesher.h"
#include "vector3.h"

namespace blender::geometry {

/* -------------------------------------------------------------------------- */
/** \name Input conversion (Blender #Mesh -> remesher triangle soup)
 * \{ */

/* Compacted triangle soup fed to the remesher. Only vertices that are actually referenced by a
 * triangle are kept, so loose vertices never leak into the result and indices stay dense. */
struct RemeshInput {
  std::vector<Vector3> vertices;
  std::vector<std::vector<size_t>> triangles;
};

static RemeshInput mesh_to_remesh_input(const Mesh &mesh)
{
  const Span<float3> positions = mesh.vert_positions();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int3> corner_tris = mesh.corner_tris();

  RemeshInput input;
  input.vertices.reserve(positions.size());
  input.triangles.reserve(corner_tris.size());

  Array<int> vert_map(positions.size(), -1);
  for (const int3 &tri : corner_tris) {
    std::array<size_t, 3> remapped;
    for (const int i : IndexRange(3)) {
      const int vert = corner_verts[tri[i]];
      if (vert_map[vert] == -1) {
        vert_map[vert] = int(input.vertices.size());
        const float3 &p = positions[vert];
        input.vertices.push_back(Vector3(double(p.x), double(p.y), double(p.z)));
      }
      remapped[i] = size_t(vert_map[vert]);
    }
    /* Skip topologically degenerate triangles: the half-edge builder assumes every directed edge
     * of a face is unique, and a repeated vertex would create a zero-length edge. */
    if (remapped[0] == remapped[1] || remapped[1] == remapped[2] || remapped[0] == remapped[2]) {
      continue;
    }
    input.triangles.push_back({remapped[0], remapped[1], remapped[2]});
  }
  return input;
}

/** \} */

/* -------------------------------------------------------------------------- */
/** \name Output conversion (remesher half-edge mesh -> Blender #Mesh)
 * \{ */

/* Walk the remeshed half-edge mesh once, assigning a dense output index to every referenced
 * vertex and collecting triangle corner indices. The winding matches the upstream test harness
 * (previous, current, next start-vertex) so face normals stay consistent. */
static Mesh *halfedge_mesh_to_mesh(IsotropicHalfedgeMesh &halfedge_mesh, const Mesh &src_mesh)
{
  using Vertex = IsotropicHalfedgeMesh::Vertex;
  using Face = IsotropicHalfedgeMesh::Face;

  for (Vertex *vertex = halfedge_mesh.moveToNextVertex(nullptr); vertex != nullptr;
       vertex = halfedge_mesh.moveToNextVertex(vertex))
  {
    vertex->outputIndex = std::numeric_limits<size_t>::max();
  }

  Vector<float3> positions;
  Vector<std::array<int, 3>> tris;

  auto output_index = [&](Vertex *vertex) -> int {
    if (vertex->outputIndex == std::numeric_limits<size_t>::max()) {
      vertex->outputIndex = size_t(positions.size());
      positions.append(float3(float(vertex->position[0]),
                              float(vertex->position[1]),
                              float(vertex->position[2])));
    }
    return int(vertex->outputIndex);
  };

  for (Face *face = halfedge_mesh.moveToNextFace(nullptr); face != nullptr;
       face = halfedge_mesh.moveToNextFace(face))
  {
    Vertex *v0 = face->halfedge->previousHalfedge->startVertex;
    Vertex *v1 = face->halfedge->startVertex;
    Vertex *v2 = face->halfedge->nextHalfedge->startVertex;
    tris.append({output_index(v0), output_index(v1), output_index(v2)});
  }

  if (tris.is_empty() || positions.is_empty()) {
    return nullptr;
  }

  const int verts_num = positions.size();
  const int faces_num = tris.size();
  const int corners_num = faces_num * 3;

  Mesh *mesh = bke::mesh_new_no_attributes(verts_num, 0, faces_num, corners_num);
  BKE_mesh_copy_parameters_for_eval(mesh, &src_mesh);

  /* Uniform triangle fan: every face has exactly three corners. */
  MutableSpan<int> face_offsets = mesh->face_offsets_for_write();
  threading::parallel_for(IndexRange(faces_num + 1), 4096, [&](const IndexRange range) {
    for (const int face : range) {
      face_offsets[face] = face * 3;
    }
  });

  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  attributes.add<int>(".corner_vert", bke::AttrDomain::Corner, bke::AttributeInitConstruct());
  attributes.add<float3>("position", bke::AttrDomain::Point, bke::AttributeInitConstruct());

  MutableSpan<float3> mesh_positions = mesh->vert_positions_for_write();
  mesh_positions.copy_from(positions);

  MutableSpan<int> mesh_corner_verts = mesh->corner_verts_for_write();
  threading::parallel_for(IndexRange(faces_num), 4096, [&](const IndexRange range) {
    for (const int face : range) {
      mesh_corner_verts[face * 3 + 0] = tris[face][0];
      mesh_corner_verts[face * 3 + 1] = tris[face][1];
      mesh_corner_verts[face * 3 + 2] = tris[face][2];
    }
  });

  bke::mesh_calc_edges(*mesh, false, false);
  mesh->tag_positions_changed();

  return mesh;
}

/** \} */

/* -------------------------------------------------------------------------- */
/** \name Attribute transfer (nearest-surface resampling)
 * \{ */

static bool topology_attribute(const StringRef name)
{
  return name == "position" || name == ".corner_vert" || name == ".corner_edge" ||
         name == ".edge_verts";
}

/* Resample the input's point/face/corner attributes onto the remeshed result. For each output
 * vertex the nearest point on the original surface gives a source triangle and barycentric
 * weights; face attributes use the nearest source face at each output face centroid. Edge-domain
 * attributes have no meaningful mapping after a full retopology and are intentionally dropped. */
static void transfer_attributes(const Mesh &src_mesh,
                                Mesh &dst_mesh,
                                const bke::AttributeFilter &attribute_filter)
{
  const Span<float3> src_positions = src_mesh.vert_positions();
  const Span<int> src_corner_verts = src_mesh.corner_verts();
  const Span<int3> src_corner_tris = src_mesh.corner_tris();
  const Span<int> src_tri_faces = src_mesh.corner_tri_faces();
  if (src_corner_tris.is_empty()) {
    return;
  }

  bke::BVHTreeFromMesh bvh = src_mesh.bvh_corner_tris();
  if (bvh.tree == nullptr) {
    return;
  }

  const Span<float3> dst_positions = dst_mesh.vert_positions();
  const OffsetIndices<int> dst_faces = dst_mesh.faces();
  const Span<int> dst_corner_verts = dst_mesh.corner_verts();
  const int dst_verts_num = dst_mesh.verts_num;
  const int dst_faces_num = dst_mesh.faces_num;
  const int dst_corners_num = dst_mesh.corners_num;

  /* Per output vertex: nearest source triangle and barycentric weights. */
  Array<int> vert_tri(dst_verts_num);
  Array<float3> vert_bary(dst_verts_num);
  threading::parallel_for(IndexRange(dst_verts_num), 2048, [&](const IndexRange range) {
    for (const int i : range) {
      BVHTreeNearest nearest;
      nearest.dist_sq = FLT_MAX;
      nearest.index = -1;
      BLI_bvhtree_find_nearest(
          bvh.tree, dst_positions[i], &nearest, bvh.nearest_callback, &bvh);
      if (nearest.index < 0) {
        vert_tri[i] = 0;
        vert_bary[i] = float3(1.0f, 0.0f, 0.0f);
        continue;
      }
      vert_tri[i] = nearest.index;
      const int3 &tri = src_corner_tris[nearest.index];
      float weights[3];
      interp_weights_tri_v3(weights,
                            src_positions[src_corner_verts[tri[0]]],
                            src_positions[src_corner_verts[tri[1]]],
                            src_positions[src_corner_verts[tri[2]]],
                            nearest.co);
      vert_bary[i] = float3(weights[0], weights[1], weights[2]);
    }
  });

  /* Per output face: nearest source triangle at the face centroid. */
  Array<int> face_tri(dst_faces_num);
  threading::parallel_for(IndexRange(dst_faces_num), 2048, [&](const IndexRange range) {
    for (const int face : range) {
      float3 centroid(0.0f);
      for (const int corner : dst_faces[face]) {
        centroid += dst_positions[dst_corner_verts[corner]];
      }
      centroid /= float(dst_faces[face].size());
      BVHTreeNearest nearest;
      nearest.dist_sq = FLT_MAX;
      nearest.index = -1;
      BLI_bvhtree_find_nearest(bvh.tree, centroid, &nearest, bvh.nearest_callback, &bvh);
      face_tri[face] = std::max(nearest.index, 0);
    }
  });

  /* Per output corner: reuse the owning vertex's nearest-surface result. */
  Array<int> corner_tri(dst_corners_num);
  Array<float3> corner_bary(dst_corners_num);
  threading::parallel_for(IndexRange(dst_corners_num), 4096, [&](const IndexRange range) {
    for (const int corner : range) {
      const int vert = dst_corner_verts[corner];
      corner_tri[corner] = vert_tri[vert];
      corner_bary[corner] = vert_bary[vert];
    }
  });

  const bke::AttributeAccessor src_attributes = src_mesh.attributes();
  bke::MutableAttributeAccessor dst_attributes = dst_mesh.attributes_for_write();

  const IndexMask verts_mask(dst_verts_num);
  const IndexMask faces_mask(dst_faces_num);
  const IndexMask corners_mask(dst_corners_num);

  src_attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (topology_attribute(iter.name)) {
      return;
    }
    /* Strings are the only attribute type nearest-surface sampling cannot handle. */
    if (iter.data_type == bke::AttrType::String) {
      return;
    }
    /* Edge topology is fully rebuilt, so edge attributes cannot be mapped. */
    if (iter.domain == bke::AttrDomain::Edge) {
      return;
    }
    if (attribute_filter.allow_skip(iter.name)) {
      return;
    }
    const bke::GAttributeReader src = iter.get();
    if (!src) {
      return;
    }

    switch (iter.domain) {
      case bke::AttrDomain::Point: {
        bke::GSpanAttributeWriter dst = dst_attributes.lookup_or_add_for_write_only_span(
            iter.name, bke::AttrDomain::Point, iter.data_type);
        if (!dst) {
          return;
        }
        bke::mesh_surface_sample::sample_point_attribute(
            src_corner_verts, src_corner_tris, vert_tri, vert_bary, src.varray, verts_mask, dst.span);
        dst.finish();
        break;
      }
      case bke::AttrDomain::Face: {
        bke::GSpanAttributeWriter dst = dst_attributes.lookup_or_add_for_write_only_span(
            iter.name, bke::AttrDomain::Face, iter.data_type);
        if (!dst) {
          return;
        }
        bke::mesh_surface_sample::sample_face_attribute(
            src_tri_faces, face_tri, src.varray, faces_mask, dst.span);
        dst.finish();
        break;
      }
      case bke::AttrDomain::Corner: {
        bke::GSpanAttributeWriter dst = dst_attributes.lookup_or_add_for_write_only_span(
            iter.name, bke::AttrDomain::Corner, iter.data_type);
        if (!dst) {
          return;
        }
        bke::mesh_surface_sample::sample_corner_attribute(
            src_corner_tris, corner_tri, corner_bary, src.varray, corners_mask, dst.span);
        dst.finish();
        break;
      }
      default:
        break;
    }
  });
}

/** \} */

/* -------------------------------------------------------------------------- */
/** \name Public entry point
 * \{ */

Mesh *isotropic_remesh(const Mesh &mesh,
                       const IsotropicRemeshParams &params,
                       const bke::AttributeFilter &attribute_filter,
                       const std::function<bool(float progress)> &progress_fn)
{
  RemeshInput input = mesh_to_remesh_input(mesh);
  if (input.triangles.empty() || input.vertices.empty()) {
    return nullptr;
  }

  IsotropicRemesher remesher(&input.vertices, &input.triangles);
  remesher.setPreserveBoundaries(params.preserve_boundary);
  if (params.preserve_sharp_edges) {
    remesher.setSharpEdgeThresholdRadians(std::max(double(params.sharp_angle), 1.0e-4));
  }
  if (params.target_edge_length > 0.0f) {
    remesher.setTargetEdgeLength(double(params.target_edge_length));
  }

  const size_t iterations = size_t(std::max(params.iterations, 0));

  std::function<bool(size_t, size_t)> iteration_callback;
  if (progress_fn) {
    iteration_callback = [&](const size_t current, const size_t total) -> bool {
      const float progress = total > 0 ? float(current) / float(total) : 1.0f;
      return progress_fn(progress);
    };
  }

  remesher.remesh(iterations, iteration_callback);

  IsotropicHalfedgeMesh *halfedge_mesh = remesher.remeshedHalfedgeMesh();
  if (halfedge_mesh == nullptr) {
    return nullptr;
  }

  Mesh *result = halfedge_mesh_to_mesh(*halfedge_mesh, mesh);
  if (result == nullptr) {
    return nullptr;
  }

  if (params.transfer_attributes) {
    transfer_attributes(mesh, *result, attribute_filter);
  }

  return result;
}

/** \} */

}  // namespace blender::geometry
