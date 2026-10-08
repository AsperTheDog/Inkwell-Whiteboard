# Compiles every app/shaders/*.slang to SPIR-V at build time with slangc. Each file holds its entry points
# (`vsMain`/`fsMain`, ...), all emitted into one module: <exe dir>/shaders/<name>.spv.
# Shared Slang modules live in shaders/lib/ (imported with `import name;`) and are not compiled on their own.

find_program(WB_SLANGC
	NAMES slangc
	HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" "$ENV{HOME}/toolchains/slang/bin"
	DOC "Path to the slangc shader compiler")

function(wb_compile_shaders p_target p_shaderDir)
	file(GLOB l_shaders CONFIGURE_DEPENDS "${p_shaderDir}/*.slang")
	file(GLOB_RECURSE l_includes CONFIGURE_DEPENDS "${p_shaderDir}/lib/*.slang")
	set_source_files_properties(${l_shaders} ${l_includes} PROPERTIES HEADER_FILE_ONLY TRUE)
	target_sources(${p_target} PRIVATE ${l_shaders} ${l_includes})
	source_group("shaders" FILES ${l_shaders} ${l_includes})

	if(NOT l_shaders)
		return()
	endif()
	if(NOT WB_SLANGC)
		message(FATAL_ERROR "slangc not found. Install the Vulkan SDK (VULKAN_SDK env var) or set -DWB_SLANGC=<path>.")
	endif()

	set(l_outputs "")
	foreach(l_shader IN LISTS l_shaders)
		get_filename_component(l_name "${l_shader}" NAME_WE)
		set(l_out "$<TARGET_FILE_DIR:${p_target}>/shaders/${l_name}.spv")
		set(l_debugFlags "$<$<CONFIG:Debug>:-g2>$<$<NOT:$<CONFIG:Debug>>:-O2>")
		add_custom_command(
			OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/$<CONFIG>/${l_name}.stamp"
			COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${p_target}>/shaders"
			COMMAND "${WB_SLANGC}" "${l_shader}" -target spirv -profile spirv_1_6 -fvk-use-entrypoint-name
				-matrix-layout-column-major -I "${p_shaderDir}/lib" ${l_debugFlags} -o "${l_out}"
			COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders/$<CONFIG>"
			COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/shaders/$<CONFIG>/${l_name}.stamp"
			DEPENDS "${l_shader}" ${l_includes}
			COMMENT "slangc ${l_name}.slang"
			VERBATIM COMMAND_EXPAND_LISTS)
		list(APPEND l_outputs "${CMAKE_CURRENT_BINARY_DIR}/shaders/$<CONFIG>/${l_name}.stamp")
	endforeach()

	add_custom_target(${p_target}_shaders DEPENDS ${l_outputs})
	add_dependencies(${p_target} ${p_target}_shaders)
endfunction()
