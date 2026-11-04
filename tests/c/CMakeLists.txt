foreach(t test_version test_stubs test_stub_errors)
  add_executable(${t} ${t}.c)
  target_link_libraries(${t} PRIVATE datax)
  target_include_directories(${t} PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_compile_options(${t} PRIVATE ${DATAX_WARNINGS})
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# Emulator-backed smoke tests: emu.sh starts iiod-emu on a free port, exports
# DATAX_TEST_URI, runs the command and always kills the emulator.
add_executable(test_emu_smoke test_emu_smoke.c)
target_link_libraries(test_emu_smoke PRIVATE iio_dep)
target_include_directories(test_emu_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_emu_smoke PRIVATE ${DATAX_WARNINGS})
foreach(fx ad936x adrv9009-zu11eg tick)
  add_test(NAME emu_smoke_${fx}
    COMMAND ${PROJECT_SOURCE_DIR}/tests/emu/emu.sh
            ${PROJECT_SOURCE_DIR}/tests/emu/${fx}.xml $<TARGET_FILE:test_emu_smoke>)
  set_tests_properties(emu_smoke_${fx} PROPERTIES
    ENVIRONMENT "DATAX_LIBIIO_BUILD=${DATAX_LIBIIO_BUILD}"
    TIMEOUT 60)
endforeach()
