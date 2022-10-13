cmake_minimum_required(VERSION 3.17)

project(ecs)

file(
	GLOB
	SOURCE
	"./*.c"
)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-incompatible-pointer-types")

add_library(ecs ${SOURCE})
