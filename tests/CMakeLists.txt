include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
  GIT_SHALLOW TRUE
  FIND_PACKAGE_ARGS 3
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# ==============================================================================
# Compile-time (static) checks, unit tests and integration tests: one executable
# ==============================================================================
# The integration tests skip themselves unless SCHEMACHT_TEST_DB holds a libpq connection string.
add_executable(
  schemacht_tests
  compile/raw_statement_test.cpp
  compile/runner_test.cpp
  compile/schema_test.cpp
  compile/sql_test.cpp
  compile/table_test.cpp
  unit/binary_form_test.cpp
  unit/chrono_uuid_test.cpp
  unit/column_type_test.cpp
  unit/field_codec_test.cpp
  unit/prepared_test.cpp
  unit/row_reader_test.cpp
  unit/row_test.cpp
  unit/statements_test.cpp
  unit/table_name_test.cpp
  unit/thread_pool_test.cpp
  unit/util_test.cpp
  integration/database_test.cpp
  integration/table_differences_test.cpp
  integration/transaction_test.cpp
)
target_include_directories(schemacht_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(schemacht_tests PRIVATE schemacht::schemacht Catch2::Catch2WithMain)
catch_discover_tests(schemacht_tests)

# Failing allocations on purpose: its own executable, as it replaces the global operator new. Skipped without SCHEMACHT_TEST_DB.
add_executable(schemacht_fault_tests fault/allocation_test.cpp)
target_include_directories(schemacht_fault_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(schemacht_fault_tests PRIVATE schemacht::schemacht Catch2::Catch2WithMain)
catch_discover_tests(schemacht_fault_tests)

# ==============================================================================
# The examples, run against the test database (skipped, as the integration tests are, without SCHEMACHT_TEST_DB)
# ==============================================================================
foreach(example first_program basic custom_types transfers extensions)
  if(TARGET schemacht_example_${example})
    add_test(NAME example.${example} COMMAND ${CMAKE_COMMAND} -DEXAMPLE=$<TARGET_FILE:schemacht_example_${example}> -P
                                             ${CMAKE_CURRENT_SOURCE_DIR}/run_example.cmake)
    set_tests_properties(example.${example} PROPERTIES SKIP_REGULAR_EXPRESSION "SCHEMACHT_TEST_DB is not set: skipped")
  endif()
endforeach()

# ==============================================================================
# Compile-fail tests
# ==============================================================================
# Each compiles compile_fail/cases.cpp with one case defined, and passes if the compiler's output has the expected message
# (which it can only print by failing).
function(schemacht_compile_fail case regex)
  add_test(
    NAME compile_fail.${case}
    COMMAND ${CMAKE_CXX_COMPILER} -std=c++20 -fsyntax-only -I${PROJECT_SOURCE_DIR}/include -DCASE_${case}
            ${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/cases.cpp
  )
  set_tests_properties(compile_fail.${case} PROPERTIES PASS_REGULAR_EXPRESSION "${regex}")
endfunction()

schemacht_compile_fail(BAD_TABLE_NAME "a table name must be lowercase")
schemacht_compile_fail(BAD_COLUMN_NAME "a column name must be lowercase")
schemacht_compile_fail(DUPLICATE_COLUMN "two fields of the schema have the same column name")
schemacht_compile_fail(UINT64_FIELD "64-bit unsigned column")
schemacht_compile_fail(UNKNOWN_COL "no column with that name")
schemacht_compile_fail(UNKNOWN_FIELD_BY_NAME "the schema has no column with that name")
schemacht_compile_fail(NARROWING_BIND "would be narrowed")
schemacht_compile_fail(NEGATIVE_FOR_UNSIGNED_BIND "would be narrowed")
schemacht_compile_fail(WRONG_ARGUMENT_COUNT "wrong number of arguments for this query")
schemacht_compile_fail(BAD_OPERATOR "must be a single SQL operator")
schemacht_compile_fail(BAD_FUNCTION "must be a SQL function name")
schemacht_compile_fail(RAW_UNUSED_ARGUMENT "has no \\$n placeholder")
schemacht_compile_fail(RAW_PLACEHOLDER_BEYOND "beyond the arguments it declares")
schemacht_compile_fail(RAW_DOLLAR_QUOTE "dollar-quoted text is not supported")
schemacht_compile_fail(RAW_NARROWING_BIND "raw statement declares, or would be narrowed")
schemacht_compile_fail(RAW_REFERENCE_ARGUMENT "RawArgument")
schemacht_compile_fail(RAW_NOT_ARGUMENTS "IsRawArguments")
schemacht_compile_fail(RAW_ARGUMENT_WITHOUT_TEXT_FORM "EVERY_ARGUMENT_HAS_TEXT_FORM")
schemacht_compile_fail(INSERT_FIELD_WITHOUT_TEXT_FORM "TextSchema")
schemacht_compile_fail(RAW_RESULT_WITHOUT_TEXT_FORM "TextRow")
schemacht_compile_fail(GET_UNKNOWN_COLUMN "HasColumnNamed")
schemacht_compile_fail(SELECT_BY_KEY_COMPOSITE "HasSingleColumnKey|primary_key_type")
schemacht_compile_fail(UPDATE_WITHOUT_WHERE "must say which rows it changes")
schemacht_compile_fail(UPDATE_SET_TWICE "can set a column only once")
schemacht_compile_fail(RETURNING_TWICE "can be returned only once")
schemacht_compile_fail(UPDATE_SET_NULL_REQUIRED "NullableColumn")
schemacht_compile_fail(UPDATE_NARROWING_SET "would be narrowed")
schemacht_compile_fail(DELETE_WHERE_EVERYTHING "would delete every row")
schemacht_compile_fail(CREATE_TABLE_GENERATED "only as an identity key")
schemacht_compile_fail(CREATE_TABLE_WITHOUT_SQL_TYPE "a column's type has no SQL name")
schemacht_compile_fail(COLUMN_TYPE_WITHOUT_FORMAT "HasTextForm")
schemacht_compile_fail(IN_ON_ARRAY_COLUMN "can't test a column that is itself an array")
schemacht_compile_fail(OPERATION_ARGUMENT_TYPE "the type its operation declares")
schemacht_compile_fail(OPERATION_ARGUMENT_OTHER_TYPE "the type its operation declares")
schemacht_compile_fail(OPERATION_ARGUMENT_DISAGREES "disagree")
schemacht_compile_fail(OPERATION_ARGUMENT_NOT_COLUMN_TYPE "must be a column type of your own")
schemacht_compile_fail(RANKING_COLUMN_PREFIX "RankingOn")
schemacht_compile_fail(TABLE_WRONG_SCHEMA "a statement runs only on a table of its own schema's shape")
schemacht_compile_fail(TABLE_WRONG_SCHEMA_INSERT "a statement runs only on a table of its own schema's shape")
schemacht_compile_fail(TABLE_RAW_STATEMENT "no member named")
schemacht_compile_fail(UPSERT_NO_KEY "upsert: the schema has no primary key")
schemacht_compile_fail(UPSERT_GENERATED_KEY "upsert: the database numbers the primary key")
schemacht_compile_fail(UPSERT_NOTHING_TO_SET "has nothing to update")
schemacht_compile_fail(UPSERT_MERGE_KEY "upsert: a MergeWith or Keep names a primary key column")
schemacht_compile_fail(UPSERT_MERGE_GENERATED "upsert: a MergeWith or Keep names a GENERATED column")
schemacht_compile_fail(UPSERT_MERGE_UNKNOWN "the schema has no column with that name")
schemacht_compile_fail(UPSERT_MERGE_TWICE "upsert: two of its MergeWith and Keep name the same column")
schemacht_compile_fail(UPSERT_KEEP_KEY "upsert: a MergeWith or Keep names a primary key column")
schemacht_compile_fail(UPSERT_KEEP_EVERYTHING "every column besides those matched on is kept")
schemacht_compile_fail(UPSERT_CONFLICT_NOT_UNIQUE "upsert: ConflictOn names a column that is not KeyRole::Unique")
schemacht_compile_fail(UPSERT_CONFLICT_ON_KEY "upsert: ConflictOn names a column that is not KeyRole::Unique")
schemacht_compile_fail(UPSERT_CONFLICT_UNKNOWN "the schema has no column with that name")
schemacht_compile_fail(UPSERT_CONFLICT_TWICE "upsert: two ConflictOn")
schemacht_compile_fail(UPSERT_CONFLICT_GENERATED "upsert: the database computes the ConflictOn column")
schemacht_compile_fail(UPSERT_KEEP_CONFLICT_COLUMN "upsert: a MergeWith or Keep names the ConflictOn column")
schemacht_compile_fail(UPSERT_CONFLICT_NOTHING_TO_SET "has nothing to update")
schemacht_compile_fail(UPSERT_KEEP_AND_MERGE "upsert: two of its MergeWith and Keep name the same column")
schemacht_compile_fail(UPSERT_MERGE_BAD_FUNCTION "must be a SQL function name")
schemacht_compile_fail(UPSERT_MERGE_BAD_OPERATOR "must be a single SQL operator")
schemacht_compile_fail(UPSERT_MERGE_ARGUMENT_PREFIX "MergeOperation")
schemacht_compile_fail(UPSERT_MERGE_COLUMN_PREFIX "MergeOperation")
schemacht_compile_fail(UPSERT_MERGE_BOTH "MergeOperation")
schemacht_compile_fail(STREAM_CHUNKS_ROW_HANDLER "ChunkCallback")
schemacht_compile_fail(UNCHECKED_SQL_ARGUMENT_WITHOUT_TEXT_FORM "UncheckedSqlArgument")

# With no case defined, cases.cpp must compile: the failures above are the cases', not the file's. It must as C++23 too: the
# library is written for C++20, and a project on a later standard includes the same headers.
add_test(NAME compile_fail.baseline COMMAND ${CMAKE_CXX_COMPILER} -std=c++20 -fsyntax-only -I${PROJECT_SOURCE_DIR}/include
                                            ${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/cases.cpp)
add_test(NAME compile_fail.baseline_cxx23 COMMAND ${CMAKE_CXX_COMPILER} -std=c++23 -fsyntax-only -I${PROJECT_SOURCE_DIR}/include
                                                  ${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/cases.cpp)
