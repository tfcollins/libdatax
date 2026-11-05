foreach(t test_version test_stubs test_stub_errors)
  add_executable(${t} ${t}.c)
  target_link_libraries(${t} PRIVATE datax)
  target_include_directories(${t} PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_compile_options(${t} PRIVATE ${DATAX_WARNINGS})
  add_test(NAME ${t} COMMAND ${t})
endforeach()

if(DATAX_LIBIIO_BUILD)
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

  # Streaming device map: the family ops are not exported, so the test links
  # the platform sources directly; the fold lookup runs against the library.
  add_executable(test_stream_map test_stream_map.c
    ${PROJECT_SOURCE_DIR}/src/platform/family.c
    ${PROJECT_SOURCE_DIR}/src/platform/ad936x.c
    ${PROJECT_SOURCE_DIR}/src/platform/adrv9009.c
    ${PROJECT_SOURCE_DIR}/src/error.c)
  target_link_libraries(test_stream_map PRIVATE datax iio_dep Threads::Threads)
  target_include_directories(test_stream_map PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
  target_compile_options(test_stream_map PRIVATE ${DATAX_WARNINGS})
  target_compile_definitions(test_stream_map PRIVATE _POSIX_C_SOURCE=200809L)
  foreach(fx ad936x adrv9009-zu11eg adrv9009-single)
    add_test(NAME test_stream_map_${fx}
      COMMAND ${PROJECT_SOURCE_DIR}/tests/emu/emu.sh
              ${PROJECT_SOURCE_DIR}/tests/emu/${fx}.xml $<TARGET_FILE:test_stream_map>)
    set_tests_properties(test_stream_map_${fx} PROPERTIES
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

  # adrv9009 pair (4R4T) and single-phy (2R2T) immediate verbs.
  add_executable(test_device_adrv9009 test_device_adrv9009.c)
  target_link_libraries(test_device_adrv9009 PRIVATE datax iio_dep m)
  target_include_directories(test_device_adrv9009 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(test_device_adrv9009 PRIVATE ${DATAX_WARNINGS})
  foreach(fx pair single)
    if(fx STREQUAL "pair")
      set(xml adrv9009-zu11eg)
    else()
      set(xml adrv9009-single)
    endif()
    add_test(NAME test_device_adrv9009_${fx}
      COMMAND ${PROJECT_SOURCE_DIR}/tests/emu/emu.sh
              ${PROJECT_SOURCE_DIR}/tests/emu/${xml}.xml $<TARGET_FILE:test_device_adrv9009>)
    set_tests_properties(test_device_adrv9009_${fx} PROPERTIES
      ENVIRONMENT "DATAX_LIBIIO_BUILD=${DATAX_LIBIIO_BUILD};DATAX_TEST_MODE=${fx}"
      TIMEOUT 60)
  endforeach()

  # Capability probe and Tick time, per fixture (with and without the Tick device).
  foreach(t test_caps test_time)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE datax)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
    target_compile_options(${t} PRIVATE ${DATAX_WARNINGS})
  endforeach()
  foreach(fx ad936x ad936x+tick adrv9009-zu11eg adrv9009-single adrv9009+tick)
    add_test(NAME test_caps_${fx}
      COMMAND ${PROJECT_SOURCE_DIR}/tests/emu/emu.sh
              ${PROJECT_SOURCE_DIR}/tests/emu/${fx}.xml $<TARGET_FILE:test_caps>)
    set_tests_properties(test_caps_${fx} PROPERTIES
      ENVIRONMENT "DATAX_LIBIIO_BUILD=${DATAX_LIBIIO_BUILD};DATAX_TEST_XML=${fx}"
      TIMEOUT 60)
  endforeach()
  add_test(NAME test_caps_bare_context
    COMMAND ${PROJECT_SOURCE_DIR}/tests/emu/emu.sh
            ${PROJECT_SOURCE_DIR}/tests/emu/tick.xml $<TARGET_FILE:test_caps>)
  set_tests_properties(test_caps_bare_context PROPERTIES
    ENVIRONMENT "DATAX_LIBIIO_BUILD=${DATAX_LIBIIO_BUILD};DATAX_TEST_XML=tick"
    TIMEOUT 60)
  foreach(fx ad936x ad936x+tick adrv9009+tick)
    add_test(NAME test_time_${fx}
      COMMAND ${PROJECT_SOURCE_DIR}/tests/emu/emu.sh
              ${PROJECT_SOURCE_DIR}/tests/emu/${fx}.xml $<TARGET_FILE:test_time>)
    set_tests_properties(test_time_${fx} PROPERTIES
      ENVIRONMENT "DATAX_LIBIIO_BUILD=${DATAX_LIBIIO_BUILD};DATAX_TEST_XML=${fx}"
      TIMEOUT 60)
  endforeach()
else()
  message(STATUS "DATAX_LIBIIO_BUILD not set: emulator-backed tests are not registered")
endif()
