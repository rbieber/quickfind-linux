cmake_minimum_required(VERSION 3.20)
project(QuickFind VERSION 8.0.0 LANGUAGES C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
   message(FATAL_ERROR "This CMake build targets Linux; see Makefile.dos for DOS")
endif()

option(QF_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)

add_executable(qf QFLINUX.C QFUTIL.C QFARCH.C QFLEGACY.C QFTAR.C)
# Preserve the historical uppercase filenames while compiling them as C.
set_source_files_properties(QFLINUX.C QFUTIL.C QFARCH.C QFLEGACY.C QFTAR.C
                           PROPERTIES LANGUAGE C)
set_target_properties(qf PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_compile_definitions(qf PRIVATE _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700 _FILE_OFFSET_BITS=64)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
   target_compile_options(qf PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
   if(QF_ENABLE_SANITIZERS)
      target_compile_options(qf PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(qf PRIVATE -fsanitize=address,undefined)
   endif()
elseif(QF_ENABLE_SANITIZERS)
   message(FATAL_ERROR "Sanitizer checks require GCC or Clang")
endif()

include(CTest)
if(BUILD_TESTING)
   find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
   add_test(NAME quickfind-regression
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_quickfind.py"
                    "$<TARGET_FILE:qf>")
   set_tests_properties(quickfind-regression PROPERTIES TIMEOUT 120)
endif()

include(GNUInstallDirs)
install(TARGETS qf RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
