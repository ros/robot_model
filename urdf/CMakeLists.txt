cmake_minimum_required(VERSION 2.8.3)
project(urdf)

if(NOT WIN32)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wall -Wextra")
endif()

find_package(Boost REQUIRED thread)
find_package(urdfdom REQUIRED)
find_package(urdfdom_headers REQUIRED)
find_package(catkin REQUIRED COMPONENTS
  urdf_parser_plugin pluginlib rosconsole_bridge roscpp cmake_modules std_srvs)


find_package(TinyXML REQUIRED)

find_package(PkgConfig)
pkg_check_modules(libpcrecpp libpcrecpp)

catkin_package(
  LIBRARIES ${PROJECT_NAME}
  INCLUDE_DIRS include ${TinyXML_INLCLUDE_DIRS}
  CATKIN_DEPENDS rosconsole_bridge roscpp
  DEPENDS urdfdom_headers urdfdom Boost pcrecpp
)

include_directories(SYSTEM ${Boost_INCLUDE_DIR})
link_directories(${Boost_LIBRARY_DIRS})

include_directories(
  include
  ${catkin_INCLUDE_DIRS}
  ${urdfdom_INCLUDE_DIRS}
  ${urdfdom_headers_INCLUDE_DIRS}
  ${TinyXML_INCLUDE_DIRS}
  )
link_directories(${catkin_LIBRARY_DIRS})

add_library(${PROJECT_NAME} src/model.cpp src/rosconsole_bridge.cpp)
target_link_libraries(${PROJECT_NAME} ${TinyXML_LIBRARIES} ${catkin_LIBRARIES} ${urdfdom_LIBRARIES})

if(APPLE)
  set_target_properties(${PROJECT_NAME} PROPERTIES LINK_FLAGS "-undefined dynamic_lookup")
endif(APPLE)

if(CATKIN_ENABLE_TESTING)
  find_package(catkin REQUIRED COMPONENTS rostest)
  add_rostest_gtest(test_urdf_parser test/test_robot_model_parser.launch test/test_robot_model_parser.cpp)
  target_link_libraries(test_urdf_parser ${PROJECT_NAME})

  add_rostest_gtest(test_reload_robot_model
                   test/test_reload_robot_model.launch
                   test/test_reload_robot_model.cpp)
  target_link_libraries(test_reload_robot_model ${PROJECT_NAME})
endif()

install(TARGETS ${PROJECT_NAME}
  ARCHIVE DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
  LIBRARY DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
  RUNTIME DESTINATION ${CATKIN_PACKAGE_BIN_DESTINATION})

install(DIRECTORY include/${PROJECT_NAME}/
  DESTINATION ${CATKIN_PACKAGE_INCLUDE_DESTINATION})
