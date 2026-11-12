# Fork: libFuzzer harnesses for the code that reads raw radio data and J2K pictures. Clang only:
#   cmake -C cmake/fork/pi.cmake -DSATDUMP_TESTS=ON -DSATDUMP_FUZZ=ON \
#         -DSATDUMP_SANITIZERS=address,undefined -DSATDUMP_CPU_FLAGS= -DBUILD_GUI=OFF \
#         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -S . -B build-fuzz -G Ninja
#   ninja -C build-fuzz fuzz_aos_demux fuzz_msumr_lrpt fuzz_admin_msg fuzz_reedsolomon fuzz_j2k \
#         fuzz_jpeg fuzz_jpeg12
#   ctest --test-dir build-fuzz -L fuzz      # a short, repeatable run of each
#   tests/fuzz/run.sh build-fuzz/tests/fuzz/fuzz_msumr_lrpt 600   # fuzz for 10 minutes
set(_plugins ${PROJECT_SOURCE_DIR}/plugins)
set(_msumr ${_plugins}/meteor_support/meteor/instruments/msumr)

# satdump_fuzz(<name> [plugin sources...]) builds fuzz_<name> from fuzz_<name>.cpp
function(satdump_fuzz name)
    add_executable(fuzz_${name} fuzz_${name}.cpp ${ARGN})
    target_include_directories(fuzz_${name} PRIVATE
        ${PROJECT_SOURCE_DIR}/src-core
        ${_plugins}/noaa_metop_support
        ${_plugins}/meteor_support
        ${_msumr}/lrpt)
    target_link_libraries(fuzz_${name} PRIVATE satdump_core)
    target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
    # A fixed seed and run count, so ctest gives the same result every time
    add_test(NAME fuzz_${name} COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run.sh $<TARGET_FILE:fuzz_${name}> 0)
    set_tests_properties(fuzz_${name} PROPERTIES LABELS fuzz)
endfunction()

satdump_fuzz(aos_demux)
# Fork part "aos": with Rust, the harness also checks the Rust demuxer gives the C++ one's packets
if(SATDUMP_WITH_RUST)
    target_compile_definitions(fuzz_aos_demux PRIVATE SATDUMP_FUZZ_AOS_RUST=1)
endif()
satdump_fuzz(reedsolomon)
satdump_fuzz(admin_msg ${_plugins}/noaa_metop_support/metop/instruments/admin_msg/admin_msg_reader.cpp)
satdump_fuzz(msumr_lrpt ${_msumr}/lrpt_msumr_reader.cpp ${_msumr}/lrpt/segment.cpp ${_msumr}/lrpt/huffman.cpp ${_msumr}/lrpt/idct.cpp)
# Fork: J2K decoding through the system OpenJPEG (src-core/image/j2k_stream.h)
satdump_fuzz(j2k)
# Fork: 8-bit and 12-bit JPEG decoding through the bundled libjpeg copies (src-core/image/jpeg_utils.h,
# jpeg12_utils.h); the good streams they start from are made with the unit tests' encoder
satdump_fuzz(jpeg)
satdump_fuzz(jpeg12)
target_include_directories(fuzz_jpeg PRIVATE ${PROJECT_SOURCE_DIR}/tests/unit)
target_include_directories(fuzz_jpeg12 PRIVATE ${PROJECT_SOURCE_DIR}/tests/unit)
