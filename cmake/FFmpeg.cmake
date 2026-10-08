# Links a target against the prebuilt shared FFmpeg in vendor/ffmpeg/<platform> (scripts/fetch_ffmpeg.sh downloads it)
# and puts the libraries next to the executable.

if(WIN32)
	set(WB_FFMPEG_PLATFORM win64)
else()
	set(WB_FFMPEG_PLATFORM linux64)
endif()
set(WB_FFMPEG_ROOT "${PROJECT_SOURCE_DIR}/vendor/ffmpeg/${WB_FFMPEG_PLATFORM}" CACHE PATH "Prebuilt FFmpeg (include/, lib/, bin/)")

function(wb_link_ffmpeg p_target)
	if(NOT EXISTS "${WB_FFMPEG_ROOT}/include/libavcodec/avcodec.h")
		message(FATAL_ERROR "FFmpeg not found in ${WB_FFMPEG_ROOT}. Run scripts/fetch_ffmpeg.sh first.")
	endif()
	target_include_directories(${p_target} PRIVATE "${WB_FFMPEG_ROOT}/include")
	target_link_directories(${p_target} PRIVATE "${WB_FFMPEG_ROOT}/lib")
	if(WIN32)
		target_link_libraries(${p_target} PRIVATE avformat avcodec avutil swscale swresample)
		file(GLOB l_dlls "${WB_FFMPEG_ROOT}/bin/*.dll")
		add_custom_command(TARGET ${p_target} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different ${l_dlls} "$<TARGET_FILE_DIR:${p_target}>")
	else()
		target_link_libraries(${p_target} PRIVATE avformat avcodec avutil swscale swresample)
		set_target_properties(${p_target} PROPERTIES BUILD_RPATH "\$ORIGIN" INSTALL_RPATH "\$ORIGIN")
		file(GLOB l_libs "${WB_FFMPEG_ROOT}/lib/*.so*")
		add_custom_command(TARGET ${p_target} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different ${l_libs} "$<TARGET_FILE_DIR:${p_target}>")
	endif()
endfunction()
