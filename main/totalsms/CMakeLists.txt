idf_component_register(
  SRCS
    "single.c"
  INCLUDE_DIRS .
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
	"-DSMS_ENABLE_FORCE_INLINE=1"
	"-DSMS_LITTLE_ENDIAN=1"
)

target_compile_options(${COMPONENT_LIB} PRIVATE
	"-fjump-tables"
	"-ftree-switch-conversion"
	"-Ofast"
)

