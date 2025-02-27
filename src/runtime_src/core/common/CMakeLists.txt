# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2019-2021 Xilinx, Inc. All rights reserved.
#
include_directories(
  ${CMAKE_BINARY_DIR} # includes version.h
  )

if (NOT WIN32)
include_directories(${DRM_INCLUDE_DIRS})

pkg_check_modules(LIBDW REQUIRED libdw)
INCLUDE_DIRECTORIES(${LIBDW_INCLUDEDIR})

pkg_check_modules(LIBELF REQUIRED libelf)
INCLUDE_DIRECTORIES(${LIBELF_INCLUDEDIR})

pkg_check_modules(LIBFFI REQUIRED libffi)
INCLUDE_DIRECTORIES(${LIBFFI_INCLUDEDIR})

endif (NOT WIN32)

file(GLOB XRT_CORE_COMMON_SRC_SHARED
  "config_reader.cpp"
  "debug.cpp"
  "debug_ip.cpp"
  "device.cpp"
  "error.cpp"
  "info_*.cpp"
  "message.cpp"
  "module_loader.cpp"
  "query_requests.cpp"
  "sensor.cpp"
  "system.cpp"
  "thread.cpp"
  "time.cpp"
  "utils.cpp"
  "xclbin_parser.cpp"
  "xclbin_swemu.cpp"
  "api/*.cpp"
  )

if (DEFINED XRT_AIE_BUILD)
  message("-- enabling api aie build")
  list(APPEND XRT_CORE_COMMON_SRC_SHARED "api/aie/xrt_graph.cpp")
endif()

# The static library files may not contain dependencies on dwarf/elf/ffi
# so can't include ps kernel sources
set(XRT_CORE_COMMON_SRC_STATIC ${XRT_CORE_COMMON_SRC_SHARED})

if (DEFINED XRT_PSKERNEL_BUILD)
  message("-- enabling api ps build")
  list(APPEND XRT_CORE_COMMON_SRC_SHARED "pskernel_parse.cpp")
  list(APPEND XRT_CORE_COMMON_SRC_SHARED "api/pskernel/xrt_pskernel.cpp")
endif()

# Files to include in object list
file(GLOB XRT_CORE_COMMON_SRC_OBJ
  "scheduler.cpp"
  )

add_compile_options("-DXRT_VERSION_MAJOR=\"${XRT_VERSION_MAJOR}\"")

add_library(xrt_coreutil SHARED ${XRT_CORE_COMMON_SRC_SHARED})
add_library(xrt_coreutil_static STATIC ${XRT_CORE_COMMON_SRC_STATIC})

set_target_properties(xrt_coreutil PROPERTIES
  VERSION ${XRT_VERSION_STRING}
  SOVERSION ${XRT_SOVERSION})

# Private dependencies for fully resolved dynamic xrt_coreutil
target_link_libraries(xrt_coreutil
  PRIVATE
  ${Boost_FILESYSTEM_LIBRARY}
  ${Boost_SYSTEM_LIBRARY})

# Targets linking with xrt_coreutil_static must also link with boost
# libraries used by coreutil.  These type of link dependencies are
# known as INTERFACE dependencies.  Here the libraries are specified
# by their system name so that static of target can pick static link
# libraries of boost
target_link_libraries(xrt_coreutil_static
  INTERFACE
  boost_filesystem
  boost_system)

if (NOT WIN32)
  # Additional link dependencies for xrt_coreutil
  # xrt_uuid.h depends on uuid
  target_link_libraries(xrt_coreutil PRIVATE pthread dl PUBLIC uuid)

  # Targets of xrt_coreutil_static must link with these additional
  # system libraries
  target_link_libraries(xrt_coreutil_static INTERFACE uuid dl rt pthread)

  if (DEFINED XRT_PSKERNEL_BUILD)
    target_link_libraries(xrt_coreutil PRIVATE ${LIBDW_LDFLAGS} ${LIBELF_LDFLAGS} ${LIBFFI_LDFLAGS})
  endif()
  
endif()

install(TARGETS xrt_coreutil
  EXPORT xrt-targets
  LIBRARY DESTINATION ${XRT_INSTALL_LIB_DIR} ${XRT_NAMELINK_SKIP}
  RUNTIME DESTINATION ${XRT_INSTALL_BIN_DIR}
)

install(TARGETS xrt_coreutil xrt_coreutil_static
  EXPORT xrt-dev-targets
  ARCHIVE DESTINATION ${XRT_INSTALL_LIB_DIR} COMPONENT ${XRT_DEV_COMPONENT}
  LIBRARY DESTINATION ${XRT_INSTALL_LIB_DIR} COMPONENT ${XRT_DEV_COMPONENT} ${XRT_NAMELINK_ONLY}
)

# The scheduler object files are for auto config of scheduler. These
# files reference xrt_core symbols, hence are excluded from
# xrt_corecommon shared library and instead linked explicitly into
# client (core) libraries
add_library(core_common_objects OBJECT ${XRT_CORE_COMMON_SRC_OBJ})
