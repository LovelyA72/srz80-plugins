srz80_plugin(w65c816 KIND card
  SOURCES
    card.cpp
    g65816/g65816.cpp
    g65816/g65816ds.cpp
    g65816/g65816o0.cpp
    g65816/g65816o1.cpp
    g65816/g65816o2.cpp
    g65816/g65816o3.cpp
    g65816/g65816o4.cpp
  OUTPUT cpu_w65c816
  INCLUDES "${CMAKE_CURRENT_LIST_DIR}/g65816")
