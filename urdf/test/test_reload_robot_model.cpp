/*
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2016, Open Source Robotics Foundation, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage, Inc. nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

#include <string>

#include <gtest/gtest.h>

#include "urdf/model.h"

const std::string parameter_key = "robot_description";
const std::string new_parameter_key = "new_robot_description";

/* Simple case: Initialize a robot model and a robot_description parameter
 * Publish a new robot_description parameter
 * send request/publish to latched topic
 * Expect to see the change in the in-memory representation of the model
 */
TEST(TestURDF, reload_robot_model)
{
  urdf::Model model;
  // Initialize the model with reload_robot_model set as true
  ASSERT_TRUE(model.initParam(parameter_key, true));
  EXPECT_EQ(model.name_, "r2d2");
  EXPECT_EQ(model.links_.size(), 17);

  ros::NodeHandle node_handle;
  auto reload_model_client = node_handle.serviceClient<std_srvs::Trigger>("/reload_robot_model");

  // read text from new_robot_dscription a
  ASSERT_TRUE(node_handle.hasParam(new_parameter_key));
  std::string new_urdf;
  node_handle.getParam(new_parameter_key, new_urdf);
  ASSERT_TRUE(node_handle.hasParam(parameter_key));
  node_handle.setParam(parameter_key, new_urdf);

  std_srvs::Trigger::Request req;
  std_srvs::Trigger::Response res;

  // may have to wait a second for the parameter server to get the update?
  ASSERT_TRUE(reload_model_client.call(req, res));

  // Check the new model representation
  EXPECT_EQ(model.name_, "one_link");
  EXPECT_EQ(model.links_.size(), 1);
}


/* Multithreaded case
 * Lock model accesses
 */


/* It's important to keep in mind that changes to the urdf::Model DO NOT affect the
 * robot_description parameter, and changes to the parameter will only affect the Model
 * when reload_robot_model service is manually triggered.
 */


int main(int argc, char** argv)
{
  ros::init(argc, argv, "test_mutable_robot_description");
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
