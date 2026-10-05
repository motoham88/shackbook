cmake_minimum_required(VERSION 3.20)
project(ShackBook
    VERSION 0.8.0
    DESCRIPTION "Standalone ham radio logbook with TCI integration"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Qt 6.2+ for QSqlDatabase, QWebSocket, modern QString APIs.
# Network is required by shackbook-server (QTcpServer); a Qt::Sql transitive
# dep on most platforms but listed here for explicitness.
find_package(Qt6 6.2 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Sql
    Network
    SerialPort
    WebSockets
)

# Source layout — keep flat under src/ so includes are simple.
set(SHACKBOOK_SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/TciClient.cpp
    src/TciClient.h
    src/CwSender.h
    src/CwKeyer.cpp
    src/CwKeyer.h
    src/TxPowerTracker.cpp
    src/TxPowerTracker.h
    src/MqttPacket.cpp
    src/MqttPacket.h
    src/MqttPublisher.cpp
    src/MqttPublisher.h
    src/ShackStatus.cpp
    src/ShackStatus.h
    src/TciDiscovery.cpp
    src/TciDiscovery.h
    src/RigctldClient.cpp
    src/RigctldClient.h
    src/HamlibRigList.cpp
    src/HamlibRigList.h
    src/RigctldSupervisor.cpp
    src/RigctldSupervisor.h
    src/LogbookModel.cpp
    src/LogbookModel.h
    src/AdifReader.cpp
    src/AdifReader.h
    src/AwardsDialog.cpp
    src/AwardsDialog.h
    src/SectionMapDialog.cpp
    src/SectionMapDialog.h
    src/LotwDialog.cpp
    src/LotwDialog.h
    # ── Session Map: derived operating sessions + shared world geometry ──
    src/QsoSession.cpp
    src/QsoSession.h
    src/WorldMap.h
    src/SessionMapDialog.cpp
    src/SessionMapDialog.h
    src/GridMapDialog.cpp
    src/GridMapDialog.h
    src/server/WsjtxAdifReceiver.cpp
    src/server/WsjtxAdifReceiver.h
    src/Qso.h
    src/EditDialog.cpp
    src/EditDialog.h
    src/SettingsDialog.cpp
    src/SettingsDialog.h
    # ── Spot autofill (Phase 2: DX cluster, Phase 3: POTA) ──
    src/SpotData.h
    src/SpotIndex.cpp
    src/SpotIndex.h
    src/AetherSettingsReader.cpp
    src/AetherSettingsReader.h
    src/DxClusterClient.cpp
    src/DxClusterClient.h
    src/PotaClient.cpp
    src/PotaClient.h
    # ── Callsign lookup (worked-before / cty.dat / QRZ-HamQTH-callook) ──
    src/CtyDat.cpp
    src/CtyDat.h
    src/CallsignLookup.cpp
    src/CallsignLookup.h
    # ── County reference list (worked/needed for state QSO parties) ──
    src/AppRename.cpp
    src/AppRename.h
    src/CountyList.cpp
    src/CountyList.h
    # ── Contest definitions + worked/needed counties (QSO party mode) ──
    src/ContestDef.cpp
    src/ContestDef.h
    src/ContestCalendar.cpp
    src/ContestCalendar.h
    src/QsoPartyDialog.cpp
    src/QsoPartyDialog.h
    src/CountyProgress.cpp
    src/CountyProgress.h
    # ── APRS over AetherSDR's KISS-over-TCP TNC (activity + messaging) ──
    src/AprsDecode.cpp
    src/AprsDecode.h
    src/AprsKissClient.cpp
    src/AprsKissClient.h
    src/AprsStationModel.cpp
    src/AprsStationModel.h
    src/AprsActivityDialog.cpp
    src/AprsActivityDialog.h
    data/cty.qrc
    data/counties.qrc
    data/contests.qrc
    data/calendar.qrc
)

# Windows: link as a GUI app so no console window appears at launch.
# Adding shackbook.rc compiles in the app icon (logbook.ico) so Explorer
# / taskbar / Alt-Tab show it for ShackBook.exe.
if(WIN32)
    add_executable(ShackBook WIN32 ${SHACKBOOK_SOURCES} src/shackbook.rc)
elseif(APPLE)
    add_executable(ShackBook MACOSX_BUNDLE ${SHACKBOOK_SOURCES})
else()
    add_executable(ShackBook ${SHACKBOOK_SOURCES})
endif()

target_include_directories(ShackBook PRIVATE src)

# Version comes from project() — never hand-typed in a source file. It went
# stale from 0.1 to 0.3 that way once already.
#
# SHACKBOOK_BUILD_TAG marks a build that is NOT an installed release, so the
# window title can say so. An unmarked developer build is indistinguishable
# from the installed copy on screen, and on 2026-08-05 a feature was tested
# against a months-old installed binary for exactly that reason. Release CI
# builds pass -DSHACKBOOK_RELEASE=ON and get no tag.
option(SHACKBOOK_RELEASE "Mark this build as an official release (no dev tag in the title)" OFF)
target_compile_definitions(ShackBook PRIVATE
    SHACKBOOK_VERSION="${PROJECT_VERSION}"
)
if(NOT SHACKBOOK_RELEASE)
    target_compile_definitions(ShackBook PRIVATE SHACKBOOK_BUILD_TAG="dev")
endif()

target_link_libraries(ShackBook PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Sql
    Qt6::SerialPort
    Qt6::WebSockets
)

# Keep MSVC quiet about Qt's deprecated-but-not-fatal API surface, and
# enable common warnings.
if(MSVC)
    target_compile_options(ShackBook PRIVATE /W3 /permissive-)
    target_compile_definitions(ShackBook PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(ShackBook PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Windows: run windeployqt after every build so Qt's DLLs and the
# platforms\qwindows.dll plugin get copied next to ShackBook.exe.  This
# lets the binary launch by double-click without Qt on PATH.
#
# We locate windeployqt via the bin/ directory next to the qmake we used,
# which gives a portable path regardless of how Qt was found.
if(WIN32)
    get_target_property(_qt6_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt6_bin "${_qt6_qmake}" DIRECTORY)
    find_program(WINDEPLOYQT_EXE windeployqt HINTS "${_qt6_bin}")
    if(WINDEPLOYQT_EXE)
        add_custom_command(TARGET ShackBook POST_BUILD
            COMMAND "${WINDEPLOYQT_EXE}"
                    $<IF:$<CONFIG:Debug>,--debug,--release>
                    --no-translations
                    --no-system-d3d-compiler
                    --no-opengl-sw
                    --no-quick-import
                    --compiler-runtime
                    "$<TARGET_FILE:ShackBook>"
            COMMENT "Running windeployqt to bundle Qt runtime DLLs"
            VERBATIM
        )
    else()
        message(WARNING "windeployqt not found in ${_qt6_bin} — "
                        "ShackBook.exe will need Qt DLLs on PATH to launch")
    endif()
endif()

# On macOS the app needs a sensible bundle identifier.
if(APPLE)
    set_target_properties(ShackBook PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME           "ShackBook"
        MACOSX_BUNDLE_GUI_IDENTIFIER        "com.g0jkn.shackbook"
        MACOSX_BUNDLE_BUNDLE_VERSION        "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING  "${PROJECT_VERSION}"
    )
endif()

# ────────────────────────────────────────────────────────────────────
#  shackbook-server — headless web + N3FJP-compat server (Phase 0 spike)
# ────────────────────────────────────────────────────────────────────
# Target host is shack-hub (Linux); also built on Windows + macOS for
# dev convenience.  No GUI/Widgets — QCoreApplication only.  Reuse of
# the desktop's LogbookModel etc. lands in Phase 1; for now the spike
# is self-contained under src/server/.
set(SHACKBOOK_SERVER_SOURCES
    src/server/main.cpp
    src/server/HttpServer.cpp
    src/server/HttpServer.h
    src/server/N3fjpServer.cpp
    src/server/N3fjpServer.h
    src/server/N3fjpClient.cpp
    src/server/N3fjpClient.h
    src/server/WsjtxAdifReceiver.cpp
    src/server/WsjtxAdifReceiver.h
    # ── Domain layer reused from the desktop binary (see design doc §16) ──
    src/Qso.h
    src/LogbookModel.cpp
    src/LogbookModel.h
    src/AdifReader.cpp
    src/AdifReader.h
)

add_executable(shackbook-server ${SHACKBOOK_SERVER_SOURCES})

target_include_directories(shackbook-server PRIVATE src src/server)

target_link_libraries(shackbook-server PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Sql
)

if(MSVC)
    target_compile_options(shackbook-server PRIVATE /W3 /permissive-)
    target_compile_definitions(shackbook-server PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(shackbook-server PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Tests (opt-in: -DSHACKBOOK_TESTS=ON) ─────────────────────────────────
option(SHACKBOOK_TESTS "Build ShackBook unit tests" OFF)
if(SHACKBOOK_TESTS)
    enable_testing()

    add_executable(durability_test
        tests/durability_test.cpp
        src/LogbookModel.cpp
        src/LogbookModel.h
        src/AdifReader.cpp
        src/AdifReader.h
        src/Qso.h
    )
    target_include_directories(durability_test PRIVATE src)
    target_link_libraries(durability_test PRIVATE Qt6::Core Qt6::Sql)
    add_test(NAME durability_test COMMAND durability_test)

    add_executable(aprs_decode_test
        tests/aprs_decode_test.cpp
        src/AprsDecode.cpp
        src/AprsDecode.h
    )
    target_include_directories(aprs_decode_test PRIVATE src)
    target_link_libraries(aprs_decode_test PRIVATE Qt6::Core)
    add_test(NAME aprs_decode_test COMMAND aprs_decode_test)

    add_executable(county_list_test
        tests/county_list_test.cpp
        src/CountyList.cpp
        src/CountyList.h
    )
    target_include_directories(county_list_test PRIVATE src)
    # The test reads the source-tree data file directly, so it does not need
    # the app's .qrc linked in and fails loudly if the data file goes missing.
    target_compile_definitions(county_list_test PRIVATE
        SHACKBOOK_COUNTIES_DAT="${CMAKE_CURRENT_SOURCE_DIR}/data/counties.dat")
    target_link_libraries(county_list_test PRIVATE Qt6::Core)
    add_test(NAME county_list_test COMMAND county_list_test)

    add_executable(app_rename_test
        tests/app_rename_test.cpp
        src/AppRename.cpp
        src/AppRename.h
    )
    target_include_directories(app_rename_test PRIVATE src)
    target_link_libraries(app_rename_test PRIVATE Qt6::Core)
    add_test(NAME app_rename_test COMMAND app_rename_test)

    add_executable(tci_tune_test
        tests/tci_tune_test.cpp
        src/TciClient.cpp
        src/TciClient.h
        src/TxPowerTracker.cpp
        src/TxPowerTracker.h
    )
    target_include_directories(tci_tune_test PRIVATE src)
    target_link_libraries(tci_tune_test PRIVATE Qt6::Core Qt6::WebSockets)
    add_test(NAME tci_tune_test COMMAND tci_tune_test)

    # Measured TX power (#23): the tracker's rules, and TciClient parsing
    # tx_sensors/trx against a loopback fake TCI server.
    add_executable(tx_power_test
        tests/tx_power_test.cpp
        src/TxPowerTracker.cpp
        src/TxPowerTracker.h
        src/TciClient.cpp
        src/TciClient.h
    )
    target_include_directories(tx_power_test PRIVATE src)
    target_link_libraries(tx_power_test PRIVATE Qt6::Core Qt6::Network Qt6::WebSockets)
    add_test(NAME tx_power_test COMMAND tx_power_test)

    # CW keyer (#32): the TCI commands that key the transmitter — exact bytes,
    # refusals, nothing on connect, nothing queued across a reconnect.
    add_executable(cw_send_test
        tests/cw_send_test.cpp
        src/TciClient.cpp
        src/TciClient.h
        src/TxPowerTracker.cpp
        src/TxPowerTracker.h
    )
    target_include_directories(cw_send_test PRIVATE src)
    target_link_libraries(cw_send_test PRIVATE Qt6::Core Qt6::Network Qt6::WebSockets)
    add_test(NAME cw_send_test COMMAND cw_send_test)

    # CW keyer (#32): macro expansion, the transmit-safety gates, and the
    # send/stop state machine against a recording fake sender.
    add_executable(cw_keyer_test
        tests/cw_keyer_test.cpp
        src/CwKeyer.cpp
        src/CwKeyer.h
        src/CwSender.h
        src/TciClient.cpp
        src/TciClient.h
        src/TxPowerTracker.cpp
        src/TxPowerTracker.h
    )
    target_include_directories(cw_keyer_test PRIVATE src)
    target_link_libraries(cw_keyer_test PRIVATE Qt6::Core Qt6::Network Qt6::WebSockets)
    add_test(NAME cw_keyer_test COMMAND cw_keyer_test)

    # MQTT shack status (#24): packet bytes, HA discovery, and the publisher
    # against a loopback fake broker.
    add_executable(mqtt_test
        tests/mqtt_test.cpp
        src/MqttPacket.cpp
        src/MqttPacket.h
        src/MqttPublisher.cpp
        src/MqttPublisher.h
        src/ShackStatus.cpp
        src/ShackStatus.h
    )
    target_include_directories(mqtt_test PRIVATE src)
    target_link_libraries(mqtt_test PRIVATE Qt6::Core Qt6::Network)
    add_test(NAME mqtt_test COMMAND mqtt_test)

    add_executable(contest_county_test
        tests/contest_county_test.cpp
        src/ContestDef.cpp
        src/ContestDef.h
        src/CountyList.cpp
        src/CountyList.h
        src/CountyProgress.cpp
        src/CountyProgress.h
    )
    target_include_directories(contest_county_test PRIVATE src)
    target_compile_definitions(contest_county_test PRIVATE
        SHACKBOOK_COUNTIES_DAT="${CMAKE_CURRENT_SOURCE_DIR}/data/counties.dat"
        SHACKBOOK_CONTESTS_DAT="${CMAKE_CURRENT_SOURCE_DIR}/data/contests.dat")
    target_link_libraries(contest_county_test PRIVATE Qt6::Core)
    add_test(NAME contest_county_test COMMAND contest_county_test)

    add_executable(contest_calendar_test
        tests/contest_calendar_test.cpp
        src/ContestCalendar.cpp
        src/ContestCalendar.h
    )
    target_include_directories(contest_calendar_test PRIVATE src)
    target_compile_definitions(contest_calendar_test PRIVATE
        SHACKBOOK_CALENDAR_DAT="${CMAKE_CURRENT_SOURCE_DIR}/data/calendar.dat")
    target_link_libraries(contest_calendar_test PRIVATE Qt6::Core)
    add_test(NAME contest_calendar_test COMMAND contest_calendar_test)
    add_executable(qso_session_test
        tests/qso_session_test.cpp
        src/QsoSession.cpp
        src/QsoSession.h
    )
    target_include_directories(qso_session_test PRIVATE src)
    target_link_libraries(qso_session_test PRIVATE Qt6::Core)
    add_test(NAME qso_session_test COMMAND qso_session_test)

    add_executable(tci_discovery_test
        tests/tci_discovery_test.cpp
        src/TciDiscovery.cpp
        src/TciDiscovery.h
        src/TciClient.cpp
        src/TciClient.h
        src/TxPowerTracker.cpp
        src/TxPowerTracker.h
    )
    target_include_directories(tci_discovery_test PRIVATE src)
    target_link_libraries(tci_discovery_test PRIVATE Qt6::Core Qt6::Network Qt6::WebSockets)
    add_test(NAME tci_discovery_test COMMAND tci_discovery_test)

    # Radio mode -> ADIF mapping. Fed by BOTH the TCI and rigctld paths,
    # which speak different vocabularies -- an unmatched string yields an
    # empty mode and a QSO logged without one. See issue #16.
    # rigctld lifecycle: adopt a running server, never spawn over one, and
    # never leave behind one we started. See issue #13.
    add_executable(rigctld_supervisor_test
        tests/rigctld_supervisor_test.cpp
        src/RigctldSupervisor.cpp
        src/RigctldSupervisor.h
    )
    target_include_directories(rigctld_supervisor_test PRIVATE src)
    target_link_libraries(rigctld_supervisor_test PRIVATE Qt6::Core Qt6::Network)
    add_test(NAME rigctld_supervisor_test COMMAND rigctld_supervisor_test)

    add_executable(adif_mode_map_test
        tests/adif_mode_map_test.cpp
        src/LogbookModel.cpp
        src/LogbookModel.h
        src/AdifReader.cpp
        src/AdifReader.h
    )
    target_include_directories(adif_mode_map_test PRIVATE src)
    target_link_libraries(adif_mode_map_test PRIVATE
        Qt6::Core Qt6::Sql Qt6::Network)
    add_test(NAME adif_mode_map_test COMMAND adif_mode_map_test)

    add_executable(hamlib_riglist_test
        tests/hamlib_riglist_test.cpp
        src/HamlibRigList.cpp
        src/HamlibRigList.h
        src/RigctldClient.cpp
        src/RigctldClient.h
    )
    target_include_directories(hamlib_riglist_test PRIVATE src)
    target_link_libraries(hamlib_riglist_test PRIVATE Qt6::Core Qt6::Network)
    add_test(NAME hamlib_riglist_test COMMAND hamlib_riglist_test)

    add_executable(hamlib_detect_test
        tests/hamlib_detect_test.cpp
        src/RigctldClient.cpp
        src/RigctldClient.h
    )
    target_include_directories(hamlib_detect_test PRIVATE src)
    target_link_libraries(hamlib_detect_test PRIVATE Qt6::Core Qt6::Network)
    add_test(NAME hamlib_detect_test COMMAND hamlib_detect_test)

    add_executable(rigctld_client_test
        tests/rigctld_client_test.cpp
        src/RigctldClient.cpp
        src/RigctldClient.h
    )
    target_include_directories(rigctld_client_test PRIVATE src)
    target_link_libraries(rigctld_client_test PRIVATE Qt6::Core Qt6::Network)
    add_test(NAME rigctld_client_test COMMAND rigctld_client_test)

    # Bench tool, not a test: points the real client at an EXTERNAL rigctld
    # (the Python personas, or a real radio). Deliberately not add_test'd —
    # it needs something running to talk to.
    add_executable(rigctld_live_probe
        tests/rigctld_live_probe.cpp
        src/RigctldClient.cpp
        src/RigctldClient.h
    )
    target_include_directories(rigctld_live_probe PRIVATE src)
    target_link_libraries(rigctld_live_probe PRIVATE Qt6::Core Qt6::Network)

    # Bench tool, not a test: times the CW keyer's TCI commands (#32) against
    # a REAL TCI server. Read-only unless given --transmit. Not add_test'd.
    add_executable(tci_cw_live_probe
        tests/tci_cw_live_probe.cpp
        src/TciClient.cpp
        src/TciClient.h
        src/TxPowerTracker.cpp
        src/TxPowerTracker.h
    )
    target_include_directories(tci_cw_live_probe PRIVATE src)
    target_link_libraries(tci_cw_live_probe PRIVATE Qt6::Core Qt6::Network Qt6::WebSockets)

    add_executable(radio_attribution_test
        tests/radio_attribution_test.cpp
        src/LogbookModel.cpp
        src/LogbookModel.h
        src/AdifReader.cpp
        src/AdifReader.h
        src/Qso.h
    )
    target_include_directories(radio_attribution_test PRIVATE src)
    target_link_libraries(radio_attribution_test PRIVATE Qt6::Core Qt6::Sql)
    add_test(NAME radio_attribution_test COMMAND radio_attribution_test)
endif()
