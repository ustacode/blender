# SPDX-FileCopyrightText: 2020-2021 Jeremy HU
# SPDX-FileCopyrightText: 2024 Blender Authors (build integration only)
#
# SPDX-License-Identifier: MIT
#
# Isotropic remesher by Jeremy HU (https://github.com/huxingyi/isotropicremesher),
# vendored under the MIT license. Only the CMake glue and the small, clearly marked
# "BLENDER MODIFICATION" hooks in src/isotropicremesher.{h,cpp} are Blender additions.

set(INC
  PUBLIC src
)

set(INC_SYS
)

set(SRC
  src/axisalignedboundingboxtree.cpp
  src/isotropichalfedgemesh.cpp
  src/isotropicremesher.cpp

  src/axisalignedboundingbox.h
  src/axisalignedboundingboxtree.h
  src/double.h
  src/isotropichalfedgemesh.h
  src/isotropicremesher.h
  src/vector2.h
  src/vector3.h
)

set(LIB
)

# The upstream code relies on M_PI, which is not defined by <cmath> on MSVC without this.
if(WIN32)
  add_definitions(-D_USE_MATH_DEFINES)
endif()

blender_add_lib(extern_isotropicremesher "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
add_library(bf::extern::isotropicremesher ALIAS extern_isotropicremesher)
