# Find libinput
#
# LIBINPUT_INCLUDE_DIR
# LIBINPUT_LIBRARIES
# LIBINPUT_FOUND

FIND_PATH(LIBINPUT_INCLUDE_DIR libinput.h)

FIND_LIBRARY(LIBINPUT_LIBRARIES NAMES input)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LIBINPUT DEFAULT_MSG LIBINPUT_INCLUDE_DIR LIBINPUT_LIBRARIES)

mark_as_advanced(LIBINPUT_INCLUDE_DIR LIBINPUT_LIBRARIES)
