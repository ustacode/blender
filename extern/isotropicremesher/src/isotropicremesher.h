/*
 *  Copyright (c) 2020-2021 Jeremy HU <jeremy-at-dust3d dot org>. All rights reserved. 
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:

 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.

 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 */
#ifndef ISOTROPIC_REMESHER_H
#define ISOTROPIC_REMESHER_H
#include <vector>
#include <functional> /* BLENDER MODIFICATION: progress/cancellation callback support. */
#include "vector3.h"
#include "axisalignedboundingboxtree.h"
#include "axisalignedboundingbox.h"

class IsotropicHalfedgeMesh;

class IsotropicRemesher
{
public:
    IsotropicRemesher(const std::vector<Vector3> *vertices,
            const std::vector<std::vector<size_t>> *triangles);
    ~IsotropicRemesher();
    double initialAverageEdgeLength();
    void setSharpEdgeIncludedAngle(double degrees);
    /* BLENDER MODIFICATION: set the sharp-edge threshold directly as the angle (in radians)
     * between adjacent face normals, matching Blender's shade-smooth/edge-split convention. */
    void setSharpEdgeThresholdRadians(double radians);
    void setTargetEdgeLength(double edgeLength);
    void setTargetTriangleCount(size_t triangleCount);
    /* BLENDER MODIFICATION: allow callers to disable boundary-edge preservation. When enabled
     * (the default, matching upstream behaviour) open boundaries are marked as features so they
     * are neither collapsed nor projected away. */
    void setPreserveBoundaries(bool preserveBoundaries);
    /* BLENDER MODIFICATION: the optional callback is invoked once before each iteration with the
     * current (0-based) iteration index and the total iteration count. Returning false requests
     * early cancellation; the mesh produced so far is left in a valid state. */
    void remesh(size_t iteration,
            const std::function<bool(size_t currentIteration, size_t totalIterations)> &progressCallback = nullptr);
    IsotropicHalfedgeMesh *remeshedHalfedgeMesh();
    
private:
    const std::vector<Vector3> *m_vertices = nullptr;
    const std::vector<std::vector<size_t>> *m_triangles = nullptr;
    std::vector<Vector3> *m_triangleNormals = nullptr;
    IsotropicHalfedgeMesh *m_halfedgeMesh = nullptr;
    std::vector<AxisAlignedBoudingBox> *m_triangleBoxes = nullptr;
    AxisAlignedBoudingBoxTree *m_axisAlignedBoundingBoxTree = nullptr;
    double m_sharpEdgeThresholdRadians = 0;
    bool m_preserveBoundaries = true; /* BLENDER MODIFICATION */
    double m_targetEdgeLength = 0;
    double m_initialAverageEdgeLength = 0;
    size_t m_targetTriangleCount = 0;
    
    void addTriagleToAxisAlignedBoundingBox(const std::vector<size_t> &triangle, AxisAlignedBoudingBox *box)
    {
        for (size_t i = 0; i < 3; ++i)
            box->update((*m_vertices)[triangle[i]]);
    }
    
    void splitLongEdges(double maxEdgeLength);
    void collapseShortEdges(double minEdgeLengthSquared, double maxEdgeLengthSquared);
    void flipEdges();
    void shiftVertices();
    void projectVertices();
    void buildAxisAlignedBoundingBoxTree();
};

#endif
