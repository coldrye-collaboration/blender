# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  ../include
  ../../blenfont
  ../../blenkernel
  ../../blenlib
  ../../blentranslation
  ../../compositor
  ../../depsgraph
  ../../draw
  ../../functions
  ../../gpu
  ../../imbuf
  ../../makesdna
  ../../makesrna
  ../../nodes
  ../../render
  ../../windowmanager
  ../../../../intern/glew-mx
  ../../../../intern/guardedalloc
  # RNA_prototypes.h
  ${CMAKE_BINARY_DIR}/source/blender/makesrna
)


set(SRC
  drawnode.cc
  link_drag_search.cc
  node_add.cc
  node_context_path.cc
  node_draw.cc
  node_edit.cc
  node_geometry_attribute_search.cc
  node_gizmo.cc
  node_group.cc
  node_ops.cc
  node_relationships.cc
  node_select.cc
  node_templates.cc
  node_view.cc
  space_node.cc

  node_intern.hh
)

set(LIB
  bf_blenkernel
  bf_blenlib
  bf_editor_screen
)

if(WITH_COMPOSITOR_CPU)
  add_definitions(-DWITH_COMPOSITOR_CPU)
endif()

if(WITH_OPENIMAGEDENOISE)
  add_definitions(-DWITH_OPENIMAGEDENOISE)
endif()

if(WITH_OPENSUBDIV)
  add_definitions(-DWITH_OPENSUBDIV)
endif()

if(WITH_TBB)
  add_definitions(-DWITH_TBB)
  if(WIN32)
    # TBB includes Windows.h which will define min/max macros
    # that will collide with the stl versions.
    add_definitions(-DNOMINMAX)
  endif()
  list(APPEND INC_SYS
    ${TBB_INCLUDE_DIRS}
  )

  list(APPEND LIB
    ${TBB_LIBRARIES}
  )
endif()

blender_add_lib(bf_editor_space_node "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")

# RNA_prototypes.h
add_dependencies(bf_editor_space_node bf_rna)
