// Copyright 2020 PAL Robotics S.L.
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

#ifndef HARDWARE_INTERFACE__HANDLE_HPP_
#define HARDWARE_INTERFACE__HANDLE_HPP_

#include <fmt/compile.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>
#include <variant>

#include "hardware_interface/hardware_info.hpp"
#include "hardware_interface/introspection.hpp"
#include "hardware_interface/lexical_casts.hpp"
#include "hardware_interface/macros.hpp"

#include "rclcpp/logging.hpp"

namespace
{
#ifndef _WIN32
template <typename T>
std::string get_type_name()
{
  int status = 0;
  std::unique_ptr<char[], void (*)(void *)> res{
    abi::__cxa_demangle(typeid(T).name(), nullptr, nullptr, &status), std::free};
  return (status == 0) ? res.get() : typeid(T).name();
}
#else
// not supported on Windows, use typeid directly
template <typename T>
std::string get_type_name()
{
  return typeid(T).name();
}
#endif
}  // namespace

namespace hardware_interface
{

using HANDLE_DATATYPE = std::variant<std::monostate, double, bool>;

/// A handle used to get and set a value on a given interface.
class Handle
{
public:
  [[deprecated("Use InterfaceDescription for initializing the Interface")]]
  Handle(const std::string & prefix_name, const std::string & interface_name, double * value_ptr)
  : prefix_name_(prefix_name),
    interface_name_(interface_name),
    handle_name_(prefix_name_ + "/" + interface_name_),
    value_ptr_(value_ptr)
  {
  }

  explicit Handle(
    const std::string & prefix_name, const std::string & interface_name,
    const std::string & data_type = "double", const std::string & initial_value = "")
  : prefix_name_(prefix_name),
    interface_name_(interface_name),
    handle_name_(prefix_name_ + "/" + interface_name_),
    data_type_(data_type)
  {
    // As soon as multiple datatypes are used in HANDLE_DATATYPE
    // we need to initialize according the type passed in interface description
    if (data_type_ == hardware_interface::HandleDataType::DOUBLE)
    {
      try
      {
        value_ = initial_value.empty() ? std::numeric_limits<double>::quiet_NaN()
                                       : hardware_interface::stod(initial_value);
        value_ptr_ = std::get_if<double>(&value_);
      }
      catch (const std::invalid_argument & err)
      {
        throw std::invalid_argument(
          fmt::format(
            FMT_COMPILE(
              "Invalid initial value : '{}' parsed for interface : '{}' with type : '{}'"),
            initial_value, handle_name_, data_type_.to_string()));
      }
    }
    else if (data_type_ == hardware_interface::HandleDataType::BOOL)
    {
      value_ptr_ = nullptr;
      value_ = initial_value.empty() ? false : hardware_interface::parse_bool(initial_value);
    }
    else
    {
      throw std::runtime_error(
        fmt::format(
          FMT_COMPILE(
            "Invalid data type : '{}' for interface : {}. Supported types are double and bool."),
          data_type, handle_name_));
    }
  }

  explicit Handle(const InterfaceDescription & interface_description)
  : Handle(
      interface_description.get_prefix_name(), interface_description.get_interface_name(),
      interface_description.get_data_type_string(),
      interface_description.interface_info.initial_value)
  {
  }

  [[deprecated("Use InterfaceDescription for initializing the Interface")]]

  explicit Handle(const std::string & interface_name)
  : interface_name_(interface_name), handle_name_("/" + interface_name_), value_ptr_(nullptr)
  {
  }

  [[deprecated("Use InterfaceDescription for initializing the Interface")]]

  explicit Handle(const char * interface_name)
  : interface_name_(interface_name), handle_name_("/" + interface_name_), value_ptr_(nullptr)
  {
  }

  Handle(const Handle & other) noexcept { copy(other); }

  Handle & operator=(const Handle & other)
  {
    if (this != &other)
    {
      copy(other);
    }
    return *this;
  }

  Handle(Handle && other) noexcept { swap(*this, other); }

  Handle & operator=(Handle && other)
  {
    swap(*this, other);
    return *this;
  }

  virtual ~Handle() = default;

  /// Returns true if handle references a value.
  inline operator bool() const { return value_ptr_ != nullptr; }

  const std::string & get_name() const { return handle_name_; }

  const std::string & get_interface_name() const { return interface_name_; }

  const std::string & get_prefix_name() const { return prefix_name_; }

  [[deprecated(
    "Use std::optional<T> get_optional() instead to retrieve the value. This method will be "
    "removed by the ROS 2 Kilted Kaiju release.")]]
  double get_value() const
  {
    std::shared_lock<std::shared_mutex> lock(handle_mutex_, std::try_to_lock);
    if (!lock.owns_lock())
    {
      return std::numeric_limits<double>::quiet_NaN();
    }
    // BEGIN (Handle export change): for backward compatibility
    // TODO(Manuel) return value_ if old functionality is removed
    THROW_ON_NULLPTR(value_ptr_);
    return *value_ptr_;
    // END
  }

  /**
   * @brief Get the value of the handle.
   * @tparam T The type of the value to be retrieved.
   * @return The value of the handle if it accessed successfully, std::nullopt otherwise.
   *
   * @note The method is thread-safe and non-blocking.
   * @note When different threads access the same handle at same instance, and if they are unable to
   * lock the handle to access the value, the handle returns std::nullopt. If the operation is
   * successful, the value is returned.
   */
  template <typename T = double>
  [[nodiscard]] std::optional<T> get_optional() const
  {
    std::shared_lock<std::shared_mutex> lock(handle_mutex_, std::try_to_lock);
    return get_optional<T>(lock);
  }
  /**
   * @brief Get the value of the handle.
   * @tparam T The type of the value to be retrieved.
   * @param lock The lock to access the value.
   * @return The value of the handle if it accessed successfully, std::nullopt otherwise.
   *
   * @note The method is thread-safe and non-blocking.
   * @note When different threads access the same handle at same instance, and if they are unable to
   * lock the handle to access the value, the handle returns std::nullopt. If the operation is
   * successful, the value is returned.
   */
  template <typename T = double>
  [[nodiscard]] std::optional<T> get_optional(std::shared_lock<std::shared_mutex> & lock) const
  {
    if (!lock.owns_lock())
    {
      return std::nullopt;
    }
    // BEGIN (Handle export change): for backward compatibility
    // TODO(saikishor) return value_ if old functionality is removed
    if constexpr (std::is_same_v<T, double>)
    {
      switch (data_type_)
      {
        case HandleDataType::DOUBLE:
          THROW_ON_NULLPTR(value_ptr_);
          return *value_ptr_;
        case HandleDataType::BOOL:
          RCLCPP_WARN_ONCE(
            rclcpp::get_logger(get_name()),
            "Casting bool to double for interface : %s. Better use get_optional<bool>().",
            get_name().c_str());
          return static_cast<double>(std::get<bool>(value_));
        default:
          throw std::runtime_error(
            fmt::format(
              FMT_COMPILE("Data type : '{}' cannot be casted to double for interface : {}"),
              data_type_.to_string(), get_name()));
      }
    }
    try
    {
      return std::get<T>(value_);
    }
    catch (const std::bad_variant_access & err)
    {
      throw std::runtime_error(
        fmt::format(
          FMT_COMPILE("Invalid data type : '{}' access for interface : {} expected : '{}'"),
          get_type_name<T>(), get_name(), data_type_.to_string()));
    }
    // END
  }

  /**
   * @brief Set the value of the handle.
   * @tparam T The type of the value to be set.
   * @param value The value to be set.
   * @return true if the value is set successfully, false otherwise.
   *
   * @note The method is thread-safe and non-blocking.
   * @note When different threads access the same handle at same instance, and if they are unable to
   * lock the handle to set the value, the handle returns false. If the operation is successful, the
   * handle is updated and returns true.
   */
  template <typename T>
  [[nodiscard]] bool set_value(const T & value)
  {
    std::unique_lock<std::shared_mutex> lock(handle_mutex_, std::try_to_lock);
    return set_value(lock, value);
  }

  /**
   * @brief Set the value of the handle.
   * @tparam T The type of the value to be set.
   * @param lock The lock to set the value.
   * @param value The value to be set.
   * @return true if the value is set successfully, false otherwise.
   *
   * @note The method is thread-safe and non-blocking.
   * @note When different threads access the same handle at same instance, and if they are unable to
   * lock the handle to set the value, the handle returns false. If the operation is successful, the
   * handle is updated and returns true.
   */
  template <typename T>
  [[nodiscard]] bool set_value(std::unique_lock<std::shared_mutex> & lock, const T & value)
  {
    if (!lock.owns_lock())
    {
      return false;
    }
    // BEGIN (Handle export change): for backward compatibility
    // TODO(Manuel) set value_ directly if old functionality is removed
    if constexpr (std::is_same_v<T, double>)
    {
      // If the template is of type double, check if the value_ptr_ is not nullptr
      THROW_ON_NULLPTR(value_ptr_);
      *value_ptr_ = value;
    }
    else
    {
      if (!std::holds_alternative<T>(value_))
      {
        throw std::runtime_error(
          fmt::format(
            FMT_COMPILE("Invalid data type : '{}' access for interface : {} expected : '{}'"),
            get_type_name<T>(), get_name(), data_type_.to_string()));
      }
      value_ = value;
    }
    return true;
    // END
  }

  std::shared_mutex & get_mutex() const { return handle_mutex_; }

  HandleDataType get_data_type() const { return data_type_; }

  /// Returns true if the handle data type can be casted to double.
  bool is_castable_to_double() const { return data_type_.is_castable_to_double(); }

private:
  void copy(const Handle & other) noexcept
  {
    std::scoped_lock lock(other.handle_mutex_, handle_mutex_);
    prefix_name_ = other.prefix_name_;
    interface_name_ = other.interface_name_;
    handle_name_ = other.handle_name_;
    value_ = other.value_;
    if (std::holds_alternative<std::monostate>(value_))
    {
      value_ptr_ = other.value_ptr_;
    }
    else
    {
      value_ptr_ = std::get_if<double>(&value_);
    }
  }

  void swap(Handle & first, Handle & second) noexcept
  {
    std::scoped_lock lock(first.handle_mutex_, second.handle_mutex_);
    std::swap(first.prefix_name_, second.prefix_name_);
    std::swap(first.interface_name_, second.interface_name_);
    std::swap(first.handle_name_, second.handle_name_);
    std::swap(first.value_, second.value_);
    std::swap(first.value_ptr_, second.value_ptr_);
  }

protected:
  std::string prefix_name_;
  std::string interface_name_;
  std::string handle_name_;
  HANDLE_DATATYPE value_ = std::monostate{};
  HandleDataType data_type_ = HandleDataType::DOUBLE;
  // BEGIN (Handle export change): for backward compatibility
  // TODO(Manuel) redeclare as HANDLE_DATATYPE * value_ptr_ if old functionality is removed
  double * value_ptr_;
  // END
  mutable std::shared_mutex handle_mutex_;
};

class StateInterface : public Handle
{
public:
  explicit StateInterface(const InterfaceDescription & interface_description)
  : Handle(interface_description)
  {
  }

  void registerIntrospection() const
  {
    if (value_ptr_ || std::holds_alternative<double>(value_))
    {
      std::function<double()> f = [this]()
      { return value_ptr_ ? *value_ptr_ : std::get<double>(value_); };
      DEFAULT_REGISTER_ROS2_CONTROL_INTROSPECTION("state_interface." + get_name(), f);
    }
  }

  void unregisterIntrospection() const
  {
    if (value_ptr_ || std::holds_alternative<double>(value_))
    {
      DEFAULT_UNREGISTER_ROS2_CONTROL_INTROSPECTION("state_interface." + get_name());
    }
  }

  StateInterface(const StateInterface & other) = default;

  StateInterface(StateInterface && other) = default;

// Disable deprecated warnings
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  using Handle::Handle;
#pragma GCC diagnostic pop

  using SharedPtr = std::shared_ptr<StateInterface>;
  using ConstSharedPtr = std::shared_ptr<const StateInterface>;
};

class CommandInterface : public Handle
{
public:
  explicit CommandInterface(const InterfaceDescription & interface_description)
  : Handle(interface_description)
  {
  }
  /// CommandInterface copy constructor is actively deleted.
  /**
   * Command interfaces are having a unique ownership and thus
   * can't be copied in order to avoid simultaneous writes to
   * the same resource.
   */
  CommandInterface(const CommandInterface & other) = delete;

  CommandInterface(CommandInterface && other) = default;

  void set_on_set_command_limiter(std::function<double(double, bool &)> on_set_command_limiter)
  {
    on_set_command_limiter_ = on_set_command_limiter;
  }

  /// A setter for the value of the command interface that triggers the limiter.
  /**
   * @param value The value to be set.
   * @return True if the value was set successfully, false otherwise.
   */
  template <typename T>
  [[nodiscard]] bool set_limited_value(const T & value)
  {
    if constexpr (std::is_same_v<T, double>)
    {
      return set_value(on_set_command_limiter_(value, is_command_limited_));
    }
    else
    {
      return set_value(value);
    }
  }

  const bool & is_limited() const { return is_command_limited_; }

  void registerIntrospection() const
  {
    if (value_ptr_ || std::holds_alternative<double>(value_))
    {
      std::function<double()> f = [this]()
      { return value_ptr_ ? *value_ptr_ : std::get<double>(value_); };
      DEFAULT_REGISTER_ROS2_CONTROL_INTROSPECTION("command_interface." + get_name(), f);
      DEFAULT_REGISTER_ROS2_CONTROL_INTROSPECTION(
        "command_interface." + get_name() + ".is_limited", &is_command_limited_);
    }
  }

  void unregisterIntrospection() const
  {
    if (value_ptr_ || std::holds_alternative<double>(value_))
    {
      DEFAULT_UNREGISTER_ROS2_CONTROL_INTROSPECTION("command_interface." + get_name());
      DEFAULT_UNREGISTER_ROS2_CONTROL_INTROSPECTION(
        "command_interface." + get_name() + ".is_limited");
    }
  }

// Disable deprecated warnings
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  using Handle::Handle;
#pragma GCC diagnostic pop

  using SharedPtr = std::shared_ptr<CommandInterface>;

private:
  bool is_command_limited_ = false;
  std::function<double(double, bool &)> on_set_command_limiter_ =
    [](double value, bool & is_limited)
  {
    is_limited = false;
    return value;
  };
};

}  // namespace hardware_interface

#endif  // HARDWARE_INTERFACE__HANDLE_HPP_
