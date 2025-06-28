cmake_minimum_required(VERSION 3.18)

project(isofuzz VERSION 1.0.0)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Library Target ---
# Defines the 'isofuzz' library and lists its source files.
add_library(isofuzz STATIC
        src/isofuzz.cpp
        src/scheduler.cpp
        src/logger.cpp
)

# --- Include Directories ---
target_include_directories(isofuzz
        PUBLIC
        # For consumers of this library within the same build tree
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        # For consumers of this library after it has been installed
        $<INSTALL_INTERFACE:include>
        PRIVATE
        # For use only when compiling this library itself
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# --- Dependencies ---
# We need the pthreads library for std::thread, std::mutex, etc.
find_package(Threads REQUIRED)
target_link_libraries(isofuzz PRIVATE Threads::Threads)

# --- Compiler Flags ---
# Add some good practice compiler warnings and the pthread flag.
target_compile_options(isofuzz PRIVATE -Wall -Wextra -Wpedantic)
target_compile_options(isofuzz PRIVATE -pthread)

# --- Installation ---
# Include helpers for packaging
include(CMakePackageConfigHelpers)
include(GNUInstallDirs) # Provides standard directory variables like CMAKE_INSTALL_LIBDIR

# Step 1: Generate the isofuzzConfigVersion.cmake file
write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/isofuzzConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY AnyNewerVersion
)

# Step 2: Configure the main isofuzzConfig.cmake file from our template
configure_package_config_file(
        "isofuzzConfig.cmake.in" # The template file we created
        "${CMAKE_CURRENT_BINARY_DIR}/isofuzzConfig.cmake" # The output file
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/isofuzz
)

# Step 3: Define what to export (our 'isofuzz' library target)
install(TARGETS isofuzz
        EXPORT isofuzz-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Step 4: Install the public header files
install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Step 5: Install the generated CMake files
install(EXPORT isofuzz-targets
        FILE isofuzz-targets.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/isofuzz
)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/isofuzzConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/isofuzzConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/isofuzz
)


# Print a message indicating where the build output will be.
message(STATUS "IsoFuzz library will be built as 'libisofuzz.a'")