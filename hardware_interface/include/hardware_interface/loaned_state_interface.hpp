// Copyright 2020 Open Source Robotics Foundation, Inc.
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

#ifndef HARDWARE_INTERFACE__LOANED_STATE_INTERFACE_HPP_
#define HARDWARE_INTERFACE__LOANED_STATE_INTERFACE_HPP_

#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <utility>

#include "hardware_interface/handle.hpp"
#include "rclcpp/logging.hpp"
namespace hardware_interface
{
class LoanedStateInterface
{
public:
  using Deleter = std::function<void(void)>;

  [[deprecated("Replaced by the new version using shared_ptr")]] explicit LoanedStateInterface(
    const StateInterface & state_interface)
  : LoanedStateInterface(state_interface, nullptr)
  {
  }

  [[deprecated("Replaced by the new version using shared_ptr")]] LoanedStateInterface(
    const StateInterface & state_interface, Deleter && deleter)
  : state_interface_(state_interface), deleter_(std::forward<Deleter>(deleter))
  {
  }

  explicit LoanedStateInterface(StateInterface::ConstSharedPtr state_interface)
  : LoanedStateInterface(state_interface, nullptr)
  {
  }

  LoanedStateInterface(StateInterface::ConstSharedPtr state_interface, Deleter && deleter)
  : state_interface_(*state_interface), deleter_(std::forward<Deleter>(deleter))
  {
  }

  LoanedStateInterface(const LoanedStateInterface & other) = delete;

  LoanedStateInterface(LoanedStateInterface && other) = default;

  virtual ~LoanedStateInterface()
  {
    auto logger = rclcpp::get_logger(state_interface_.get_name());
    RCLCPP_WARN_EXPRESSION(
      logger,
      (get_value_statistics_.failed_counter > 0 || get_value_statistics_.timeout_counter > 0),
      "LoanedStateInterface %s has %u (%.4f %%) timeouts and %u (%.4f %%) missed calls out of %u "
      "get_value calls",
      state_interface_.get_name().c_str(), get_value_statistics_.timeout_counter,
      (get_value_statistics_.timeout_counter * 100.0) / get_value_statistics_.total_counter,
      get_value_statistics_.failed_counter,
      (get_value_statistics_.failed_counter * 10.0) / get_value_statistics_.total_counter,
      get_value_statistics_.total_counter);
    if (deleter_)
    {
      deleter_();
    }
  }

  const std::string & get_name() const { return state_interface_.get_name(); }

  const std::string & get_interface_name() const { return state_interface_.get_interface_name(); }

  const std::string & get_prefix_name() const { return state_interface_.get_prefix_name(); }

  [[deprecated(
    "Use std::optional<T> get_optional() instead to retrieve the value. This method will be "
    "removed by the ROS 2 Kilted Kaiju release.")]]
  double get_value() const
  {
    std::optional<double> opt_value = get_optional();
    if (opt_value.has_value())
    {
      return opt_value.value();
    }
    else
    {
      return std::numeric_limits<double>::quiet_NaN();
    }
  }

  /**
   * @brief Get the value of the state interface.
   * @tparam T The type of the value to be retrieved.
   * @return The value of the state interface if it accessed successfully, std::nullopt otherwise.
   * @param max_tries The maximum number of tries to get the value.
   *
   * @note The method is thread-safe and non-blocking.
   * @note When different threads access the internal handle at same instance, and if they are
   * unable to lock the handle to access the value, the handle returns std::nullopt. If the
   * operation is successful, the value is returned.
   * @note The method will try to get the value max_tries times before returning std::nullopt. The
   * method will yield the thread between tries. If the value is retrieved successfully, the method
   * returns the value immediately.
   */
  template <typename T = double>
  [[nodiscard]] std::optional<T> get_optional(unsigned int max_tries = 10) const
  {
    unsigned int nr_tries = 0;
    do
    {
      ++get_value_statistics_.total_counter;
      const std::optional<T> data = state_interface_.get_optional<T>();
      if (data.has_value())
      {
        return data;
      }
      ++get_value_statistics_.failed_counter;
      ++nr_tries;
      std::this_thread::yield();
    } while (nr_tries < max_tries);

    ++get_value_statistics_.timeout_counter;
    return std::nullopt;
  }

  /**
   * @brief Get the data type of the state interface.
   * @return The data type of the state interface.
   */
  HandleDataType get_data_type() const { return state_interface_.get_data_type(); }

  /**
   * @brief Check if the state interface can be casted to double.
   * @return True if the state interface can be casted to double, false otherwise.
   */
  bool is_castable_to_double() const { return state_interface_.is_castable_to_double(); }

protected:
  const StateInterface & state_interface_;
  Deleter deleter_;

private:
  struct HandleRTStatistics
  {
    unsigned int total_counter = 0;
    unsigned int failed_counter = 0;
    unsigned int timeout_counter = 0;
  };
  mutable HandleRTStatistics get_value_statistics_;
};

}  // namespace hardware_interface
#endif  // HARDWARE_INTERFACE__LOANED_STATE_INTERFACE_HPP_
