/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <functional>

#include "BKE_attribute_filter.hh"

struct Mesh;

namespace blender::geometry {

struct IsotropicRemeshParams {
  /** Desired edge length. Values <= 0 fall back to the input's average edge length. */
  float target_edge_length = 0.0f;
  /** Number of remeshing iterations (each iteration does split/collapse/flip/relax/project). */
  int iterations = 10;
  /** Pin open boundary edges so the mesh silhouette/holes are preserved. */
  bool preserve_boundary = true;
  /** Pin edges whose adjacent-face-normal angle exceeds #sharp_angle. */
  bool preserve_sharp_edges = false;
  /** Sharp-edge threshold in radians (angle between adjacent face normals). */
  float sharp_angle = 0.0f;
  /** Resample generic attributes from the input onto the result (nearest-surface sampling). */
  bool transfer_attributes = true;
};

/**
 * Isotropically remesh a mesh so triangles become as uniform and equilateral as possible while
 * staying on the original surface. Non-triangle faces are triangulated internally. The input
 * #mesh is treated as read-only and is never modified.
 *
 * Topology is fully rebuilt, so only #position is guaranteed to survive. When
 * #IsotropicRemeshParams::transfer_attributes is enabled, point/face/corner-domain attributes
 * that pass #attribute_filter are resampled onto the result by nearest-surface interpolation.
 * Edge-domain attributes cannot be mapped and are always dropped.
 *
 * \param progress_fn: Optional callback invoked between iterations with a value in [0, 1].
 *   Returning false requests cancellation; the partially remeshed result is still returned.
 * \return A newly allocated, main-database-free #Mesh, or null if the mesh could not be remeshed
 *   (for example it has no faces). On null the caller should keep the original input unchanged.
 */
Mesh *isotropic_remesh(const Mesh &mesh,
                       const IsotropicRemeshParams &params,
                       const bke::AttributeFilter &attribute_filter =
                           bke::AttributeFilter::default_filter(),
                       const std::function<bool(float progress)> &progress_fn = nullptr);

}  // namespace blender::geometry
