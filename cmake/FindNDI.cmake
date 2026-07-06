# Locates the NDI SDK (libndi + Processing.NDI.Lib.h).
# Search order: $NDI_SDK_DIR, third_party/ndi (unprivileged install),
# /opt/ndi, then system paths (AUR ndi-sdk installs to /usr/lib + /usr/include).
# Exposes imported target NDI::ndi plus NDI_LIBRARY / NDI_INCLUDE_DIR.

set(_ndi_hints
  $ENV{NDI_SDK_DIR}
  ${CMAKE_SOURCE_DIR}/third_party/ndi
  "${CMAKE_SOURCE_DIR}/third_party/NDI SDK for Linux"
  /opt/ndi
)

find_path(NDI_INCLUDE_DIR Processing.NDI.Lib.h
  HINTS ${_ndi_hints}
  PATH_SUFFIXES include
)
find_library(NDI_LIBRARY
  NAMES ndi
  HINTS ${_ndi_hints}
  PATH_SUFFIXES lib/x86_64-linux-gnu lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(NDI DEFAULT_MSG NDI_LIBRARY NDI_INCLUDE_DIR)

if(NDI_FOUND AND NOT TARGET NDI::ndi)
  add_library(NDI::ndi SHARED IMPORTED)
  set_target_properties(NDI::ndi PROPERTIES
    IMPORTED_LOCATION ${NDI_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES ${NDI_INCLUDE_DIR}
  )
endif()

mark_as_advanced(NDI_INCLUDE_DIR NDI_LIBRARY)
