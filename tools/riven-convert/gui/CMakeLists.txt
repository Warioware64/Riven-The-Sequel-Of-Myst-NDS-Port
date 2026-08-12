# The Qt GUI. Everything it does goes through riven_convert_core, which knows
# nothing about Qt -- so the CLI and the GUI cannot drift, and the tests stay
# fast and headless.

set(CMAKE_AUTOMOC ON)

qt_add_executable(riven-convert-gui
    main.cpp
    MainWindow.cpp
    ConversionWorker.cpp
)

target_link_libraries(riven-convert-gui PRIVATE
    riven_convert_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

# WIN32_EXECUTABLE keeps a console window from appearing behind the GUI on
# Windows. The CLI is a separate binary, so unlike the Myst converter -- which
# was one frozen console app doing both jobs -- there is no reason to keep a
# console attached here.
set_target_properties(riven-convert-gui PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

if(NOT MSVC)
    target_compile_options(riven-convert-gui PRIVATE -Wall -Wextra)
endif()

install(TARGETS riven-convert-gui
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin)

# The window is mostly layout; the worker is where the threading contract
# lives, so that is what gets a test. Needs Qt6::Test for QSignalSpy.
if(BUILD_TESTING)
    find_package(Qt6 6.5 COMPONENTS Test QUIET)
    if(Qt6Test_FOUND)
        qt_add_executable(test_gui_worker test_worker.cpp ConversionWorker.cpp)
        target_include_directories(test_gui_worker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(test_gui_worker PRIVATE
            riven_convert_core Qt6::Core Qt6::Test)
        add_test(NAME gui_worker COMMAND test_gui_worker)
    else()
        message(STATUS "Qt6::Test not found: skipping the GUI worker test")
    endif()
endif()
