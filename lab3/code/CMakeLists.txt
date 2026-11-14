cmake_minimum_required(VERSION 3.24)
project(SharedLedger VERSION 0.9.4 LANGUAGES CXX)

# The bootstrap detects the real localized compiler prefix rather than relying on
# CMake's locale-sensitive autodetection on Chinese Windows installations.
if(MSVC AND DEFINED LEDGER_MSVC_INCLUDE_PREFIX)
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${LEDGER_MSVC_INCLUDE_PREFIX}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Sql Network)
include(CTest)

add_library(ledger_core STATIC
    src/infrastructure/audit_store.cpp
    src/infrastructure/audit_store.h
    src/infrastructure/database.cpp
    src/infrastructure/database.h
    src/domain/types.h
)
target_include_directories(ledger_core PUBLIC src)
target_link_libraries(ledger_core PUBLIC Qt6::Core Qt6::Sql)

add_library(ledger_domain STATIC src/domain/split.cpp src/domain/split.h)
target_include_directories(ledger_domain PUBLIC src)
target_link_libraries(ledger_domain PUBLIC Qt6::Core)

add_library(ledger_finance STATIC src/finance/finance_service.cpp src/finance/finance_service.h src/finance/finance_projection.cpp src/finance/finance_projection.h)
target_link_libraries(ledger_finance PUBLIC ledger_core ledger_domain)

add_library(ledger_expense STATIC src/expense/expense_service.cpp src/expense/expense_service.h src/expense/expense_workflow.cpp src/expense/expense_support.h)
target_link_libraries(ledger_expense PUBLIC ledger_core ledger_domain ledger_finance)

add_library(ledger_protocol STATIC src/transport/endpoint.cpp)
target_include_directories(ledger_protocol PUBLIC src)
target_link_libraries(ledger_protocol PUBLIC Qt6::Core)

add_library(ledger_client STATIC src/transport/api_client.cpp)
target_link_libraries(ledger_client PUBLIC ledger_protocol Qt6::Network)

add_library(ledger_query STATIC src/query/query_service.cpp src/query/query_service.h)
target_link_libraries(ledger_query PUBLIC ledger_core ledger_domain ledger_finance)

add_library(ledger_identity STATIC src/identity/identity_service.cpp src/identity/account_security.cpp src/identity/password.cpp src/identity/ledger_lifecycle.cpp src/identity/membership_service.cpp)
add_library(ledger_settlement STATIC src/settlement/settlement_service.cpp src/settlement/settlement_service.h)
target_link_libraries(ledger_settlement PUBLIC ledger_core ledger_domain ledger_finance)
target_link_libraries(ledger_identity PUBLIC ledger_core ledger_expense ledger_settlement ledger_query Qt6::Network)

add_library(ledger_server STATIC src/transport/local_server.cpp)
target_link_libraries(ledger_server PUBLIC ledger_identity ledger_protocol Qt6::Network)

add_library(ledger_recovery STATIC src/recovery/recovery_store.cpp src/recovery/recovery_store.h)
target_include_directories(ledger_recovery PUBLIC src)
target_link_libraries(ledger_recovery PUBLIC Qt6::Core Crypt32)
add_library(ledger_ui STATIC src/ui/account_security_ui.cpp src/ui/expense_recovery_ui.cpp src/ui/main_window.cpp src/ui/main_window.h src/ui/branding.cpp src/ui/branding.h src/ui/expense_page.cpp src/ui/expense_page.h src/ui/expense_details_dialog.cpp src/ui/expense_details_dialog.h src/ui/settlement_page.cpp src/ui/settlement_page.h src/ui/table_style.cpp src/ui/table_style.h src/ui/ledger_lifecycle_ui.cpp src/ui/finance_dialogs.cpp src/ui/finance_dialogs.h src/ui/expense_adjustment_ui.cpp src/ui/membership_ui.cpp src/ui/browse_page.cpp src/ui/browse_page.h)
target_link_libraries(ledger_ui PUBLIC ledger_client ledger_domain ledger_recovery Qt6::Widgets)

qt_add_executable(ledger_service WIN32 src/service_main.cpp)
target_link_libraries(ledger_service PRIVATE ledger_server)

qt_add_executable(shared_ledger WIN32
    src/main.cpp
)
target_link_libraries(shared_ledger PRIVATE ledger_ui)
add_dependencies(shared_ledger ledger_service)
qt_add_resources(shared_ledger application_styles PREFIX "/" FILES resources/styles.qss resources/chevron-down.png)

function(ledger_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

foreach(target ledger_recovery ledger_core ledger_domain ledger_query ledger_finance ledger_expense ledger_settlement ledger_protocol ledger_client ledger_identity ledger_server ledger_ui ledger_service shared_ledger)
    ledger_enable_warnings(${target})
endforeach()

if(BUILD_TESTING)
    qt_add_executable(ui_recovery_tests tests/ui_recovery_tests.cpp)
    set_target_properties(ui_recovery_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(ui_recovery_tests PRIVATE ledger_ui Qt6::Test)
    target_compile_definitions(ui_recovery_tests PRIVATE LEDGER_SERVICE_PATH="$<TARGET_FILE:ledger_service>")
    qt_add_resources(ui_recovery_tests recovery_test_styles PREFIX "/" FILES resources/styles.qss resources/chevron-down.png)
    add_dependencies(ui_recovery_tests ledger_service)
    ledger_enable_warnings(ui_recovery_tests)
    qt_add_executable(recovery_tests tests/recovery_tests.cpp)
    set_target_properties(recovery_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(recovery_tests PRIVATE ledger_recovery Qt6::Test)
    ledger_enable_warnings(recovery_tests)
    add_test(NAME local_encrypted_recovery COMMAND recovery_tests -o "${CMAKE_CURRENT_BINARY_DIR}/recovery-test-results.xml,junitxml" -o "-,txt")
    qt_add_executable(ui_query_tests tests/ui_query_tests.cpp)
    set_target_properties(ui_query_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(ui_query_tests PRIVATE ledger_ui ledger_core Qt6::Test)
    target_compile_definitions(ui_query_tests PRIVATE LEDGER_SERVICE_PATH="$<TARGET_FILE:ledger_service>")
    qt_add_resources(ui_query_tests query_test_styles PREFIX "/" FILES resources/styles.qss resources/chevron-down.png)
    add_dependencies(ui_query_tests ledger_service)
    ledger_enable_warnings(ui_query_tests)
    qt_add_executable(query_audit_tests tests/query_audit_tests.cpp)
    set_target_properties(query_audit_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(query_audit_tests PRIVATE ledger_identity Qt6::Test)
    ledger_enable_warnings(query_audit_tests)
    add_test(NAME query_statistics_and_audit_permissions COMMAND query_audit_tests -o "${CMAKE_CURRENT_BINARY_DIR}/query-audit-test-results.xml,junitxml" -o "-,txt")
    qt_add_executable(ui_membership_tests tests/ui_membership_tests.cpp)
    set_target_properties(ui_membership_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(ui_membership_tests PRIVATE ledger_ui Qt6::Test)
    target_compile_definitions(ui_membership_tests PRIVATE LEDGER_SERVICE_PATH="$<TARGET_FILE:ledger_service>")
    qt_add_resources(ui_membership_tests membership_test_styles PREFIX "/" FILES resources/styles.qss resources/chevron-down.png)
    add_dependencies(ui_membership_tests ledger_service)
    ledger_enable_warnings(ui_membership_tests)
    qt_add_executable(membership_tests tests/membership_tests.cpp)
    set_target_properties(membership_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(membership_tests PRIVATE ledger_identity Qt6::Test)
    ledger_enable_warnings(membership_tests)
    add_test(NAME membership_exit_transfer_and_history COMMAND membership_tests -o "${CMAKE_CURRENT_BINARY_DIR}/membership-test-results.xml,junitxml" -o "-,txt")
    qt_add_executable(ui_finance_tests tests/ui_finance_tests.cpp)
    set_target_properties(ui_finance_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(ui_finance_tests PRIVATE ledger_ui Qt6::Test)
    target_compile_definitions(ui_finance_tests PRIVATE LEDGER_SERVICE_PATH="$<TARGET_FILE:ledger_service>")
    qt_add_resources(ui_finance_tests finance_test_styles PREFIX "/" FILES resources/styles.qss resources/chevron-down.png)
    add_dependencies(ui_finance_tests ledger_service)
    ledger_enable_warnings(ui_finance_tests)
    qt_add_executable(finance_tests tests/finance_tests.cpp)
    set_target_properties(finance_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(finance_tests PRIVATE ledger_finance ledger_expense ledger_settlement Qt6::Test)
    ledger_enable_warnings(finance_tests)
    add_test(NAME refunds_and_post_settlement_adjustments COMMAND finance_tests -o "${CMAKE_CURRENT_BINARY_DIR}/finance-test-results.xml,junitxml" -o "-,txt")
    qt_add_executable(ledger_lifecycle_tests tests/ledger_lifecycle_tests.cpp)
    set_target_properties(ledger_lifecycle_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(ledger_lifecycle_tests PRIVATE ledger_identity Qt6::Test)
    ledger_enable_warnings(ledger_lifecycle_tests)
    add_test(NAME ledger_closure_and_audit COMMAND ledger_lifecycle_tests -o "${CMAKE_CURRENT_BINARY_DIR}/ledger-lifecycle-test-results.xml,junitxml" -o "-,txt")
    qt_add_executable(ui_settlement_tests tests/ui_settlement_tests.cpp)
    set_target_properties(ui_settlement_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(ui_settlement_tests PRIVATE ledger_ui Qt6::Test)
    target_compile_definitions(ui_settlement_tests PRIVATE LEDGER_SERVICE_PATH="$<TARGET_FILE:ledger_service>")
    qt_add_resources(ui_settlement_tests settlement_test_styles PREFIX "/" FILES resources/styles.qss resources/chevron-down.png)
    add_dependencies(ui_settlement_tests ledger_service)
    ledger_enable_warnings(ui_settlement_tests)
    qt_add_executable(settlement_tests tests/settlement_tests.cpp)
    set_target_properties(settlement_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(settlement_tests PRIVATE ledger_settlement ledger_expense Qt6::Test)
    ledger_enable_warnings(settlement_tests)
    add_test(NAME settlement_and_repayments COMMAND settlement_tests -o "${CMAKE_CURRENT_BINARY_DIR}/settlement-test-results.xml,junitxml" -o "-,txt")
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    qt_add_executable(database_tests tests/database_tests.cpp)
    set_target_properties(database_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(database_tests PRIVATE ledger_core Qt6::Test)
    ledger_enable_warnings(database_tests)
    add_test(NAME database_foundation COMMAND database_tests
        -o "${CMAKE_CURRENT_BINARY_DIR}/database-test-results.xml,junitxml" -o "-,txt")

    qt_add_executable(account_security_tests tests/account_security_tests.cpp)
    set_target_properties(account_security_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(account_security_tests PRIVATE ledger_identity Qt6::Test)
    ledger_enable_warnings(account_security_tests)
    add_test(NAME account_password_and_recovery COMMAND account_security_tests
        -o "${CMAKE_CURRENT_BINARY_DIR}/account-security-test-results.xml,junitxml" -o "-,txt")

    qt_add_executable(identity_tests tests/identity_tests.cpp)
    set_target_properties(identity_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(identity_tests PRIVATE ledger_identity Qt6::Test)
    ledger_enable_warnings(identity_tests)
    add_test(NAME identity_permissions COMMAND identity_tests
        -o "${CMAKE_CURRENT_BINARY_DIR}/identity-test-results.xml,junitxml" -o "-,txt")

    qt_add_executable(expense_tests tests/expense_tests.cpp)
    set_target_properties(expense_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(expense_tests PRIVATE ledger_expense Qt6::Test)
    ledger_enable_warnings(expense_tests)
    add_test(NAME expense_rules COMMAND expense_tests
        -o "${CMAKE_CURRENT_BINARY_DIR}/expense-test-results.xml,junitxml" -o "-,txt")

    qt_add_executable(workflow_tests tests/workflow_tests.cpp)
    set_target_properties(workflow_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(workflow_tests PRIVATE ledger_expense Qt6::Test)
    ledger_enable_warnings(workflow_tests)
    add_test(NAME expense_review_workflow COMMAND workflow_tests -o "${CMAKE_CURRENT_BINARY_DIR}/workflow-test-results.xml,junitxml" -o "-,txt")

    qt_add_executable(ipc_tests tests/ipc_tests.cpp)
    set_target_properties(ipc_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(ipc_tests PRIVATE ledger_client Qt6::Test)
    target_compile_definitions(ipc_tests PRIVATE LEDGER_SERVICE_PATH="$<TARGET_FILE:ledger_service>")
    add_dependencies(ipc_tests ledger_service)
    ledger_enable_warnings(ipc_tests)
    add_test(NAME desktop_service_integration COMMAND ipc_tests
        -o "${CMAKE_CURRENT_BINARY_DIR}/ipc-test-results.xml,junitxml" -o "-,txt")

    # Run explicitly through Verify-Desktop.ps1 -IdentityFlows on a Windows desktop.
    qt_add_executable(ui_security_tests tests/ui_security_tests.cpp)
    set_target_properties(ui_security_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(ui_security_tests PRIVATE ledger_ui Qt6::Test)
    target_compile_definitions(ui_security_tests PRIVATE LEDGER_SERVICE_PATH="$<TARGET_FILE:ledger_service>")
    qt_add_resources(ui_security_tests security_test_styles PREFIX "/" FILES resources/styles.qss resources/chevron-down.png)
    add_dependencies(ui_security_tests ledger_service)
    ledger_enable_warnings(ui_security_tests)

    qt_add_executable(ui_identity_tests tests/ui_identity_tests.cpp)
    set_target_properties(ui_identity_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(ui_identity_tests PRIVATE ledger_ui Qt6::Test)
    target_compile_definitions(ui_identity_tests PRIVATE LEDGER_SERVICE_PATH="$<TARGET_FILE:ledger_service>")
    qt_add_resources(ui_identity_tests test_styles PREFIX "/" FILES resources/styles.qss resources/chevron-down.png)
    add_dependencies(ui_identity_tests ledger_service)
    ledger_enable_warnings(ui_identity_tests)

    qt_add_executable(ui_expense_tests tests/ui_expense_tests.cpp)
    set_target_properties(ui_expense_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(ui_expense_tests PRIVATE ledger_ui Qt6::Test)
    target_compile_definitions(ui_expense_tests PRIVATE LEDGER_SERVICE_PATH="$<TARGET_FILE:ledger_service>")
    qt_add_resources(ui_expense_tests expense_test_styles PREFIX "/" FILES resources/styles.qss resources/chevron-down.png)
    add_dependencies(ui_expense_tests ledger_service)
    ledger_enable_warnings(ui_expense_tests)

    qt_add_executable(ui_workflow_tests tests/ui_workflow_tests.cpp)
    set_target_properties(ui_workflow_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    target_link_libraries(ui_workflow_tests PRIVATE ledger_ui Qt6::Test)
    target_compile_definitions(ui_workflow_tests PRIVATE LEDGER_SERVICE_PATH="$<TARGET_FILE:ledger_service>")
    qt_add_resources(ui_workflow_tests workflow_test_styles PREFIX "/" FILES resources/styles.qss resources/chevron-down.png)
    add_dependencies(ui_workflow_tests ledger_service)
    ledger_enable_warnings(ui_workflow_tests)
endif()

install(TARGETS shared_ledger ledger_service RUNTIME DESTINATION .)

