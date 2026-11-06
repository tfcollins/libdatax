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

  # Stream lifecycle and RX copying. emu_pattern.sh serves a temporary copy
  # of the fixture with the RX counting pattern beside it (make_pattern.py).
  add_executable(test_stream_rx test_stream_rx.c)
  target_link_libraries(test_stream_rx PRIVATE datax iio_dep)
  target_include_directories(test_stream_rx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
  target_compile_options(test_stream_rx PRIVATE ${DATAX_WARNINGS})
  target_compile_definitions(test_stream_rx PRIVATE _POSIX_C_SOURCE=200809L)
  # mode | fixture | pattern device ("-": no buffer file) | channels | offset
  set(DATAX_STREAM_RX_CASES
    "ad936x|ad936x|cf-ad9361-lpc|1|0"
    "ad936x-2ch|ad936x|cf-ad9361-lpc|2|1000"
    "adrv9009|adrv9009-zu11eg|axi-adrv9009-rx-hpc|1|0"
    "adrv9009-single|adrv9009-single|-|1|0"
    "nofile|ad936x|-|1|0")
  foreach(case ${DATAX_STREAM_RX_CASES})
    string(REPLACE "|" ";" f "${case}")
    list(GET f 0 mode)
    list(GET f 1 xml)
    list(GET f 2 pdev)
    list(GET f 3 nch)
    list(GET f 4 off)
    add_test(NAME test_stream_rx_${mode}
      COMMAND ${PROJECT_SOURCE_DIR}/tests/emu/emu_pattern.sh
              ${PROJECT_SOURCE_DIR}/tests/emu/${xml}.xml ${pdev} ${nch} ${off}
              $<TARGET_FILE:test_stream_rx>)
    set_tests_properties(test_stream_rx_${mode} PROPERTIES
      ENVIRONMENT "DATAX_LIBIIO_BUILD=${DATAX_LIBIIO_BUILD};DATAX_TEST_MODE=${mode}"
      TIMEOUT 60)
  endforeach()
  # Destroying a started stream and destroying the context under a live
  # stream must not leak: run that group under valgrind when it is on PATH.
  if(VALGRIND)
    add_test(NAME test_stream_rx_destroy_valgrind
      COMMAND ${PROJECT_SOURCE_DIR}/tests/emu/emu_pattern.sh
              ${PROJECT_SOURCE_DIR}/tests/emu/ad936x.xml cf-ad9361-lpc 1 0
              ${VALGRIND} --error-exitcode=1 --leak-check=full
              --errors-for-leak-kinds=definite $<TARGET_FILE:test_stream_rx>)
    set_tests_properties(test_stream_rx_destroy_valgrind PROPERTIES
      ENVIRONMENT "DATAX_LIBIIO_BUILD=${DATAX_LIBIIO_BUILD};DATAX_TEST_MODE=destroy"
      TIMEOUT 120)
  endif()

  # TX (one-shot, cyclic) and zero-copy in both directions. TX blocks are
  # read back from the emulator's TX file in the same temporary directory.
  # mode | fixture | pattern device | channels | offset
  set(DATAX_STREAM_IO_CASES
    "test_stream_tx|ad936x|-|1|0"
    "test_stream_zero_copy|ad936x|cf-ad9361-lpc|1|0")
  foreach(case ${DATAX_STREAM_IO_CASES})
    string(REPLACE "|" ";" f "${case}")
    list(GET f 0 t)
    list(GET f 1 xml)
    list(GET f 2 pdev)
    list(GET f 3 nch)
    list(GET f 4 off)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE datax iio_dep)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
    target_compile_options(${t} PRIVATE ${DATAX_WARNINGS})
    target_compile_definitions(${t} PRIVATE _POSIX_C_SOURCE=200809L)
    add_test(NAME ${t}
      COMMAND ${PROJECT_SOURCE_DIR}/tests/emu/emu_pattern.sh
              ${PROJECT_SOURCE_DIR}/tests/emu/${xml}.xml ${pdev} ${nch} ${off}
              $<TARGET_FILE:${t}>)
    set_tests_properties(${t} PROPERTIES
      ENVIRONMENT "DATAX_LIBIIO_BUILD=${DATAX_LIBIIO_BUILD}"
      TIMEOUT 60)
  endforeach()

  # DMA overflow/underflow status. The noattr case serves a copy of the
  # fixture, generated at configure time, without direct_reg_access on the
  # streaming devices. The test links stream_status.c directly (as
  # test_stream_map links the platform sources) to call the library-internal
  # poll and parser.
  set(DATAX_NOREG_XML ${CMAKE_CURRENT_BINARY_DIR}/ad936x-noreg.xml)
  execute_process(COMMAND python3 ${PROJECT_SOURCE_DIR}/tests/emu/strip_reg_access.py
    ${PROJECT_SOURCE_DIR}/tests/emu/ad936x.xml ${DATAX_NOREG_XML}
    cf-ad9361-lpc cf-ad9361-dds-core-lpc RESULT_VARIABLE DATAX_NOREG_RC)
  if(NOT DATAX_NOREG_RC EQUAL 0)
    message(FATAL_ERROR "cannot generate ${DATAX_NOREG_XML}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/tests/emu/ad936x.xml
    ${PROJECT_SOURCE_DIR}/tests/emu/strip_reg_access.py)
  add_executable(test_stream_status test_stream_status.c
    ${PROJECT_SOURCE_DIR}/src/stream_status.c)
  target_link_libraries(test_stream_status PRIVATE datax iio_dep Threads::Threads)
  target_include_directories(test_stream_status PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
  target_compile_options(test_stream_status PRIVATE ${DATAX_WARNINGS})
  target_compile_definitions(test_stream_status PRIVATE _POSIX_C_SOURCE=200809L)
  foreach(mode attr noattr)
    if(mode STREQUAL "attr")
      set(sx ${PROJECT_SOURCE_DIR}/tests/emu/ad936x.xml)
    else()
      set(sx ${DATAX_NOREG_XML})
    endif()
    add_test(NAME test_stream_status_${mode}
      COMMAND ${PROJECT_SOURCE_DIR}/tests/emu/emu_pattern.sh
              ${sx} cf-ad9361-lpc 1 0 $<TARGET_FILE:test_stream_status>)
    set_tests_properties(test_stream_status_${mode} PROPERTIES
      ENVIRONMENT "DATAX_LIBIIO_BUILD=${DATAX_LIBIIO_BUILD};DATAX_TEST_MODE=${mode}"
      TIMEOUT 60)
  endforeach()

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
  foreach(fx ad936x ad936x+tick adrv9009-zu11eg adrv9009-single adrv9009+tick adrv9009+tick-fsm)
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

# Fake Tick + fake Talise FHM: a test-only shared library (never installed)
# so C tests link it and Python tests can load it through ctypes.
add_library(datax_fake_tick SHARED fake_tick.c)
target_include_directories(datax_fake_tick PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/src)
target_compile_options(datax_fake_tick PRIVATE ${DATAX_WARNINGS})
target_compile_definitions(datax_fake_tick PRIVATE _POSIX_C_SOURCE=200809L)
target_link_libraries(datax_fake_tick PRIVATE Threads::Threads)

# The fake against every golden hdl_model trace (one test per trace), and
# its own surface/PPS/FHM checks. No emulator needed.
add_executable(test_fake_tick test_fake_tick.c)
target_link_libraries(test_fake_tick PRIVATE datax datax_fake_tick)
target_include_directories(test_fake_tick PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/src)
target_compile_options(test_fake_tick PRIVATE ${DATAX_WARNINGS})
target_compile_definitions(test_fake_tick PRIVATE _POSIX_C_SOURCE=200809L)
file(GLOB DATAX_TRACES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/traces/*.json)
if(NOT DATAX_TRACES)
  message(FATAL_ERROR "no golden traces in tests/traces (run tests/traces/gen_traces.py)")
endif()
foreach(tr ${DATAX_TRACES})
  get_filename_component(tn ${tr} NAME_WE)
  add_test(NAME test_fake_tick_${tn} COMMAND test_fake_tick trace ${tr})
endforeach()
add_test(NAME test_fake_tick_unit COMMAND test_fake_tick unit)
if(DATAX_LIBIIO_BUILD)
  # The seam: bind the fake to a live device of a fixture without Tick.
  foreach(fx ad936x adrv9009-zu11eg)
    add_test(NAME test_fake_tick_seam_${fx}
      COMMAND ${PROJECT_SOURCE_DIR}/tests/emu/emu.sh
              ${PROJECT_SOURCE_DIR}/tests/emu/${fx}.xml $<TARGET_FILE:test_fake_tick> seam)
    set_tests_properties(test_fake_tick_seam_${fx} PROPERTIES
      ENVIRONMENT "DATAX_LIBIIO_BUILD=${DATAX_LIBIIO_BUILD}"
      TIMEOUT 60)
  endforeach()

  # Scheduler core and poller (Task 2): the fake bound to a device of a
  # fixture without Tick. adrv9009 runs every test (GPIO bit 0 reserved);
  # ad936x checks that its family reserves no line.
  add_executable(test_sched test_sched.c)
  target_link_libraries(test_sched PRIVATE datax datax_fake_tick Threads::Threads)
  target_include_directories(test_sched PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src)
  target_compile_options(test_sched PRIVATE ${DATAX_WARNINGS})
  target_compile_definitions(test_sched PRIVATE _POSIX_C_SOURCE=200809L)
  foreach(case "adrv9009-zu11eg|all" "ad936x|ad936x")
    string(REPLACE "|" ";" f "${case}")
    list(GET f 0 xml)
    list(GET f 1 mode)
    add_test(NAME test_sched_${mode}
      COMMAND ${PROJECT_SOURCE_DIR}/tests/emu/emu.sh
              ${PROJECT_SOURCE_DIR}/tests/emu/${xml}.xml $<TARGET_FILE:test_sched> ${mode})
    set_tests_properties(test_sched_${mode} PROPERTIES
      ENVIRONMENT "DATAX_LIBIIO_BUILD=${DATAX_LIBIIO_BUILD}"
      TIMEOUT 120)
  endforeach()
  # Device time, PPS alignment and the internal-clock fallback (fake Tick).
  add_executable(test_devtime test_devtime.c)
  target_link_libraries(test_devtime PRIVATE datax datax_fake_tick Threads::Threads)
  target_include_directories(test_devtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src)
  target_compile_options(test_devtime PRIVATE ${DATAX_WARNINGS})
  target_compile_definitions(test_devtime PRIVATE _POSIX_C_SOURCE=200809L)
  add_test(NAME test_devtime
    COMMAND ${PROJECT_SOURCE_DIR}/tests/emu/emu.sh
            ${PROJECT_SOURCE_DIR}/tests/emu/adrv9009-zu11eg.xml $<TARGET_FILE:test_devtime>)
  set_tests_properties(test_devtime PROPERTIES
    ENVIRONMENT "DATAX_LIBIIO_BUILD=${DATAX_LIBIIO_BUILD}"
    TIMEOUT 120)
  # FHM timed tunes (Task 4): fake Tick + fake Talise FHM on the adrv9009 pair.
  add_executable(test_fhm test_fhm.c)
  target_link_libraries(test_fhm PRIVATE datax datax_fake_tick Threads::Threads)
  target_include_directories(test_fhm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src)
  target_compile_options(test_fhm PRIVATE ${DATAX_WARNINGS})
  target_compile_definitions(test_fhm PRIVATE _POSIX_C_SOURCE=200809L)
  add_test(NAME test_fhm
    COMMAND ${PROJECT_SOURCE_DIR}/tests/emu/emu.sh
            ${PROJECT_SOURCE_DIR}/tests/emu/adrv9009-zu11eg.xml $<TARGET_FILE:test_fhm>)
  set_tests_properties(test_fhm PROPERTIES
    ENVIRONMENT "DATAX_LIBIIO_BUILD=${DATAX_LIBIIO_BUILD}"
    TIMEOUT 120)
  # The opt-in FHM trace: off by default, one line per event with
  # DATAX_TRACE=fhm (hardware diagnosis of stage-late tunes).
  add_test(NAME test_fhm_trace
    COMMAND ${PROJECT_SOURCE_DIR}/tests/emu/emu.sh
            ${PROJECT_SOURCE_DIR}/tests/emu/adrv9009-zu11eg.xml
            ${CMAKE_CURRENT_SOURCE_DIR}/trace_fhm.sh $<TARGET_FILE:test_fhm>)
  set_tests_properties(test_fhm_trace PROPERTIES
    ENVIRONMENT "DATAX_LIBIIO_BUILD=${DATAX_LIBIIO_BUILD}"
    TIMEOUT 120)
  # The internal hook contract (src/scheduler.h) for Task 4: compiled from
  # the library sources, since the hooks and datax_sched_enqueue are not
  # exported.
  add_executable(test_sched_hooks test_sched_hooks.c ${DATAX_SOURCES})
  target_link_libraries(test_sched_hooks PRIVATE iio_dep datax_fake_tick Threads::Threads)
  target_include_directories(test_sched_hooks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/include)
  target_compile_options(test_sched_hooks PRIVATE ${DATAX_WARNINGS})
  target_compile_definitions(test_sched_hooks PRIVATE _POSIX_C_SOURCE=200809L)
  add_test(NAME test_sched_hooks
    COMMAND ${PROJECT_SOURCE_DIR}/tests/emu/emu.sh
            ${PROJECT_SOURCE_DIR}/tests/emu/adrv9009-zu11eg.xml $<TARGET_FILE:test_sched_hooks>)
  set_tests_properties(test_sched_hooks PROPERTIES
    ENVIRONMENT "DATAX_LIBIIO_BUILD=${DATAX_LIBIIO_BUILD}"
    TIMEOUT 120)
  # MCS verification and PHASE_COHERENT (Task 5): the fsm fixture, attributes
  # edited through libiio to put a phy in each failing state. Compiled from
  # the library sources: it calls the unexported flag refresh.
  add_executable(test_mcs test_mcs.c ${DATAX_SOURCES})
  target_link_libraries(test_mcs PRIVATE iio_dep Threads::Threads)
  target_include_directories(test_mcs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/include)
  target_compile_options(test_mcs PRIVATE ${DATAX_WARNINGS})
  target_compile_definitions(test_mcs PRIVATE _POSIX_C_SOURCE=200809L)
  add_test(NAME test_mcs
    COMMAND ${PROJECT_SOURCE_DIR}/tests/emu/emu.sh
            ${PROJECT_SOURCE_DIR}/tests/emu/adrv9009+tick-fsm.xml $<TARGET_FILE:test_mcs>)
  set_tests_properties(test_mcs PROPERTIES
    ENVIRONMENT "DATAX_LIBIIO_BUILD=${DATAX_LIBIIO_BUILD}"
    TIMEOUT 60)
  # Destroying the scheduler or context while the poller is mid-I/O must join
  # it before anything is freed: valgrind when it is on PATH.
  if(VALGRIND)
    add_test(NAME test_sched_destroy_valgrind
      COMMAND ${PROJECT_SOURCE_DIR}/tests/emu/emu.sh
              ${PROJECT_SOURCE_DIR}/tests/emu/adrv9009-zu11eg.xml
              ${VALGRIND} --error-exitcode=1 --leak-check=full
              --errors-for-leak-kinds=definite $<TARGET_FILE:test_sched> destroy)
    set_tests_properties(test_sched_destroy_valgrind PROPERTIES
      ENVIRONMENT "DATAX_LIBIIO_BUILD=${DATAX_LIBIIO_BUILD}"
      TIMEOUT 300)
  endif()
endif()
