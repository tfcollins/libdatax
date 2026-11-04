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

# Context lifecycle and device folding, once per fixture.
add_executable(test_context test_context.c)
target_link_libraries(test_context PRIVATE datax iio_dep)
target_include_directories(test_context PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_context PRIVATE ${DATAX_WARNINGS})
foreach(fx ad936x adrv9009-zu11eg tick)
  add_test(NAME test_context_${fx}
    COMMAND ${PROJECT_SOURCE_DIR}/tests/emu/emu.sh
            ${PROJECT_SOURCE_DIR}/tests/emu/${fx}.xml $<TARGET_FILE:test_context>)
  set_tests_properties(test_context_${fx} PROPERTIES
    ENVIRONMENT "DATAX_LIBIIO_BUILD=${DATAX_LIBIIO_BUILD};DATAX_TEST_XML=${fx}"
    TIMEOUT 60)
endforeach()

find_program(VALGRIND valgrind)
if(VALGRIND)
  add_test(NAME test_context_valgrind
    COMMAND ${PROJECT_SOURCE_DIR}/tests/emu/emu.sh
            ${PROJECT_SOURCE_DIR}/tests/emu/ad936x.xml
            ${VALGRIND} --error-exitcode=1 --leak-check=full
            --errors-for-leak-kinds=definite $<TARGET_FILE:test_context>)
  set_tests_properties(test_context_valgrind PROPERTIES
    ENVIRONMENT "DATAX_LIBIIO_BUILD=${DATAX_LIBIIO_BUILD};DATAX_TEST_XML=ad936x"
    TIMEOUT 120)
endif()

# ad936x immediate verbs against the emulator.
add_executable(test_device_ad936x test_device_ad936x.c)
target_link_libraries(test_device_ad936x PRIVATE datax iio_dep m)
target_include_directories(test_device_ad936x PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_device_ad936x PRIVATE ${DATAX_WARNINGS})
add_test(NAME test_device_ad936x
  COMMAND ${PROJECT_SOURCE_DIR}/tests/emu/emu.sh
          ${PROJECT_SOURCE_DIR}/tests/emu/ad936x.xml $<TARGET_FILE:test_device_ad936x>)
set_tests_properties(test_device_ad936x PROPERTIES
  ENVIRONMENT "DATAX_LIBIIO_BUILD=${DATAX_LIBIIO_BUILD}"
  TIMEOUT 60)
