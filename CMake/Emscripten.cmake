# WebAssembly build support, included from the top-level CMakeLists.txt when building with emcmake.
#
# zlib, libpng, libjpeg and SDL2 come from Emscripten's ports. libopenmpt (needed for the MO3 music)
# has no port, so it is built from its release tarball with its own Makefile, like the Switch CI does.

include(ExternalProject)

set(LIBOPENMPT_VERSION 0.8.9)
set(LIBOPENMPT_SHA256 9273b88b67973cc69e54d748ab1b749399d6d07695f1c37d0c59f88b4106074f)
set(LIBOPENMPT_PREFIX ${CMAKE_BINARY_DIR}/libopenmpt)

ExternalProject_Add(libopenmpt_wasm
	URL https://lib.openmpt.org/files/libopenmpt/src/libopenmpt-${LIBOPENMPT_VERSION}+release.makefile.tar.gz
	URL_HASH SHA256=${LIBOPENMPT_SHA256}
	DOWNLOAD_EXTRACT_TIMESTAMP ON
	PREFIX ${LIBOPENMPT_PREFIX}
	BUILD_IN_SOURCE ON
	CONFIGURE_COMMAND ""
	# Uses libopenmpt's bundled miniz, minimp3 and stb_vorbis, so it needs nothing else
	BUILD_COMMAND make
		CONFIG=emscripten
		CC=${CMAKE_C_COMPILER} CXX=${CMAKE_CXX_COMPILER} AR=${CMAKE_AR}
		SHARED_LIB=0 STATIC_LIB=1 DYNLINK=0
		EXAMPLES=0 OPENMPT123=0 TEST=0
		PREFIX=${LIBOPENMPT_PREFIX}
		install
	INSTALL_COMMAND ""
	BUILD_BYPRODUCTS ${LIBOPENMPT_PREFIX}/lib/libopenmpt.a
)

# Pre-seed SDL-Mixer-X's FindOpenMPT so it uses this build instead of searching the sysroot
file(MAKE_DIRECTORY ${LIBOPENMPT_PREFIX}/include/libopenmpt)
set(OpenMPT_INCLUDE_DIR ${LIBOPENMPT_PREFIX}/include CACHE PATH "" FORCE)
set(OpenMPT_LIBRARY ${LIBOPENMPT_PREFIX}/lib/libopenmpt.a CACHE FILEPATH "" FORCE)

set(EMSCRIPTEN_PORT_FLAGS
	-sUSE_ZLIB=1
	-sUSE_LIBPNG=1
	-sUSE_LIBJPEG=1
	-sUSE_SDL=2
)

# Directory holding main.pak and the properties folder, bundled into a .data file next to the .wasm.
# Leave it empty to have the page ask the player for their game files instead.
set(PVZ_WEB_DATA_DIR "" CACHE PATH "Folder with the PvZ game data (main.pak, properties/) to bundle into the web build")

function(pvz_configure_web_target target)
	add_dependencies(SDL2_mixer_ext_Static libopenmpt_wasm)

	target_compile_options(${target} PRIVATE ${EMSCRIPTEN_PORT_FLAGS} -fexceptions)
	target_link_options(${target} PRIVATE
		${EMSCRIPTEN_PORT_FLAGS}
		-fexceptions

		# The game runs a blocking main loop (and nested ones for modal dialogs); Asyncify lets it
		# suspend so the browser can render and deliver events
		-sASYNCIFY=1
		-sASYNCIFY_STACK_SIZE=1048576

		-sALLOW_MEMORY_GROWTH=1
		-sINITIAL_MEMORY=268435456
		-sSTACK_SIZE=4194304
		-sMAX_WEBGL_VERSION=2

		-sFORCE_FILESYSTEM=1
		-lidbfs.js
		-lwebsocket.js
		-sENVIRONMENT=web
		-sEXIT_RUNTIME=0
		-sEXPORTED_RUNTIME_METHODS=FS,callMain
		-sINVOKE_RUN=0

		--pre-js ${CMAKE_CURRENT_SOURCE_DIR}/web/pre.js
		--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html
	)
	set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
		${CMAKE_CURRENT_SOURCE_DIR}/web/pre.js
		${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html
	)

	if (PVZ_WEB_DATA_DIR)
		target_link_options(${target} PRIVATE "SHELL:--preload-file \"${PVZ_WEB_DATA_DIR}@/pvz\"")
	endif()

	set_target_properties(${target} PROPERTIES SUFFIX ".html")
endfunction()
