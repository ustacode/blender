/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_mesh_types.h"

#include "BLI_math_base.h"

#include "GEO_foreach_geometry.hh"
#include "GEO_isotropic_remesh.hh"
#include "GEO_randomize.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_isotropic_remesh_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Geometry>("Mesh")
      .supported_type(GeometryComponent::Type::Mesh)
      .is_default_link_socket()
      .description("Mesh to remesh into uniform triangles");
  b.add_output<decl::Geometry>("Mesh").propagate_all().align_with_previous();

  b.add_input<decl::Float>("Target Edge Length")
      .default_value(0.1f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .description(
          "Desired length of the triangle edges in the result. "
          "A value of zero uses the average edge length of the input mesh");
  b.add_input<decl::Int>("Iterations")
      .default_value(10)
      .min(0)
      .max(100)
      .description("Number of split/collapse/flip/relax passes to run");
  b.add_input<decl::Bool>("Preserve Boundary")
      .default_value(true)
      .description("Keep open boundary edges in place instead of collapsing or moving them");
  b.add_input<decl::Bool>("Preserve Sharp Edges")
      .default_value(false)
      .description("Keep edges whose adjacent faces meet at more than the sharp angle");
  b.add_input<decl::Float>("Sharp Angle")
      .default_value(DEG2RADF(30.0f))
      .min(0.0f)
      .max(float(M_PI))
      .subtype(PROP_ANGLE)
      .description(
          "Edges where the angle between adjacent face normals is larger than this are treated "
          "as sharp features when \"Preserve Sharp Edges\" is enabled");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh");
  const AttributeFilter &attribute_filter = params.get_attribute_filter("Mesh");

  geometry::IsotropicRemeshParams remesh_params;
  remesh_params.target_edge_length = std::max(params.extract_input<float>("Target Edge Length"),
                                              0.0f);
  remesh_params.iterations = std::max(params.extract_input<int>("Iterations"), 0);
  remesh_params.preserve_boundary = params.extract_input<bool>("Preserve Boundary");
  remesh_params.preserve_sharp_edges = params.extract_input<bool>("Preserve Sharp Edges");
  remesh_params.sharp_angle = params.extract_input<float>("Sharp Angle");
  remesh_params.transfer_attributes = true;

  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
    const Mesh *src_mesh = geometry_set.get_mesh();
    if (src_mesh == nullptr) {
      return;
    }
    if (src_mesh->faces_num == 0) {
      /* Nothing to remesh; leave the geometry untouched. */
      return;
    }

    Mesh *result = geometry::isotropic_remesh(*src_mesh, remesh_params, attribute_filter);
    if (result == nullptr) {
      params.error_message_add(NodeWarningType::Warning,
                               TIP_("Remeshing produced no geometry; the input was kept"));
      return;
    }

    geometry::debug_randomize_mesh_order(result);
    geometry_set.replace_mesh(result);
  });

  params.set_output("Mesh", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeIsotropicRemesh", GEO_NODE_ISOTROPIC_REMESH);
  ntype.ui_name = "Isotropic Remesh";
  ntype.ui_description =
      "Rebuild a mesh out of uniformly sized, near-equilateral triangles that follow the "
      "original surface";
  ntype.enum_name_legacy = "ISOTROPIC_REMESH";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_isotropic_remesh_cc
