// Copyright 2020 ros2_control Development Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <vector>

#include "hardware_interface/sensor_interface.hpp"
#include "rclcpp/logging.hpp"

using hardware_interface::return_type;
using hardware_interface::SensorInterface;
using hardware_interface::StateInterface;

class TestSensor : public SensorInterface
{
  CallbackReturn on_init(
    const hardware_interface::HardwareComponentInterfaceParams & params) override
  {
    if (SensorInterface::on_init(params) != CallbackReturn::SUCCESS)
    {
      return CallbackReturn::ERROR;
    }
    // can only give feedback state for velocity
    if (get_hardware_info().sensors[0].state_interfaces.size() == 2)
    {
      return CallbackReturn::ERROR;
    }
    if (get_hardware_info().rw_rate == 0u)
    {
      RCLCPP_WARN(
        get_logger(),
        "Sensor hardware component '%s' from plugin '%s' failed to initialize as rw_rate is 0.",
        get_hardware_info().name.c_str(), get_hardware_info().hardware_plugin_name.c_str());
      return CallbackReturn::ERROR;
    }
    return CallbackReturn::SUCCESS;
  }

  std::vector<StateInterface> export_state_interfaces() override
  {
    std::vector<StateInterface> state_interfaces;
    state_interfaces.emplace_back(
      hardware_interface::StateInterface(
        get_hardware_info().sensors[0].name,
        get_hardware_info().sensors[0].state_interfaces[0].name, &velocity_state_));

    return state_interfaces;
  }

  return_type read(const rclcpp::Time & /*time*/, const rclcpp::Duration & /*period*/) override
  {
    return return_type::OK;
  }

private:
  double velocity_state_ = 0.0;
};

class TestUninitializableSensor : public TestSensor
{
  CallbackReturn on_init(
    const hardware_interface::HardwareComponentInterfaceParams & params) override
  {
    SensorInterface::on_init(params);
    return CallbackReturn::ERROR;
  }
};

#include "pluginlib/class_list_macros.hpp"  // NOLINT
PLUGINLIB_EXPORT_CLASS(TestSensor, hardware_interface::SensorInterface)
PLUGINLIB_EXPORT_CLASS(TestUninitializableSensor, hardware_interface::SensorInterface)
