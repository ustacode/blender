/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <algorithm>
#include <array>
#include <cfloat>

#include "BLI_bounds.hh"
#include "BLI_math_base.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"

#include "BKE_attribute.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"

#include "DNA_mesh_types.h"

#include "GEO_isotropic_remesh.hh"
#include "GEO_mesh_primitive_cuboid.hh"
#include "GEO_mesh_primitive_grid.hh"
#include "GEO_mesh_primitive_uv_sphere.hh"

#include "CLG_log.h"
#include "testing/testing.h"

namespace blender::geometry::tests {

class IsotropicRemeshTest : public testing::Test {
 public:
  static void SetUpTestSuite()
  {
    CLG_init();
    BKE_idtype_init();
  }
  static void TearDownTestSuite()
  {
    CLG_exit();
  }
};

/* -------------------------------------------------------------------------- */
/* Helpers */

static float average_edge_length(const Mesh &mesh)
{
  const Span<float3> positions = mesh.vert_positions();
  const Span<int2> edges = mesh.edges();
  if (edges.is_empty()) {
    return 0.0f;
  }
  float total = 0.0f;
  for (const int2 &edge : edges) {
    total += math::distance(positions[edge[0]], positions[edge[1]]);
  }
  return total / float(edges.size());
}

/* Build a mesh directly from a triangle soup (used for degenerate / disconnected cases). */
static Mesh *mesh_from_tris(const Span<float3> positions, const Span<std::array<int, 3>> tris)
{
  Mesh *mesh = bke::mesh_new_no_attributes(
      positions.size(), 0, tris.size(), tris.size() * 3);
  MutableSpan<int> offsets = mesh->face_offsets_for_write();
  for (const int i : IndexRange(tris.size() + 1)) {
    offsets[i] = i * 3;
  }
  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  attributes.add<int>(".corner_vert", bke::AttrDomain::Corner, bke::AttributeInitConstruct());
  attributes.add<float3>("position", bke::AttrDomain::Point, bke::AttributeInitConstruct());
  mesh->vert_positions_for_write().copy_from(positions);
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
  for (const int i : tris.index_range()) {
    corner_verts[i * 3 + 0] = tris[i][0];
    corner_verts[i * 3 + 1] = tris[i][1];
    corner_verts[i * 3 + 2] = tris[i][2];
  }
  bke::mesh_calc_edges(*mesh, false, false);
  return mesh;
}

static Mesh *make_tetrahedron(const float3 &center, const float scale)
{
  const std::array<float3, 4> base = {float3(1, 1, 1),
                                      float3(1, -1, -1),
                                      float3(-1, 1, -1),
                                      float3(-1, -1, 1)};
  std::array<float3, 4> positions;
  for (const int i : IndexRange(4)) {
    positions[i] = center + base[i] * scale;
  }
  const std::array<std::array<int, 3>, 4> tris = {
      {{0, 1, 2}, {0, 3, 1}, {0, 2, 3}, {1, 3, 2}}};
  return mesh_from_tris(positions, tris);
}

static bool all_faces_are_triangles(const Mesh &mesh)
{
  return mesh.faces_num > 0 && mesh.corners_num == mesh.faces_num * 3;
}

/* -------------------------------------------------------------------------- */
/* A closed manifold surface remeshes into valid, uniform triangles. */

TEST_F(IsotropicRemeshTest, ClosedManifoldSphere)
{
  Mesh *sphere = create_uv_sphere_mesh(1.0f, 32, 16, std::nullopt);
  const float target = 0.2f;

  IsotropicRemeshParams params;
  params.target_edge_length = target;
  params.iterations = 5;
  Mesh *result = isotropic_remesh(*sphere, params);

  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(all_faces_are_triangles(*result));
  EXPECT_TRUE(BKE_mesh_is_valid(result));

  /* Every edge should land inside the algorithm's [4/5, 4/3] * target acceptance band, so the
   * average must be comfortably within a wider tolerance. */
  const float avg = average_edge_length(*result);
  EXPECT_GT(avg, target * 0.5f);
  EXPECT_LT(avg, target * 2.0f);

  BKE_id_free(nullptr, result);
  BKE_id_free(nullptr, sphere);
}

/* Refining below the input density should increase the triangle count. */
TEST_F(IsotropicRemeshTest, RefinementIncreasesResolution)
{
  Mesh *sphere = create_uv_sphere_mesh(1.0f, 16, 8, std::nullopt);
  IsotropicRemeshParams params;
  params.target_edge_length = 0.1f;
  params.iterations = 5;
  Mesh *result = isotropic_remesh(*sphere, params);

  ASSERT_NE(result, nullptr);
  EXPECT_GT(result->faces_num, sphere->faces_num);

  BKE_id_free(nullptr, result);
  BKE_id_free(nullptr, sphere);
}

/* -------------------------------------------------------------------------- */
/* Boundary preservation keeps the outline of an open mesh in place. */

TEST_F(IsotropicRemeshTest, BoundaryPreservation)
{
  Mesh *grid = create_grid_mesh(8, 8, 2.0f, 2.0f, std::nullopt);
  const Bounds<float3> src_bounds = *bounds::min_max(grid->vert_positions());

  IsotropicRemeshParams params;
  params.target_edge_length = 0.15f;
  params.iterations = 5;
  params.preserve_boundary = true;
  Mesh *result = isotropic_remesh(*grid, params);

  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(all_faces_are_triangles(*result));
  const Bounds<float3> dst_bounds = *bounds::min_max(result->vert_positions());

  /* With the boundary pinned the planar outline (and hence the bounding box) must be preserved. */
  EXPECT_NEAR(dst_bounds.min.x, src_bounds.min.x, 1e-4f);
  EXPECT_NEAR(dst_bounds.min.y, src_bounds.min.y, 1e-4f);
  EXPECT_NEAR(dst_bounds.max.x, src_bounds.max.x, 1e-4f);
  EXPECT_NEAR(dst_bounds.max.y, src_bounds.max.y, 1e-4f);

  BKE_id_free(nullptr, result);
  BKE_id_free(nullptr, grid);
}

/* -------------------------------------------------------------------------- */
/* Sharp edges/corners of a cube are kept when sharp-edge preservation is on. */

TEST_F(IsotropicRemeshTest, SharpEdgePreservation)
{
  Mesh *cube = create_cuboid_mesh(float3(2, 2, 2), 2, 2, 2);
  const Bounds<float3> src_bounds = *bounds::min_max(cube->vert_positions());

  IsotropicRemeshParams params;
  params.target_edge_length = 0.2f;
  params.iterations = 10;
  params.preserve_sharp_edges = true;
  params.sharp_angle = DEG2RADF(30.0f);
  Mesh *result = isotropic_remesh(*cube, params);
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(all_faces_are_triangles(*result));

  /* With the 12 sharp edges pinned, the cube's extent (its 8 corners) must be preserved;
   * without the fix the corners round inward and the bounding box shrinks noticeably. */
  const Bounds<float3> dst_bounds = *bounds::min_max(result->vert_positions());
  for (const int axis : IndexRange(3)) {
    EXPECT_NEAR(dst_bounds.min[axis], src_bounds.min[axis], 0.05f);
    EXPECT_NEAR(dst_bounds.max[axis], src_bounds.max[axis], 0.05f);
  }

  /* Each of the 8 corners must still be hit by an output vertex. */
  const Span<float3> out_positions = result->vert_positions();
  const std::array<float3, 8> corners = {float3(-1, -1, -1),
                                         float3(1, -1, -1),
                                         float3(-1, 1, -1),
                                         float3(1, 1, -1),
                                         float3(-1, -1, 1),
                                         float3(1, -1, 1),
                                         float3(-1, 1, 1),
                                         float3(1, 1, 1)};
  for (const float3 &corner : corners) {
    float nearest = FLT_MAX;
    for (const float3 &p : out_positions) {
      nearest = std::min(nearest, math::distance(p, corner));
    }
    EXPECT_LT(nearest, 0.1f) << "cube corner was not preserved";
  }

  BKE_id_free(nullptr, result);
  BKE_id_free(nullptr, cube);
}

/* -------------------------------------------------------------------------- */
/* Disconnected components are all remeshed without crashing. */

TEST_F(IsotropicRemeshTest, DisconnectedGeometry)
{
  Mesh *tet_a = make_tetrahedron(float3(-3, 0, 0), 1.0f);
  Mesh *tet_b = make_tetrahedron(float3(3, 0, 0), 1.0f);

  /* Merge the two tetrahedra into one mesh with two islands. */
  const int verts_num = tet_a->verts_num + tet_b->verts_num;
  const int tris_num = tet_a->faces_num + tet_b->faces_num;
  Array<float3> positions(verts_num);
  Array<std::array<int, 3>> tris(tris_num);
  positions.as_mutable_span().slice(0, 4).copy_from(tet_a->vert_positions());
  positions.as_mutable_span().slice(4, 4).copy_from(tet_b->vert_positions());
  const Span<int> a_corners = tet_a->corner_verts();
  const Span<int> b_corners = tet_b->corner_verts();
  for (const int i : IndexRange(4)) {
    tris[i] = {a_corners[i * 3], a_corners[i * 3 + 1], a_corners[i * 3 + 2]};
    tris[i + 4] = {
        b_corners[i * 3] + 4, b_corners[i * 3 + 1] + 4, b_corners[i * 3 + 2] + 4};
  }
  Mesh *combined = mesh_from_tris(positions, tris);

  IsotropicRemeshParams params;
  params.target_edge_length = 0.5f;
  params.iterations = 3;
  Mesh *result = isotropic_remesh(*combined, params);

  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(all_faces_are_triangles(*result));
  EXPECT_TRUE(BKE_mesh_is_valid(result));
  /* Both islands should survive, so the result must be richer than a single remeshed tet. */
  EXPECT_GT(result->faces_num, 8);

  BKE_id_free(nullptr, result);
  BKE_id_free(nullptr, combined);
  BKE_id_free(nullptr, tet_a);
  BKE_id_free(nullptr, tet_b);
}

/* -------------------------------------------------------------------------- */
/* Degenerate (repeated-index) triangles are skipped rather than crashing. */

TEST_F(IsotropicRemeshTest, DegenerateTrianglesAreSkipped)
{
  const std::array<float3, 5> positions = {float3(1, 1, 1),
                                           float3(1, -1, -1),
                                           float3(-1, 1, -1),
                                           float3(-1, -1, 1),
                                           float3(2, 2, 2)};
  /* A valid tetrahedron plus one topologically degenerate face (v4, v4, v0). */
  const std::array<std::array<int, 3>, 5> tris = {
      {{0, 1, 2}, {0, 3, 1}, {0, 2, 3}, {1, 3, 2}, {4, 4, 0}}};
  Mesh *mesh = mesh_from_tris(positions, tris);

  IsotropicRemeshParams params;
  params.target_edge_length = 0.5f;
  params.iterations = 3;
  Mesh *result = isotropic_remesh(*mesh, params);

  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(all_faces_are_triangles(*result));
  EXPECT_TRUE(BKE_mesh_is_valid(result));

  BKE_id_free(nullptr, result);
  BKE_id_free(nullptr, mesh);
}

/* -------------------------------------------------------------------------- */
/* Point, face and corner attributes are resampled onto the result. */

TEST_F(IsotropicRemeshTest, AttributeTransfer)
{
  Mesh *sphere = create_uv_sphere_mesh(1.0f, 24, 12, "uv_map");

  /* Add a point attribute equal to the vertex height and a constant face attribute. */
  bke::MutableAttributeAccessor attributes = sphere->attributes_for_write();
  bke::SpanAttributeWriter<float> height = attributes.lookup_or_add_for_write_span<float>(
      "height", bke::AttrDomain::Point);
  const Span<float3> positions = sphere->vert_positions();
  for (const int i : positions.index_range()) {
    height.span[i] = positions[i].z;
  }
  height.finish();
  bke::SpanAttributeWriter<int> region = attributes.lookup_or_add_for_write_span<int>(
      "region", bke::AttrDomain::Face);
  region.span.fill(7);
  region.finish();

  IsotropicRemeshParams params;
  params.target_edge_length = 0.2f;
  params.iterations = 4;
  params.transfer_attributes = true;
  Mesh *result = isotropic_remesh(*sphere, params);
  ASSERT_NE(result, nullptr);

  const bke::AttributeAccessor result_attributes = result->attributes();

  /* Point attribute survives and stays within the source range [-1, 1]. */
  const VArray<float> out_height = *result_attributes.lookup<float>("height",
                                                                    bke::AttrDomain::Point);
  ASSERT_TRUE(out_height);
  for (const int i : IndexRange(result->verts_num)) {
    EXPECT_GE(out_height[i], -1.01f);
    EXPECT_LE(out_height[i], 1.01f);
  }

  /* Face attribute was constant, so every resampled face must keep that value. */
  const VArray<int> out_region = *result_attributes.lookup<int>("region", bke::AttrDomain::Face);
  ASSERT_TRUE(out_region);
  for (const int i : IndexRange(result->faces_num)) {
    EXPECT_EQ(out_region[i], 7);
  }

  /* The UV map (corner domain) is carried across as well. */
  EXPECT_TRUE(result_attributes.contains("uv_map"));

  BKE_id_free(nullptr, result);
  BKE_id_free(nullptr, sphere);
}

/* -------------------------------------------------------------------------- */
/* A moderately sized mesh completes and the cancellation callback is honored. */

TEST_F(IsotropicRemeshTest, PerformanceAndCancellation)
{
  Mesh *sphere = create_uv_sphere_mesh(1.0f, 64, 32, std::nullopt);

  /* Cancel immediately: the first iteration callback returns false. */
  IsotropicRemeshParams params;
  params.target_edge_length = 0.05f;
  params.iterations = 20;
  int callback_calls = 0;
  Mesh *cancelled = isotropic_remesh(
      *sphere, params, bke::AttributeFilter::default_filter(), [&](const float /*progress*/) {
        callback_calls++;
        return false;
      });
  ASSERT_NE(cancelled, nullptr);
  EXPECT_EQ(callback_calls, 1);
  EXPECT_TRUE(all_faces_are_triangles(*cancelled));
  BKE_id_free(nullptr, cancelled);

  /* Full run to completion on the same input. */
  Mesh *result = isotropic_remesh(*sphere, params);
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(all_faces_are_triangles(*result));
  EXPECT_TRUE(BKE_mesh_is_valid(result));
  BKE_id_free(nullptr, result);

  BKE_id_free(nullptr, sphere);
}

}  // namespace blender::geometry::tests
