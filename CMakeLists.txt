cmake_minimum_required(VERSION 3.16)
project(servidor_http VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()
find_package(Threads REQUIRED)

# Partes puras (sem rede): testadas direto.
add_library(httpcore STATIC src/http.c src/path.c src/mime.c src/strbuf.c)
target_include_directories(httpcore PUBLIC src)

add_executable(servidor-http src/main.c src/server.c src/pool.c)
target_link_libraries(servidor-http PRIVATE httpcore Threads::Threads)

enable_testing()
add_executable(test_http tests/test_http.c)
target_link_libraries(test_http PRIVATE httpcore)
add_test(NAME http COMMAND test_http)
add_test(NAME integracao COMMAND sh ${CMAKE_SOURCE_DIR}/tests/integracao.sh ${CMAKE_BINARY_DIR})
