cmake_minimum_required(VERSION 3.20)
project(StateRAMTrueReclaim LANGUAGES CXX)

add_executable(StateRAMTrueReclaim true_reclaim.cpp)
target_compile_features(StateRAMTrueReclaim PRIVATE cxx_std_17)
target_compile_definitions(StateRAMTrueReclaim PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(StateRAMTrueReclaim PRIVATE Cabinet Psapi)

if(MSVC)
  target_compile_options(StateRAMTrueReclaim PRIVATE /W4 /O2 /EHsc)
endif()
