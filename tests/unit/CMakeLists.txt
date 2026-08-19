# Standalone unit-test project for the pure JSON (de)serialization layer
# touched by docs/plans/sia-transaction-history.md.
#
# Deliberately NOT part of the main tree's CMakeLists.txt / add_subdirectory
# graph: the root CMakeLists.txt runs `include(vcpkg_prerequisites)` before
# `project()`, which bootstraps the full vcpkg dependency set (Qt Quick /
# Widgets / WebEngine / Svg / Charts, cpprestsdk, antara, ...) on every
# configure regardless of target -- there is no way to reach a "core" target
# without paying for that. This project is configured and built as its own
# tree instead (see .github/workflows/atomicdex-desktop-ci.yml), pulling
# only what the covered translation units actually need.
#
# transaction.data.cpp (needed transitively by rpc.tx.history.cpp -- its
# tx_history_answer_success::from_json calls transaction_data's from_json on
# every element of the transactions vector, so the symbol must be linked
# even though this project's own tests don't call it directly) turned out to
# reach further than the plan's working assumption expected: Qt (via
# coins.cfg.hpp), boost::multiprecision (via safe.float.hpp), and spdlog +
# fmt + entt + Howard Hinnant's date/tz (via log.prerequisites.hpp and
# to_human_date, both pulled in via global.utilities.hpp) -- for logging and
# one timestamp-format call, not anything JSON-shape-related. That's real
# and is pulled in below rather than worked around, but it does mean this
# job needs more than "a bare ubuntu-latest runner with only a C++
# toolchain and CMake" as the plan hoped: Qt5, Boost, and fmt development
# packages must be present (fast apt installs, not vcpkg source builds --
# see the workflow job for the exact package list), and spdlog/entt/date
# are fetched at configure time (small, mostly-header-only; date/tz uses
# the runner's own system tzdata rather than downloading a copy).

cmake_minimum_required(VERSION 3.20)
project(komodo_wallet_unit_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt5 5.15 COMPONENTS Core REQUIRED)
find_package(Boost REQUIRED)
find_package(fmt REQUIRED)

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)

set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
    GIT_SHALLOW TRUE
)

set(BUILD_TZ_LIB ON CACHE BOOL "" FORCE)
set(USE_SYSTEM_TZ_DB ON CACHE BOOL "" FORCE)
set(MANUAL_TZ_DB OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    date
    GIT_REPOSITORY https://github.com/HowardHinnant/date.git
    GIT_TAG v3.0.1
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(nlohmann_json spdlog date)

# Only entt/core/attribute.h (for ENTT_API) is actually needed here -- avoid
# configuring entt's own CMakeLists (packaging/install rules, its own test
# suite) for the sake of one header.
FetchContent_Populate(entt
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG v3.13.2
    GIT_SHALLOW TRUE
    SOURCE_DIR ${CMAKE_BINARY_DIR}/_deps/entt-src
)
add_library(entt_headers INTERFACE)
target_include_directories(entt_headers INTERFACE ${CMAKE_BINARY_DIR}/_deps/entt-src/src)

set(WALLET_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src/core)

add_executable(komodo_wallet_unit_tests
    main.cpp
    ${WALLET_SRC}/atomicdex/api/kdf/rpc_v2/rpc2.task.enable_sia_coin.init.cpp
    ${WALLET_SRC}/atomicdex/api/kdf/rpc.tx.history.cpp
    ${WALLET_SRC}/atomicdex/api/kdf/paging_options.cpp
    ${WALLET_SRC}/atomicdex/api/kdf/transaction.data.cpp
    ${WALLET_SRC}/atomicdex/services/kdf/kdf.coin.activation.policy.cpp
)
target_include_directories(komodo_wallet_unit_tests PRIVATE ${WALLET_SRC})
target_link_libraries(komodo_wallet_unit_tests PRIVATE
    Qt5::Core
    Boost::boost
    fmt::fmt
    nlohmann_json::nlohmann_json
    entt_headers
    spdlog::spdlog
    date::date
    date::date-tz
)
target_compile_definitions(komodo_wallet_unit_tests PRIVATE
    ENTT_API_EXPORT=
    ENTT_API=
    # Mirror the small set of DEX_* string/int literals the main tree
    # supplies as INTERFACE_DEFS on its `core` target (src/CMakeLists.txt) --
    # dex.constants.hpp needs them to exist, but their actual values are
    # irrelevant to anything under test here.
    DEX_API="komodo_wallet_unit_tests"
    DEX_RPC="127.0.0.1"
    DEX_RPCPORT="7783"
    DEX_PRIMARY_COIN="KMD"
    DEX_SECOND_PRIMARY_COIN="BTC"
)

enable_testing()
add_test(NAME komodo_wallet_unit_tests COMMAND komodo_wallet_unit_tests)
