# Links a target against the prebuilt PDFium in vendor/pdfium/<platform> (scripts/fetch_pdfium.sh downloads it)
# and puts the library next to the executable.

if(WIN32)
	set(WB_PDFIUM_PLATFORM win64)
else()
	set(WB_PDFIUM_PLATFORM linux64)
endif()
set(WB_PDFIUM_ROOT "${PROJECT_SOURCE_DIR}/vendor/pdfium/${WB_PDFIUM_PLATFORM}" CACHE PATH "Prebuilt PDFium (include/, lib/, bin/)")

function(wb_link_pdfium p_target)
	if(NOT EXISTS "${WB_PDFIUM_ROOT}/include/fpdfview.h")
		message(FATAL_ERROR "PDFium not found in ${WB_PDFIUM_ROOT}. Run scripts/fetch_pdfium.sh first.")
	endif()
	target_include_directories(${p_target} PRIVATE "${WB_PDFIUM_ROOT}/include")
	target_link_directories(${p_target} PRIVATE "${WB_PDFIUM_ROOT}/lib")
	if(WIN32)
		target_link_libraries(${p_target} PRIVATE "${WB_PDFIUM_ROOT}/lib/pdfium.dll.lib")
		add_custom_command(TARGET ${p_target} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WB_PDFIUM_ROOT}/bin/pdfium.dll" "$<TARGET_FILE_DIR:${p_target}>")
	else()
		target_link_libraries(${p_target} PRIVATE pdfium)
		set_target_properties(${p_target} PROPERTIES BUILD_RPATH "\$ORIGIN" INSTALL_RPATH "\$ORIGIN")
		add_custom_command(TARGET ${p_target} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WB_PDFIUM_ROOT}/lib/libpdfium.so" "$<TARGET_FILE_DIR:${p_target}>")
	endif()
endfunction()
