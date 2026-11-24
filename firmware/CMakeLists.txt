# Toucan firmware module (the repository root is the Zephyr module, see
# zephyr/module.yml). Nanopb generation follows cormoran's
# zmk-module-template-with-custom-studio-rpc (v0.3-branch).

target_include_directories(app PRIVATE include)

if(CONFIG_TOUCAN_STUDIO)
  list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
  include(nanopb)
  set(NANOPB_GENERATE_CPP_APPEND_PATH TRUE)
  set(NANOPB_GENERATE_CPP_STANDALONE OFF)

  # Generated code goes into its own library; adding it to `app` directly
  # breaks the generation order.
  zephyr_library_named(toucan_studio_proto)
  nanopb_generate_cpp(proto_srcs proto_hdrs RELPATH ${CMAKE_CURRENT_SOURCE_DIR}/proto
                      ${CMAKE_CURRENT_SOURCE_DIR}/proto/toucan/studio.proto)
  target_include_directories(toucan_studio_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
  target_sources(toucan_studio_proto PRIVATE ${proto_srcs} ${proto_hdrs})

  target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
  add_dependencies(app toucan_studio_proto)
  target_sources(app PRIVATE src/studio_rpc.c)

  target_sources_ifdef(CONFIG_TOUCAN_STUDIO_COMBO app PRIVATE src/combo_rpc.c)
  target_sources_ifdef(CONFIG_TOUCAN_STUDIO_MACRO app PRIVATE src/macro_rpc.c)
  target_sources_ifdef(CONFIG_TOUCAN_STUDIO_POINTER app PRIVATE src/pointer_rpc.c)
endif()

target_sources_ifdef(CONFIG_TOUCAN_BEHAVIOR_RUNTIME_MACRO app PRIVATE src/behavior_runtime_macro.c)
target_sources_ifdef(CONFIG_TOUCAN_INPUT_PROCESSOR_RUNTIME_SCALER app PRIVATE src/runtime_scaler.c)
