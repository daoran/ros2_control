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

#include "controller_manager/controller_manager.hpp"

#include <fmt/compile.h>

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "controller_interface/controller_interface_base.hpp"
#include "controller_manager_msgs/msg/hardware_component_state.hpp"
#include "hardware_interface/helpers.hpp"
#include "hardware_interface/introspection.hpp"
#include "hardware_interface/types/lifecycle_state_names.hpp"
#include "lifecycle_msgs/msg/state.hpp"
#include "rcl/arguments.h"
#include "rclcpp/version.h"
#include "rclcpp_lifecycle/state.hpp"

#include "controller_manager/controller_manager_parameters.hpp"

namespace  // utility
{
static constexpr const char * kControllerInterfaceNamespace = "controller_interface";
static constexpr const char * kControllerInterfaceClassName =
  "controller_interface::ControllerInterface";
static constexpr const char * kChainableControllerInterfaceClassName =
  "controller_interface::ChainableControllerInterface";

// Changed services history QoS to keep all so we don't lose any client service calls
// \note The versions conditioning is added here to support the source-compatibility with Humble
#if RCLCPP_VERSION_MAJOR >= 17
rclcpp::QoS qos_services =
  rclcpp::QoS(rclcpp::QoSInitialization(RMW_QOS_POLICY_HISTORY_KEEP_ALL, 1))
    .reliable()
    .durability_volatile();
#else
static const rmw_qos_profile_t qos_services = {
  RMW_QOS_POLICY_HISTORY_KEEP_ALL,
  1,  // message queue depth
  RMW_QOS_POLICY_RELIABILITY_RELIABLE,
  RMW_QOS_POLICY_DURABILITY_VOLATILE,
  RMW_QOS_DEADLINE_DEFAULT,
  RMW_QOS_LIFESPAN_DEFAULT,
  RMW_QOS_POLICY_LIVELINESS_SYSTEM_DEFAULT,
  RMW_QOS_LIVELINESS_LEASE_DURATION_DEFAULT,
  false};
#endif

inline bool is_controller_unconfigured(
  const controller_interface::ControllerInterfaceBase & controller)
{
  return controller.get_lifecycle_state().id() ==
         lifecycle_msgs::msg::State::PRIMARY_STATE_UNCONFIGURED;
}

inline bool is_controller_inactive(const controller_interface::ControllerInterfaceBase & controller)
{
  return controller.get_lifecycle_state().id() ==
         lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE;
}

inline bool is_controller_inactive(
  const controller_interface::ControllerInterfaceBaseSharedPtr & controller)
{
  return is_controller_inactive(*controller);
}

inline bool is_controller_active(const controller_interface::ControllerInterfaceBase & controller)
{
  return controller.get_lifecycle_state().id() == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
}

inline bool is_controller_active(
  const controller_interface::ControllerInterfaceBaseSharedPtr & controller)
{
  return is_controller_active(*controller);
}

bool controller_name_compare(const controller_manager::ControllerSpec & a, const std::string & name)
{
  return a.info.name == name;
}

/// Checks if an interface belongs to a controller based on its prefix.
/**
 * A State/Command interface can be provided by a controller in which case is called
 * "state/reference" interface. This means that the @interface_name starts with the name of a
 * controller.
 *
 * \param[in] interface_name to be found in the map.
 * \param[in] controllers list of controllers to compare their names to interface's prefix.
 * \param[out] following_controller_it iterator to the following controller that reference interface
 * @interface_name belongs to.
 * \return true if interface has a controller name as prefix, false otherwise.
 */
bool is_interface_a_chained_interface(
  const std::string & interface_name,
  const std::vector<controller_manager::ControllerSpec> & controllers,
  controller_manager::ControllersListIterator & following_controller_it)
{
  auto split_pos = interface_name.find_first_of('/');
  if (split_pos == std::string::npos)  // '/' exist in the string (should be always false)
  {
    RCLCPP_FATAL(
      rclcpp::get_logger("ControllerManager::utils"),
      "Character '/', was not find in the interface name '%s'. This should never happen. "
      "Stop the controller manager immediately and restart it.",
      interface_name.c_str());
    throw std::runtime_error("Mismatched interface name. See the FATAL message above.");
  }

  auto interface_prefix = interface_name.substr(0, split_pos);
  following_controller_it = std::find_if(
    controllers.begin(), controllers.end(),
    std::bind(controller_name_compare, std::placeholders::_1, interface_prefix));

  RCLCPP_DEBUG(
    rclcpp::get_logger("ControllerManager::utils"),
    "Deduced interface prefix '%s' - searching for the controller with the same name.",
    interface_prefix.c_str());

  if (following_controller_it == controllers.end())
  {
    RCLCPP_DEBUG(
      rclcpp::get_logger("ControllerManager::utils"),
      "Required interface '%s' with prefix '%s' is not a chain interface.", interface_name.c_str(),
      interface_prefix.c_str());

    return false;
  }
  return true;
}

void controller_chain_spec_cleanup(
  std::unordered_map<std::string, controller_manager::ControllerChainSpec> & ctrl_chain_spec,
  const std::string & controller)
{
  const auto following_controllers = ctrl_chain_spec[controller].following_controllers;
  const auto preceding_controllers = ctrl_chain_spec[controller].preceding_controllers;
  for (const auto & flwg_ctrl : following_controllers)
  {
    if (!ros2_control::remove_item(ctrl_chain_spec[flwg_ctrl].preceding_controllers, controller))
    {
      RCLCPP_ERROR(
        rclcpp::get_logger("ControllerManager::utils"),
        "Controller '%s' is not in the list of preceding controllers of '%s'.", controller.c_str(),
        flwg_ctrl.c_str());
    }
  }
  for (const auto & preced_ctrl : preceding_controllers)
  {
    if (ros2_control::remove_item(ctrl_chain_spec[preced_ctrl].following_controllers, controller))
    {
      RCLCPP_ERROR(
        rclcpp::get_logger("ControllerManager::utils"),
        "Controller '%s' is not in the list of following controllers of '%s'.", controller.c_str(),
        preced_ctrl.c_str());
    }
  }
  ctrl_chain_spec.erase(controller);
}

// Gets the list of active controllers that use the command interface of the given controller
void get_active_controllers_using_command_interfaces_of_controller(
  const std::string & controller_name,
  const std::vector<controller_manager::ControllerSpec> & controllers,
  std::vector<std::string> & controllers_using_command_interfaces)
{
  auto it = std::find_if(
    controllers.begin(), controllers.end(),
    std::bind(controller_name_compare, std::placeholders::_1, controller_name));
  if (it == controllers.end())
  {
    RCLCPP_ERROR(
      rclcpp::get_logger("ControllerManager::utils"),
      "Controller '%s' not found in the list of controllers.", controller_name.c_str());
    return;
  }
  const auto cmd_itfs = it->c->command_interface_configuration().names;
  for (const auto & cmd_itf : cmd_itfs)
  {
    for (const auto & controller : controllers)
    {
      const auto ctrl_cmd_itfs = controller.c->command_interface_configuration().names;
      // check if the controller is active and has the command interface and make sure that it
      // doesn't exist in the list already
      if (
        is_controller_active(controller.c) &&
        std::find(ctrl_cmd_itfs.begin(), ctrl_cmd_itfs.end(), cmd_itf) != ctrl_cmd_itfs.end())
      {
        ros2_control::add_item(controllers_using_command_interfaces, controller.info.name);
      }
    }
  }
}

void extract_command_interfaces_for_controller(
  const controller_manager::ControllerSpec & ctrl,
  const std::unique_ptr<hardware_interface::ResourceManager> & resource_manager,
  std::vector<std::string> & request_interface_list)
{
  auto command_interface_config = ctrl.c->command_interface_configuration();
  std::vector<std::string> command_interface_names = {};
  if (command_interface_config.type == controller_interface::interface_configuration_type::ALL)
  {
    command_interface_names = resource_manager->available_command_interfaces();
  }
  if (
    command_interface_config.type == controller_interface::interface_configuration_type::INDIVIDUAL)
  {
    command_interface_names = command_interface_config.names;
  }
  request_interface_list.insert(
    request_interface_list.end(), command_interface_names.begin(), command_interface_names.end());
}

controller_interface::return_type evaluate_switch_result(
  const std::unique_ptr<hardware_interface::ResourceManager> & resource_manager,
  const std::vector<std::string> & activate_list, const std::vector<std::string> & deactivate_list,
  int strictness, rclcpp::Logger logger,
  std::vector<controller_manager::ControllerSpec> & controllers_spec, std::string & message)
{
  message.clear();
  auto switch_result = controller_interface::return_type::OK;
  std::string unable_to_activate_controllers("");
  std::string unable_to_deactivate_controllers("");
  for (auto & controller : controllers_spec)
  {
    if (is_controller_active(controller.c))
    {
      auto command_interface_config = controller.c->command_interface_configuration();
      if (command_interface_config.type == controller_interface::interface_configuration_type::ALL)
      {
        controller.info.claimed_interfaces = resource_manager->available_command_interfaces();
      }
      if (
        command_interface_config.type ==
        controller_interface::interface_configuration_type::INDIVIDUAL)
      {
        controller.info.claimed_interfaces = command_interface_config.names;
      }
    }
    else
    {
      controller.info.claimed_interfaces.clear();
    }
    if (
      std::find(activate_list.begin(), activate_list.end(), controller.info.name) !=
      activate_list.end())
    {
      if (!is_controller_active(controller.c))
      {
        unable_to_activate_controllers += controller.info.name + " ";
        RCLCPP_ERROR(logger, "Could not activate controller : '%s'", controller.info.name.c_str());
        switch_result = controller_interface::return_type::ERROR;
      }
    }
    /// @note The following is the case of the real controllers that are deactivated and doesn't
    /// include the chained controllers that are deactivated and activated
    if (
      std::find(deactivate_list.begin(), deactivate_list.end(), controller.info.name) !=
        deactivate_list.end() &&
      std::find(activate_list.begin(), activate_list.end(), controller.info.name) ==
        activate_list.end())
    {
      if (is_controller_active(controller.c))
      {
        unable_to_deactivate_controllers += controller.info.name + " ";
        RCLCPP_ERROR(
          logger, "Could not deactivate controller : '%s'", controller.info.name.c_str());
        switch_result = controller_interface::return_type::ERROR;
      }
    }
  }
  if (switch_result != controller_interface::return_type::OK)
  {
    message = "Failed switching controllers.... ";
    RCLCPP_ERROR(logger, "%s", message.c_str());
    if (!unable_to_activate_controllers.empty())
    {
      const std::string error_msg = fmt::format(
        FMT_COMPILE("Unable to activate controllers: [ {} ]"), unable_to_activate_controllers);
      message += "\n" + error_msg;
      RCLCPP_ERROR(logger, "%s", error_msg.c_str());
    }
    if (!unable_to_deactivate_controllers.empty())
    {
      const std::string error_msg = fmt::format(
        FMT_COMPILE("Unable to deactivate controllers: [ {} ]"), unable_to_deactivate_controllers);
      message += "\n" + error_msg;
      RCLCPP_ERROR(logger, "%s", error_msg.c_str());
    }
  }
  else
  {
    message = "Successfully switched controllers!";
    if (strictness != controller_manager_msgs::srv::SwitchController::Request::STRICT)
    {
      if (!deactivate_list.empty())
      {
        std::string list = std::accumulate(
          std::next(deactivate_list.begin()), deactivate_list.end(), deactivate_list.front(),
          [](const std::string & a, const std::string & b) { return a + " " + b; });
        const std::string info_msg =
          fmt::format(FMT_COMPILE("Deactivated controllers: [ {} ]"), list);
        message += "\n" + info_msg;
        RCLCPP_INFO(logger, "%s", info_msg.c_str());
      }
      if (!activate_list.empty())
      {
        std::string list = std::accumulate(
          std::next(activate_list.begin()), activate_list.end(), activate_list.front(),
          [](const std::string & a, const std::string & b) { return a + " " + b; });
        const std::string info_msg =
          fmt::format(FMT_COMPILE("Activated controllers: [ {} ]"), list);
        message += "\n" + info_msg;
        RCLCPP_INFO(logger, "%s", info_msg.c_str());
      }
    }
    RCLCPP_INFO(logger, "Successfully switched controllers!");
  }
  return switch_result;
}

void get_controller_list_command_interfaces(
  const std::vector<std::string> & controllers_list,
  const std::vector<controller_manager::ControllerSpec> & controllers_spec,
  const std::unique_ptr<hardware_interface::ResourceManager> & resource_manager,
  std::vector<std::string> & request_interface_list)
{
  for (const auto & controller_name : controllers_list)
  {
    auto found_it = std::find_if(
      controllers_spec.begin(), controllers_spec.end(),
      std::bind(controller_name_compare, std::placeholders::_1, controller_name));
    if (found_it != controllers_spec.end())
    {
      extract_command_interfaces_for_controller(
        *found_it, resource_manager, request_interface_list);
    }
  }
}
}  // namespace

namespace controller_manager
{
rclcpp::NodeOptions get_cm_node_options()
{
  rclcpp::NodeOptions node_options;
  // Required for getting types of controllers to be loaded via service call
  node_options.allow_undeclared_parameters(true);
  node_options.automatically_declare_parameters_from_overrides(true);
// \note The versions conditioning is added here to support the source-compatibility until Humble
#if RCLCPP_VERSION_MAJOR >= 21
  node_options.enable_logger_service(true);
#endif
  return node_options;
}

ControllerManager::ControllerManager(
  std::shared_ptr<rclcpp::Executor> executor, const std::string & manager_node_name,
  const std::string & node_namespace, const rclcpp::NodeOptions & options)
: rclcpp::Node(manager_node_name, node_namespace, options),
  diagnostics_updater_(this),
  executor_(executor),
  loader_(
    std::make_shared<pluginlib::ClassLoader<controller_interface::ControllerInterface>>(
      kControllerInterfaceNamespace, kControllerInterfaceClassName)),
  chainable_loader_(
    std::make_shared<pluginlib::ClassLoader<controller_interface::ChainableControllerInterface>>(
      kControllerInterfaceNamespace, kChainableControllerInterfaceClassName)),
  cm_node_options_(options)
{
  initialize_parameters();
  resource_manager_ =
    std::make_unique<hardware_interface::ResourceManager>(trigger_clock_, this->get_logger());
  init_controller_manager();
}

ControllerManager::ControllerManager(
  std::shared_ptr<rclcpp::Executor> executor, const std::string & urdf,
  bool activate_all_hw_components, const std::string & manager_node_name,
  const std::string & node_namespace, const rclcpp::NodeOptions & options)
: rclcpp::Node(manager_node_name, node_namespace, options),
  diagnostics_updater_(this),
  executor_(executor),
  loader_(
    std::make_shared<pluginlib::ClassLoader<controller_interface::ControllerInterface>>(
      kControllerInterfaceNamespace, kControllerInterfaceClassName)),
  chainable_loader_(
    std::make_shared<pluginlib::ClassLoader<controller_interface::ChainableControllerInterface>>(
      kControllerInterfaceNamespace, kChainableControllerInterfaceClassName)),
  cm_node_options_(options),
  robot_description_(urdf)
{
  initialize_parameters();
  hardware_interface::ResourceManagerParams params;
  params.robot_description = urdf;
  params.clock = trigger_clock_;
  params.logger = this->get_logger();
  params.activate_all = activate_all_hw_components;
  params.update_rate = static_cast<unsigned int>(params_->update_rate);
  params.executor = executor_;
  resource_manager_ = std::make_unique<hardware_interface::ResourceManager>(params, true);
  init_controller_manager();
}

ControllerManager::ControllerManager(
  std::unique_ptr<hardware_interface::ResourceManager> resource_manager,
  std::shared_ptr<rclcpp::Executor> executor, const std::string & manager_node_name,
  const std::string & node_namespace, const rclcpp::NodeOptions & options)
: rclcpp::Node(manager_node_name, node_namespace, options),
  resource_manager_(std::move(resource_manager)),
  diagnostics_updater_(this),
  executor_(executor),
  loader_(
    std::make_shared<pluginlib::ClassLoader<controller_interface::ControllerInterface>>(
      kControllerInterfaceNamespace, kControllerInterfaceClassName)),
  chainable_loader_(
    std::make_shared<pluginlib::ClassLoader<controller_interface::ChainableControllerInterface>>(
      kControllerInterfaceNamespace, kChainableControllerInterfaceClassName)),
  cm_node_options_(options),
  robot_description_(resource_manager_->get_robot_description())
{
  initialize_parameters();
  init_controller_manager();
}

ControllerManager::~ControllerManager()
{
  CLEAR_ALL_ROS2_CONTROL_INTROSPECTION_REGISTRIES();
  if (preshutdown_cb_handle_)
  {
    rclcpp::Context::SharedPtr context = this->get_node_base_interface()->get_context();
    context->remove_pre_shutdown_callback(*(preshutdown_cb_handle_.get()));
    preshutdown_cb_handle_.reset();
  }
}

bool ControllerManager::shutdown_controllers()
{
  RCLCPP_INFO(get_logger(), "Shutting down all controllers in the controller manager.");
  // Shutdown all controllers
  std::lock_guard<std::recursive_mutex> guard(rt_controllers_wrapper_.controllers_lock_);
  std::vector<ControllerSpec> controllers_list = rt_controllers_wrapper_.get_updated_list(guard);
  bool ctrls_shutdown_status = true;
  for (auto & controller : controllers_list)
  {
    if (is_controller_active(controller.c))
    {
      RCLCPP_INFO(
        get_logger(), "Deactivating controller '%s'", controller.c->get_node()->get_name());
      controller.c->get_node()->deactivate();
      controller.c->release_interfaces();
    }
    if (is_controller_inactive(*controller.c) || is_controller_unconfigured(*controller.c))
    {
      RCLCPP_INFO(
        get_logger(), "Shutting down controller '%s'", controller.c->get_node()->get_name());
      shutdown_controller(controller);
    }
    ctrls_shutdown_status &=
      (controller.c->get_node()->get_current_state().id() ==
       lifecycle_msgs::msg::State::PRIMARY_STATE_FINALIZED);
    executor_->remove_node(controller.c->get_node()->get_node_base_interface());
  }
  return ctrls_shutdown_status;
}

void ControllerManager::init_controller_manager()
{
  controller_manager_activity_publisher_ =
    create_publisher<controller_manager_msgs::msg::ControllerManagerActivity>(
      "~/activity", rclcpp::QoS(1).reliable().transient_local());
  rt_controllers_wrapper_.set_on_switch_callback(
    std::bind(&ControllerManager::publish_activity, this));
  resource_manager_->set_on_component_state_switch_callback(
    std::bind(&ControllerManager::publish_activity, this));

  // Get parameters needed for RT "update" loop to work
  if (is_resource_manager_initialized())
  {
    if (params_->enforce_command_limits)
    {
      resource_manager_->import_joint_limiters(robot_description_);
    }
    init_services();
  }
  else
  {
    robot_description_notification_timer_ = create_wall_timer(
      std::chrono::seconds(1),
      [&]()
      {
        RCLCPP_WARN(
          get_logger(), "Waiting for data on 'robot_description' topic to finish initialization");
      });
  }

  // set QoS to transient local to get messages that have already been published
  // (if robot state publisher starts before controller manager)
  robot_description_subscription_ = create_subscription<std_msgs::msg::String>(
    "robot_description", rclcpp::QoS(1).transient_local(),
    std::bind(&ControllerManager::robot_description_callback, this, std::placeholders::_1));
  RCLCPP_INFO(
    get_logger(), "Subscribing to '%s' topic for robot description.",
    robot_description_subscription_->get_topic_name());

  // Setup diagnostics
  periodicity_stats_.Reset();
  diagnostics_updater_.setHardwareID("ros2_control");
  diagnostics_updater_.add(
    "Controllers Activity", this, &ControllerManager::controller_activity_diagnostic_callback);
  diagnostics_updater_.add(
    "Hardware Components Activity", this,
    &ControllerManager::hardware_components_diagnostic_callback);
  diagnostics_updater_.add(
    "Controller Manager Activity", this,
    &ControllerManager::controller_manager_diagnostic_callback);

  INITIALIZE_ROS2_CONTROL_INTROSPECTION_REGISTRY(
    this, hardware_interface::DEFAULT_INTROSPECTION_TOPIC,
    hardware_interface::DEFAULT_REGISTRY_KEY);
  START_ROS2_CONTROL_INTROSPECTION_PUBLISHER_THREAD(hardware_interface::DEFAULT_REGISTRY_KEY);

  // Add on_shutdown callback to stop the controller manager
  rclcpp::Context::SharedPtr context = this->get_node_base_interface()->get_context();
  preshutdown_cb_handle_ =
    std::make_unique<rclcpp::PreShutdownCallbackHandle>(context->add_pre_shutdown_callback(
      [this]()
      {
        RCLCPP_INFO(get_logger(), "Shutdown request received....");
        if (this->get_node_base_interface()->get_associated_with_executor_atomic().load())
        {
          executor_->remove_node(this->get_node_base_interface());
        }
        executor_->cancel();
        if (!this->shutdown_controllers())
        {
          RCLCPP_ERROR(get_logger(), "Failed shutting down the controllers.");
        }
        if (!resource_manager_->shutdown_components())
        {
          RCLCPP_ERROR(get_logger(), "Failed shutting down hardware components.");
        }
        RCLCPP_INFO(get_logger(), "Shutting down the controller manager.");
      }));

  RCLCPP_INFO_EXPRESSION(
    get_logger(), params_->enforce_command_limits, "Enforcing command limits is enabled...");
}

void ControllerManager::initialize_parameters()
{
  // Initialize parameters
  try
  {
    cm_param_listener_ = std::make_shared<controller_manager::ParamListener>(
      this->get_node_parameters_interface(), this->get_logger());
    params_ = std::make_shared<controller_manager::Params>(cm_param_listener_->get_params());
    update_rate_ = static_cast<unsigned int>(params_->update_rate);
    const rclcpp::Parameter use_sim_time = this->get_parameter("use_sim_time");
    trigger_clock_ =
      use_sim_time.as_bool() ? this->get_clock() : std::make_shared<rclcpp::Clock>(RCL_STEADY_TIME);
    RCLCPP_INFO(
      get_logger(), "Using %s clock for triggering controller manager cycles.",
      trigger_clock_->get_clock_type() == RCL_STEADY_TIME ? "Steady (Monotonic)" : "ROS");
  }
  catch (const std::exception & e)
  {
    RCLCPP_ERROR(
      this->get_logger(),
      "Exception thrown while initializing controller manager parameters: %s \n", e.what());
    throw e;
  }
}

void ControllerManager::robot_description_callback(const std_msgs::msg::String & robot_description)
{
  RCLCPP_INFO(get_logger(), "Received robot description from topic.");
  RCLCPP_DEBUG(
    get_logger(), "'Content of robot description file: %s", robot_description.data.c_str());
  robot_description_ = robot_description.data;
  if (is_resource_manager_initialized())
  {
    RCLCPP_WARN(
      get_logger(),
      "ResourceManager has already loaded a urdf. Ignoring attempt to reload a robot description.");
    return;
  }
  init_resource_manager(robot_description_);
  if (is_resource_manager_initialized())
  {
    RCLCPP_INFO(
      get_logger(),
      "Resource Manager has been successfully initialized. Starting Controller Manager "
      "services...");
    init_services();
  }
}

void ControllerManager::init_resource_manager(const std::string & robot_description)
{
  if (params_->enforce_command_limits)
  {
    resource_manager_->import_joint_limiters(robot_description_);
  }
  hardware_interface::ResourceManagerParams params;
  params.robot_description = robot_description;
  params.clock = trigger_clock_;
  params.logger = this->get_logger();
  params.executor = executor_;
  params.update_rate = static_cast<unsigned int>(params_->update_rate);
  if (!resource_manager_->load_and_initialize_components(params))
  {
    RCLCPP_WARN(
      get_logger(),
      "Could not load and initialize hardware. Please check previous output for more details. "
      "After you have corrected your URDF, try to publish robot description again.");
    return;
  }

  // Get all components and if they are not defined in parameters activate them automatically
  auto components_to_activate = resource_manager_->get_components_status();

  using lifecycle_msgs::msg::State;

  auto set_components_to_state =
    [&](const std::vector<std::string> & components_to_set, rclcpp_lifecycle::State state)
  {
    for (const auto & component : components_to_set)
    {
      if (component.empty())
      {
        continue;
      }
      if (components_to_activate.find(component) == components_to_activate.end())
      {
        RCLCPP_WARN(
          get_logger(), "Hardware component '%s' is unknown, therefore not set in '%s' state.",
          component.c_str(), state.label().c_str());
      }
      else
      {
        RCLCPP_INFO(
          get_logger(), "Setting component '%s' to '%s' state.", component.c_str(),
          state.label().c_str());
        if (
          resource_manager_->set_component_state(component, state) ==
          hardware_interface::return_type::ERROR)
        {
          if (params_->hardware_components_initial_state.shutdown_on_initial_state_failure)
          {
            throw std::runtime_error(
              fmt::format(
                FMT_COMPILE("Failed to set the initial state of the component : {} to {}"),
                component.c_str(), state.label()));
          }
          else
          {
            RCLCPP_ERROR(
              get_logger(), "Failed to set the initial state of the component : '%s' to '%s'",
              component.c_str(), state.label().c_str());
          }
        }
        components_to_activate.erase(component);
      }
    }
  };

  if (cm_param_listener_->is_old(*params_))
  {
    *params_ = cm_param_listener_->get_params();
  }

  // unconfigured (loaded only)
  set_components_to_state(
    params_->hardware_components_initial_state.unconfigured,
    rclcpp_lifecycle::State(
      State::PRIMARY_STATE_UNCONFIGURED, hardware_interface::lifecycle_state_names::UNCONFIGURED));

  // inactive (configured)
  set_components_to_state(
    params_->hardware_components_initial_state.inactive,
    rclcpp_lifecycle::State(
      State::PRIMARY_STATE_INACTIVE, hardware_interface::lifecycle_state_names::INACTIVE));

  // activate all other components
  for (const auto & [component, state] : components_to_activate)
  {
    rclcpp_lifecycle::State active_state(
      State::PRIMARY_STATE_ACTIVE, hardware_interface::lifecycle_state_names::ACTIVE);
    if (
      resource_manager_->set_component_state(component, active_state) ==
      hardware_interface::return_type::ERROR)
    {
      if (params_->hardware_components_initial_state.shutdown_on_initial_state_failure)
      {
        throw std::runtime_error(
          fmt::format(
            FMT_COMPILE("Failed to set the initial state of the component : {} to {}"),
            component.c_str(), active_state.label()));
      }
      else
      {
        RCLCPP_ERROR(
          get_logger(), "Failed to set the initial state of the component : '%s' to '%s'",
          component.c_str(), active_state.label().c_str());
      }
    }
  }
  robot_description_notification_timer_->cancel();
}

void ControllerManager::init_services()
{
  // TODO(anyone): Due to issues with the MutliThreadedExecutor, this control loop does not rely on
  // the executor (see issue #260).
  // deterministic_callback_group_ = create_callback_group(
  //   rclcpp::CallbackGroupType::MutuallyExclusive);
  best_effort_callback_group_ = create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);

  using namespace std::placeholders;
  list_controllers_service_ = create_service<controller_manager_msgs::srv::ListControllers>(
    "~/list_controllers", std::bind(&ControllerManager::list_controllers_srv_cb, this, _1, _2),
    qos_services, best_effort_callback_group_);
  list_controller_types_service_ =
    create_service<controller_manager_msgs::srv::ListControllerTypes>(
      "~/list_controller_types",
      std::bind(&ControllerManager::list_controller_types_srv_cb, this, _1, _2), qos_services,
      best_effort_callback_group_);
  load_controller_service_ = create_service<controller_manager_msgs::srv::LoadController>(
    "~/load_controller", std::bind(&ControllerManager::load_controller_service_cb, this, _1, _2),
    qos_services, best_effort_callback_group_);
  configure_controller_service_ = create_service<controller_manager_msgs::srv::ConfigureController>(
    "~/configure_controller",
    std::bind(&ControllerManager::configure_controller_service_cb, this, _1, _2), qos_services,
    best_effort_callback_group_);
  reload_controller_libraries_service_ =
    create_service<controller_manager_msgs::srv::ReloadControllerLibraries>(
      "~/reload_controller_libraries",
      std::bind(&ControllerManager::reload_controller_libraries_service_cb, this, _1, _2),
      qos_services, best_effort_callback_group_);
  switch_controller_service_ = create_service<controller_manager_msgs::srv::SwitchController>(
    "~/switch_controller",
    std::bind(&ControllerManager::switch_controller_service_cb, this, _1, _2), qos_services,
    best_effort_callback_group_);
  unload_controller_service_ = create_service<controller_manager_msgs::srv::UnloadController>(
    "~/unload_controller",
    std::bind(&ControllerManager::unload_controller_service_cb, this, _1, _2), qos_services,
    best_effort_callback_group_);
  list_hardware_components_service_ =
    create_service<controller_manager_msgs::srv::ListHardwareComponents>(
      "~/list_hardware_components",
      std::bind(&ControllerManager::list_hardware_components_srv_cb, this, _1, _2), qos_services,
      best_effort_callback_group_);
  list_hardware_interfaces_service_ =
    create_service<controller_manager_msgs::srv::ListHardwareInterfaces>(
      "~/list_hardware_interfaces",
      std::bind(&ControllerManager::list_hardware_interfaces_srv_cb, this, _1, _2), qos_services,
      best_effort_callback_group_);
  set_hardware_component_state_service_ =
    create_service<controller_manager_msgs::srv::SetHardwareComponentState>(
      "~/set_hardware_component_state",
      std::bind(&ControllerManager::set_hardware_component_state_srv_cb, this, _1, _2),
      qos_services, best_effort_callback_group_);
}

controller_interface::ControllerInterfaceBaseSharedPtr ControllerManager::load_controller(
  const std::string & controller_name, const std::string & controller_type)
{
  RCLCPP_INFO(get_logger(), "Loading controller '%s'", controller_name.c_str());

  if (
    !loader_->isClassAvailable(controller_type) &&
    !chainable_loader_->isClassAvailable(controller_type))
  {
    RCLCPP_ERROR(
      get_logger(), "Loader for controller '%s' (type '%s') not found.", controller_name.c_str(),
      controller_type.c_str());
    RCLCPP_INFO(get_logger(), "Available classes:");
    for (const auto & available_class : loader_->getDeclaredClasses())
    {
      RCLCPP_INFO(get_logger(), "  %s", available_class.c_str());
    }
    for (const auto & available_class : chainable_loader_->getDeclaredClasses())
    {
      RCLCPP_INFO(get_logger(), "  %s", available_class.c_str());
    }
    return nullptr;
  }
  RCLCPP_DEBUG(get_logger(), "Loader for controller '%s' found.", controller_name.c_str());

  controller_interface::ControllerInterfaceBaseSharedPtr controller;

  try
  {
    if (loader_->isClassAvailable(controller_type))
    {
      controller = loader_->createSharedInstance(controller_type);
    }
    if (chainable_loader_->isClassAvailable(controller_type))
    {
      controller = chainable_loader_->createSharedInstance(controller_type);
    }
  }
  catch (const std::exception & e)
  {
    RCLCPP_ERROR(
      get_logger(),
      "Caught exception of type : %s while loading the controller '%s' of plugin type '%s':\n%s",
      typeid(e).name(), controller_name.c_str(), controller_type.c_str(), e.what());
    return nullptr;
  }
  catch (...)
  {
    RCLCPP_ERROR(
      get_logger(),
      "Caught unknown exception while loading the controller '%s' of plugin type '%s'",
      controller_name.c_str(), controller_type.c_str());
    throw;
  }

  ControllerSpec controller_spec;
  controller_spec.c = controller;
  controller_spec.info.name = controller_name;
  controller_spec.info.type = controller_type;
  controller_spec.last_update_cycle_time =
    std::make_shared<rclcpp::Time>(0, 0, this->get_trigger_clock()->get_clock_type());
  controller_spec.execution_time_statistics = std::make_shared<MovingAverageStatistics>();
  controller_spec.periodicity_statistics = std::make_shared<MovingAverageStatistics>();

  // We have to fetch the parameters_file at the time of loading the controller, because this way we
  // can load them at the creation of the LifeCycleNode and this helps in using the features such as
  // read_only params, dynamic maps lists etc
  // Now check if the parameters_file parameter exist
  const std::string param_name = fmt::format(FMT_COMPILE("{}.params_file"), controller_name);
  controller_spec.info.parameters_files.clear();

  // get_parameter checks if parameter has been declared/set
  rclcpp::Parameter params_files_parameter;
  if (get_parameter(param_name, params_files_parameter))
  {
    if (params_files_parameter.get_type() == rclcpp::ParameterType::PARAMETER_STRING_ARRAY)
    {
      controller_spec.info.parameters_files = params_files_parameter.as_string_array();
    }
    else if (params_files_parameter.get_type() == rclcpp::ParameterType::PARAMETER_STRING)
    {
      controller_spec.info.parameters_files.push_back(params_files_parameter.as_string());
    }
    else
    {
      RCLCPP_ERROR(
        get_logger(),
        "The 'params_file' param needs to be a string or a string array for '%s', but it is of "
        "type %s",
        controller_name.c_str(), params_files_parameter.get_type_name().c_str());
    }
  }

  const std::string fallback_ctrl_param =
    fmt::format(FMT_COMPILE("{}.fallback_controllers"), controller_name);
  std::vector<std::string> fallback_controllers;
  if (!has_parameter(fallback_ctrl_param))
  {
    declare_parameter(fallback_ctrl_param, rclcpp::ParameterType::PARAMETER_STRING_ARRAY);
  }
  if (get_parameter(fallback_ctrl_param, fallback_controllers) && !fallback_controllers.empty())
  {
    if (
      std::find(fallback_controllers.begin(), fallback_controllers.end(), controller_name) !=
      fallback_controllers.end())
    {
      RCLCPP_ERROR(
        get_logger(), "Controller '%s' cannot be a fallback controller for itself.",
        controller_name.c_str());
      return nullptr;
    }
    controller_spec.info.fallback_controllers_names = fallback_controllers;
  }

  const std::string node_options_args_param =
    fmt::format(FMT_COMPILE("{}.node_options_args"), controller_name);
  std::vector<std::string> node_options_args;
  if (!has_parameter(node_options_args_param))
  {
    declare_parameter(node_options_args_param, rclcpp::ParameterType::PARAMETER_STRING_ARRAY);
  }
  if (get_parameter(node_options_args_param, node_options_args) && !node_options_args.empty())
  {
    controller_spec.info.node_options_args = node_options_args;
  }

  return add_controller_impl(controller_spec);
}

controller_interface::ControllerInterfaceBaseSharedPtr ControllerManager::load_controller(
  const std::string & controller_name)
{
  const std::string param_name = fmt::format(FMT_COMPILE("{}.type"), controller_name);
  std::string controller_type;

  // We cannot declare the parameters for the controllers that will be loaded in the future,
  // because they are plugins and we cannot be aware of all of them.
  // So when we're told to load a controller by name, we need to declare the parameter if
  // we haven't done so, and then read it.

  // Check if parameter has been declared
  if (!has_parameter(param_name))
  {
    declare_parameter(param_name, rclcpp::ParameterType::PARAMETER_STRING);
  }
  if (!get_parameter(param_name, controller_type))
  {
    RCLCPP_ERROR(
      get_logger(), "The 'type' param was not defined for '%s'.", controller_name.c_str());
    return nullptr;
  }
  RCLCPP_INFO(
    get_logger(), "Loading controller : '%s' of type '%s'", controller_name.c_str(),
    controller_type.c_str());
  return load_controller(controller_name, controller_type);
}

controller_interface::return_type ControllerManager::unload_controller(
  const std::string & controller_name)
{
  RCLCPP_INFO(get_logger(), "Unloading controller: '%s'", controller_name.c_str());
  std::lock_guard<std::recursive_mutex> guard(rt_controllers_wrapper_.controllers_lock_);
  std::vector<ControllerSpec> & to = rt_controllers_wrapper_.get_unused_list(guard);
  const std::vector<ControllerSpec> & from = rt_controllers_wrapper_.get_updated_list(guard);

  // Transfers the active controllers over, skipping the one to be removed and the active ones.
  to = from;

  auto found_it = std::find_if(
    to.begin(), to.end(),
    std::bind(controller_name_compare, std::placeholders::_1, controller_name));
  if (found_it == to.end())
  {
    // Fails if we could not remove the controllers
    to.clear();
    RCLCPP_ERROR(
      get_logger(),
      "Could not unload controller with name '%s' because no controller with this name exists",
      controller_name.c_str());
    return controller_interface::return_type::ERROR;
  }

  auto & controller = *found_it;

  if (is_controller_active(*controller.c))
  {
    to.clear();
    RCLCPP_ERROR(
      get_logger(), "Could not unload controller with name '%s' because it is still active",
      controller_name.c_str());
    return controller_interface::return_type::ERROR;
  }

  RCLCPP_DEBUG(get_logger(), "Shutdown controller");
  controller_chain_spec_cleanup(controller_chain_spec_, controller_name);
  cleanup_controller_exported_interfaces(controller);
  if (is_controller_inactive(*controller.c) || is_controller_unconfigured(*controller.c))
  {
    RCLCPP_DEBUG(
      get_logger(), "Controller '%s' is shutdown before unloading!", controller_name.c_str());
    shutdown_controller(controller);
  }
  executor_->remove_node(controller.c->get_node()->get_node_base_interface());
  to.erase(found_it);

  // Destroys the old controllers list when the realtime thread is finished with it.
  RCLCPP_DEBUG(get_logger(), "Realtime switches over to new controller list");
  rt_controllers_wrapper_.switch_updated_list(guard);
  std::vector<ControllerSpec> & new_unused_list = rt_controllers_wrapper_.get_unused_list(guard);
  RCLCPP_DEBUG(get_logger(), "Destruct controller");
  new_unused_list.clear();
  RCLCPP_DEBUG(get_logger(), "Destruct controller finished");

  RCLCPP_DEBUG(get_logger(), "Successfully unloaded controller '%s'", controller_name.c_str());

  return controller_interface::return_type::OK;
}

controller_interface::return_type ControllerManager::cleanup_controller(
  const controller_manager::ControllerSpec & controller)
{
  try
  {
    cleanup_controller_exported_interfaces(controller);
    const auto new_state = controller.c->get_node()->cleanup();
    if (new_state.id() != lifecycle_msgs::msg::State::PRIMARY_STATE_UNCONFIGURED)
    {
      RCLCPP_ERROR(
        get_logger(), "Controller '%s' is not cleaned-up properly, it is still in state '%s'",
        controller.info.name.c_str(), new_state.label().c_str());
      return controller_interface::return_type::ERROR;
    }
  }
  catch (...)
  {
    RCLCPP_ERROR(
      get_logger(), "Caught exception while cleaning-up the controller '%s'",
      controller.info.name.c_str());
    return controller_interface::return_type::ERROR;
  }
  return controller_interface::return_type::OK;
}

void ControllerManager::shutdown_controller(
  const controller_manager::ControllerSpec & controller) const
{
  try
  {
    const auto new_state = controller.c->get_node()->shutdown();
    if (new_state.id() != lifecycle_msgs::msg::State::PRIMARY_STATE_FINALIZED)
    {
      RCLCPP_WARN(
        get_logger(), "Failed to shutdown the controller '%s' before unloading!",
        controller.info.name.c_str());
    }
  }
  catch (const std::exception & e)
  {
    RCLCPP_ERROR(
      get_logger(),
      "Caught exception of type : %s while shutdown the controller '%s' before unloading: %s",
      typeid(e).name(), controller.info.name.c_str(), e.what());
  }
  catch (...)
  {
    RCLCPP_ERROR(
      get_logger(), "Failed to shutdown the controller '%s' before unloading",
      controller.info.name.c_str());
  }
}

std::vector<ControllerSpec> ControllerManager::get_loaded_controllers() const
{
  std::lock_guard<std::recursive_mutex> guard(rt_controllers_wrapper_.controllers_lock_);
  return rt_controllers_wrapper_.get_updated_list(guard);
}

controller_interface::return_type ControllerManager::configure_controller(
  const std::string & controller_name)
{
  RCLCPP_INFO(get_logger(), "Configuring controller: '%s'", controller_name.c_str());

  const auto & controllers = get_loaded_controllers();

  auto found_it = std::find_if(
    controllers.begin(), controllers.end(),
    std::bind(controller_name_compare, std::placeholders::_1, controller_name));

  if (found_it == controllers.end())
  {
    RCLCPP_ERROR(
      get_logger(),
      "Could not configure controller with name '%s' because no controller with this name exists",
      controller_name.c_str());
    return controller_interface::return_type::ERROR;
  }
  auto controller = found_it->c;

  auto state = controller->get_lifecycle_state();
  if (
    state.id() == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE ||
    state.id() == lifecycle_msgs::msg::State::PRIMARY_STATE_FINALIZED)
  {
    RCLCPP_ERROR(
      get_logger(), "Controller '%s' can not be configured from '%s' state.",
      controller_name.c_str(), state.label().c_str());
    return controller_interface::return_type::ERROR;
  }

  auto new_state = controller->get_lifecycle_state();
  if (state.id() == lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE)
  {
    RCLCPP_DEBUG(
      get_logger(), "Controller '%s' is cleaned-up before configuring", controller_name.c_str());
    if (cleanup_controller(*found_it) != controller_interface::return_type::OK)
    {
      return controller_interface::return_type::ERROR;
    }
  }

  try
  {
    new_state = controller->configure();
    if (new_state.id() != lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE)
    {
      RCLCPP_ERROR(
        get_logger(), "After configuring, controller '%s' is in state '%s' , expected inactive.",
        controller_name.c_str(), new_state.label().c_str());
      return controller_interface::return_type::ERROR;
    }
  }
  catch (const std::exception & e)
  {
    RCLCPP_ERROR(
      get_logger(), "Caught exception of type : %s while configuring controller '%s': %s",
      typeid(e).name(), controller_name.c_str(), e.what());
    return controller_interface::return_type::ERROR;
  }
  catch (...)
  {
    RCLCPP_ERROR(
      get_logger(), "Caught unknown exception while configuring controller '%s'",
      controller_name.c_str());
    return controller_interface::return_type::ERROR;
  }

  const auto controller_update_rate = controller->get_update_rate();
  const auto cm_update_rate = get_update_rate();
  if (controller_update_rate > cm_update_rate)
  {
    RCLCPP_WARN(
      get_logger(),
      "The controller : %s update rate : %d Hz should be less than or equal to controller "
      "manager's update rate : %d Hz!. The controller will be updated at controller_manager's "
      "update rate.",
      controller_name.c_str(), controller_update_rate, cm_update_rate);
  }
  else if (cm_update_rate % controller_update_rate != 0)
  {
    RCLCPP_WARN(
      get_logger(),
      "The controller : %s update cycles won't be triggered at a constant period : %f sec, as the "
      "controller's update rate : %d Hz is not a perfect divisor of the controller manager's "
      "update rate : %d Hz!.",
      controller_name.c_str(), 1.0 / controller_update_rate, controller_update_rate,
      cm_update_rate);
  }

  // CHAINABLE CONTROLLERS: get reference interfaces from chainable controllers
  if (controller->is_chainable())
  {
    RCLCPP_DEBUG(
      get_logger(),
      "Controller '%s' is chainable. Interfaces are being exported to resource manager.",
      controller_name.c_str());
    std::vector<hardware_interface::StateInterface::ConstSharedPtr> state_interfaces;
    std::vector<hardware_interface::CommandInterface::SharedPtr> ref_interfaces;
    try
    {
      state_interfaces = controller->export_state_interfaces();
      ref_interfaces = controller->export_reference_interfaces();
      if (ref_interfaces.empty() && state_interfaces.empty())
      {
        // TODO(destogl): Add test for this!
        RCLCPP_ERROR(
          get_logger(),
          "Controller '%s' is chainable, but does not export any state or reference interfaces. "
          "Did you override the on_export_method() correctly?",
          controller_name.c_str());
        return controller_interface::return_type::ERROR;
      }
    }
    catch (const std::exception & e)
    {
      RCLCPP_FATAL(
        get_logger(), "Export of the state or reference interfaces failed with following error: %s",
        e.what());
      return controller_interface::return_type::ERROR;
    }
    resource_manager_->import_controller_reference_interfaces(controller_name, ref_interfaces);
    resource_manager_->import_controller_exported_state_interfaces(
      controller_name, state_interfaces);
  }

  // let's update the list of following and preceding controllers
  const auto cmd_itfs = controller->command_interface_configuration().names;
  const auto state_itfs = controller->state_interface_configuration().names;

  // Check if the cmd_itfs and the state_itfs are unique
  if (!ros2_control::is_unique(cmd_itfs))
  {
    std::string cmd_itfs_str = std::accumulate(
      std::next(cmd_itfs.begin()), cmd_itfs.end(), cmd_itfs.front(),
      [](const std::string & a, const std::string & b) { return a + ", " + b; });
    RCLCPP_ERROR(
      get_logger(),
      "The command interfaces of the controller '%s' are not unique. Please make sure that the "
      "command interfaces are unique : '%s'.",
      controller_name.c_str(), cmd_itfs_str.c_str());
    cleanup_controller(*found_it);
    return controller_interface::return_type::ERROR;
  }

  if (!ros2_control::is_unique(state_itfs))
  {
    std::string state_itfs_str = std::accumulate(
      std::next(state_itfs.begin()), state_itfs.end(), state_itfs.front(),
      [](const std::string & a, const std::string & b) { return a + ", " + b; });
    RCLCPP_ERROR(
      get_logger(),
      "The state interfaces of the controller '%s' are not unique. Please make sure that the state "
      "interfaces are unique : '%s'.",
      controller_name.c_str(), state_itfs_str.c_str());
    cleanup_controller(*found_it);
    return controller_interface::return_type::ERROR;
  }

  for (const auto & cmd_itf : cmd_itfs)
  {
    controller_manager::ControllersListIterator ctrl_it;
    if (is_interface_a_chained_interface(cmd_itf, controllers, ctrl_it))
    {
      ros2_control::add_item(
        controller_chain_spec_[controller_name].following_controllers, ctrl_it->info.name);
      ros2_control::add_item(
        controller_chain_spec_[ctrl_it->info.name].preceding_controllers, controller_name);
      ros2_control::add_item(
        controller_chained_reference_interfaces_cache_[ctrl_it->info.name], controller_name);
    }
  }
  // This is needed when we start exporting the state interfaces from the controllers
  for (const auto & state_itf : state_itfs)
  {
    controller_manager::ControllersListIterator ctrl_it;
    if (is_interface_a_chained_interface(state_itf, controllers, ctrl_it))
    {
      ros2_control::add_item(
        controller_chain_spec_[controller_name].preceding_controllers, ctrl_it->info.name);
      ros2_control::add_item(
        controller_chain_spec_[ctrl_it->info.name].following_controllers, controller_name);
      ros2_control::add_item(
        controller_chained_state_interfaces_cache_[ctrl_it->info.name], controller_name);
    }
  }

  // Now let's reorder the controllers
  // lock controllers
  std::lock_guard<std::recursive_mutex> guard(rt_controllers_wrapper_.controllers_lock_);
  std::vector<ControllerSpec> & to = rt_controllers_wrapper_.get_unused_list(guard);
  const std::vector<ControllerSpec> & from = rt_controllers_wrapper_.get_updated_list(guard);

  // Copy all controllers from the 'from' list to the 'to' list
  to = from;
  std::vector<ControllerSpec> sorted_list;

  // clear the list before reordering it again
  ordered_controllers_names_.clear();
  for (const auto & [ctrl_name, chain_spec] : controller_chain_spec_)
  {
    auto it =
      std::find(ordered_controllers_names_.begin(), ordered_controllers_names_.end(), ctrl_name);
    if (it == ordered_controllers_names_.end())
    {
      update_list_with_controller_chain(ctrl_name, ordered_controllers_names_.end(), false);
    }
  }

  std::vector<ControllerSpec> new_list;
  for (const auto & ctrl : ordered_controllers_names_)
  {
    auto controller_it = std::find_if(
      to.begin(), to.end(), std::bind(controller_name_compare, std::placeholders::_1, ctrl));
    if (controller_it != to.end())
    {
      new_list.push_back(*controller_it);
    }
  }

  to = new_list;
  RCLCPP_DEBUG(get_logger(), "Reordered controllers list is:");
  for (const auto & ctrl : to)
  {
    RCLCPP_DEBUG(this->get_logger(), "\t%s", ctrl.info.name.c_str());
  }

  // switch lists
  rt_controllers_wrapper_.switch_updated_list(guard);
  // clear unused list
  rt_controllers_wrapper_.get_unused_list(guard).clear();

  return controller_interface::return_type::OK;
}

void ControllerManager::clear_requests()
{
  switch_params_.do_switch = false;
  deactivate_request_.clear();
  activate_request_.clear();
  // Set these interfaces as unavailable when clearing requests to avoid leaving them in available
  // state without the controller being in active state
  for (const auto & controller_name : to_chained_mode_request_)
  {
    resource_manager_->make_controller_exported_state_interfaces_unavailable(controller_name);
    resource_manager_->make_controller_reference_interfaces_unavailable(controller_name);
  }
  to_chained_mode_request_.clear();
  from_chained_mode_request_.clear();
  activate_command_interface_request_.clear();
  deactivate_command_interface_request_.clear();
}

controller_interface::return_type ControllerManager::switch_controller(
  const std::vector<std::string> & activate_controllers,
  const std::vector<std::string> & deactivate_controllers, int strictness, bool activate_asap,
  const rclcpp::Duration & timeout)
{
  std::string message;
  return switch_controller_cb(
    activate_controllers, deactivate_controllers, strictness, activate_asap, timeout, message);
}

controller_interface::return_type ControllerManager::switch_controller_cb(
  const std::vector<std::string> & activate_controllers,
  const std::vector<std::string> & deactivate_controllers, int strictness, bool activate_asap,
  const rclcpp::Duration & timeout, std::string & message)
{
  if (!is_resource_manager_initialized())
  {
    message =
      "Resource Manager is not initialized yet! Please provide robot description on "
      "'robot_description' topic before trying to switch controllers.";
    RCLCPP_ERROR(get_logger(), "%s", message.c_str());
    return controller_interface::return_type::ERROR;
  }

  // reset the switch param internal variables
  switch_params_.reset();

  if (!deactivate_request_.empty() || !activate_request_.empty())
  {
    RCLCPP_FATAL(
      get_logger(),
      "The internal deactivate and activate request lists are not empty at the beginning of the "
      "switch_controller() call. This should never happen.");
    throw std::runtime_error("CM's internal state is not correct. See the FATAL message above.");
  }
  if (
    !deactivate_command_interface_request_.empty() || !activate_command_interface_request_.empty())
  {
    RCLCPP_FATAL(
      get_logger(),
      "The internal deactivate and activat requests command interface lists are not empty at the "
      "switch_controller() call. This should never happen.");
    throw std::runtime_error("CM's internal state is not correct. See the FATAL message above.");
  }
  if (!from_chained_mode_request_.empty() || !to_chained_mode_request_.empty())
  {
    RCLCPP_FATAL(
      get_logger(),
      "The internal 'from' and 'to' chained mode requests are not empty at the "
      "switch_controller() call. This should never happen.");
    throw std::runtime_error("CM's internal state is not correct. See the FATAL message above.");
  }
  if (strictness == 0)
  {
    std::string default_strictness = params_->defaults.switch_controller.strictness;
    // Convert to uppercase
    std::transform(
      default_strictness.begin(), default_strictness.end(), default_strictness.begin(),
      [](unsigned char c) { return std::toupper(c); });
    RCLCPP_WARN_ONCE(
      get_logger(),
      "Controller Manager: to switch controllers you need to specify a "
      "strictness level of controller_manager_msgs::SwitchController::STRICT "
      "(%d) or ::BEST_EFFORT (%d). When unspecified, the default is %s",
      controller_manager_msgs::srv::SwitchController::Request::STRICT,
      controller_manager_msgs::srv::SwitchController::Request::BEST_EFFORT,
      default_strictness.c_str());
    strictness = params_->defaults.switch_controller.strictness == "strict"
                   ? controller_manager_msgs::srv::SwitchController::Request::STRICT
                   : controller_manager_msgs::srv::SwitchController::Request::BEST_EFFORT;
  }
  else if (strictness == controller_manager_msgs::srv::SwitchController::Request::AUTO)
  {
    RCLCPP_WARN(
      get_logger(),
      "Controller Manager: AUTO is not currently implemented. "
      "Defaulting to BEST_EFFORT");
    strictness = controller_manager_msgs::srv::SwitchController::Request::BEST_EFFORT;
  }
  else if (strictness == controller_manager_msgs::srv::SwitchController::Request::FORCE_AUTO)
  {
    RCLCPP_DEBUG(
      get_logger(),
      "Controller Manager: FORCE_AUTO is not currently implemented. "
      "Defaulting to BEST_EFFORT");
    strictness = controller_manager_msgs::srv::SwitchController::Request::BEST_EFFORT;
  }

  std::string activate_list, deactivate_list;
  activate_list.reserve(500);
  deactivate_list.reserve(500);
  for (const auto & controller : activate_controllers)
  {
    activate_list.append(controller);
    activate_list.append(" ");
  }
  for (const auto & controller : deactivate_controllers)
  {
    deactivate_list.append(controller);
    deactivate_list.append(" ");
  }
  RCLCPP_INFO_EXPRESSION(
    get_logger(), !activate_list.empty(), "Activating controllers: [ %s]", activate_list.c_str());
  RCLCPP_INFO_EXPRESSION(
    get_logger(), !deactivate_list.empty(), "Deactivating controllers: [ %s]",
    deactivate_list.c_str());

  const auto list_controllers =
    [this, strictness](
      const std::vector<std::string> & controller_list, std::vector<std::string> & request_list,
      const std::string & action, std::string & msg) -> controller_interface::return_type
  {
    // lock controllers
    std::lock_guard<std::recursive_mutex> guard(rt_controllers_wrapper_.controllers_lock_);
    auto result = controller_interface::return_type::OK;

    // list all controllers to (de)activate
    for (const auto & controller : controller_list)
    {
      const auto & updated_controllers = rt_controllers_wrapper_.get_updated_list(guard);

      auto found_it = std::find_if(
        updated_controllers.begin(), updated_controllers.end(),
        std::bind(controller_name_compare, std::placeholders::_1, controller));

      if (found_it == updated_controllers.end())
      {
        const std::string error_msg = fmt::format(
          FMT_COMPILE(
            "Could not {} controller with name '{}' because no controller with this name exists"),
          action, controller);
        msg += error_msg + "\n";
        RCLCPP_WARN(get_logger(), "%s", error_msg.c_str());
        // For the BEST_EFFORT switch, if there are more controllers that are in the list, this is
        // not a critical error
        result = request_list.empty() ? controller_interface::return_type::ERROR
                                      : controller_interface::return_type::OK;
        if (strictness == controller_manager_msgs::srv::SwitchController::Request::STRICT)
        {
          msg = error_msg;
          RCLCPP_ERROR(get_logger(), "Aborting, no controller is switched! ('STRICT' switch)");
          return controller_interface::return_type::ERROR;
        }
      }
      else
      {
        result = controller_interface::return_type::OK;
        RCLCPP_DEBUG(
          get_logger(), "Found controller '%s' that needs to be %sed in list of controllers",
          controller.c_str(), action.c_str());
        request_list.push_back(controller);
      }
    }
    RCLCPP_DEBUG(
      get_logger(), "'%s' request vector has size %i", action.c_str(), (int)request_list.size());

    return result;
  };

  // list all controllers to deactivate (check if all controllers exist)
  auto ret = list_controllers(deactivate_controllers, deactivate_request_, "deactivate", message);
  if (ret != controller_interface::return_type::OK)
  {
    deactivate_request_.clear();
    return ret;
  }

  // list all controllers to activate (check if all controllers exist)
  ret = list_controllers(activate_controllers, activate_request_, "activate", message);
  if (ret != controller_interface::return_type::OK)
  {
    deactivate_request_.clear();
    activate_request_.clear();
    return ret;
  }
  // If it is a best effort switch, we can remove the controllers log that could not be activated
  message.clear();

  // lock controllers
  std::lock_guard<std::recursive_mutex> guard(rt_controllers_wrapper_.controllers_lock_);

  const std::vector<ControllerSpec> & controllers = rt_controllers_wrapper_.get_updated_list(guard);

  // if a preceding controller is deactivated, all first-level controllers should be switched 'from'
  // chained mode
  propagate_deactivation_of_chained_mode(controllers);

  // check if controllers should be switched 'to' chained mode when controllers are activated
  for (auto ctrl_it = activate_request_.begin(); ctrl_it != activate_request_.end(); ++ctrl_it)
  {
    auto controller_it = std::find_if(
      controllers.begin(), controllers.end(),
      std::bind(controller_name_compare, std::placeholders::_1, *ctrl_it));
    controller_interface::return_type status = controller_interface::return_type::OK;

    // if controller is not inactive then do not do any following-controllers checks
    if (is_controller_unconfigured(*controller_it->c))
    {
      message = fmt::format(
        FMT_COMPILE(
          "Controller with name '{}' is in 'unconfigured' state. The controller needs to be "
          "configured to be in 'inactive' state before it can be checked and activated."),
        controller_it->info.name);
      RCLCPP_WARN(get_logger(), "%s", message.c_str());
      status = controller_interface::return_type::ERROR;
    }
    else if (is_controller_active(controller_it->c))
    {
      if (
        std::find(
          deactivate_request_.begin(), deactivate_request_.end(), controller_it->info.name) ==
        deactivate_request_.end())
      {
        message = fmt::format(
          FMT_COMPILE("Controller with name '{}' is already active."), controller_it->info.name);
        RCLCPP_WARN(get_logger(), "%s", message.c_str());
        RCLCPP_WARN(get_logger(), "%s", message.c_str());
        status = controller_interface::return_type::ERROR;
      }
    }
    else if (!is_controller_inactive(controller_it->c))
    {
      message = fmt::format(
        FMT_COMPILE(
          "Controller with name '{}' is not in 'inactive' state. The controller needs to be in "
          "'inactive' state before it can be checked and activated."),
        controller_it->info.name);
      RCLCPP_WARN(get_logger(), "%s", message.c_str());
      status = controller_interface::return_type::ERROR;
    }
    else
    {
      status =
        check_following_controllers_for_activate(controllers, strictness, controller_it, message);
    }

    if (status == controller_interface::return_type::OK)
    {
      status = check_fallback_controllers_state_pre_activation(controllers, controller_it, message);
    }

    if (status != controller_interface::return_type::OK)
    {
      RCLCPP_WARN(
        get_logger(),
        "Could not activate controller with name '%s'. Check above warnings for more details. "
        "Check the state of the controllers and their required interfaces using "
        "`ros2 control list_controllers -v` CLI to get more information.",
        (*ctrl_it).c_str());
      if (strictness == controller_manager_msgs::srv::SwitchController::Request::BEST_EFFORT)
      {
        // TODO(destogl): automatic manipulation of the chain:
        // || strictness ==
        //  controller_manager_msgs::srv::SwitchController::Request::MANIPULATE_CONTROLLERS_CHAIN);
        // remove controller that can not be activated from the activation request and step-back
        // iterator to correctly step to the next element in the list in the loop
        activate_request_.erase(ctrl_it);
        message.clear();
        --ctrl_it;
      }
      if (strictness == controller_manager_msgs::srv::SwitchController::Request::STRICT)
      {
        RCLCPP_ERROR(get_logger(), "Aborting, no controller is switched! (::STRICT switch)");
        // reset all lists
        clear_requests();
        return controller_interface::return_type::ERROR;
      }
    }
  }

  // check if controllers should be deactivated if used in chained mode
  for (auto ctrl_it = deactivate_request_.begin(); ctrl_it != deactivate_request_.end(); ++ctrl_it)
  {
    auto controller_it = std::find_if(
      controllers.begin(), controllers.end(),
      std::bind(controller_name_compare, std::placeholders::_1, *ctrl_it));
    controller_interface::return_type status = controller_interface::return_type::OK;

    // if controller is not active then skip preceding-controllers checks
    if (!is_controller_active(controller_it->c))
    {
      message = fmt::format(
        FMT_COMPILE("Controller with name '{}' can not be deactivated since it is not active."),
        controller_it->info.name);
      RCLCPP_WARN(get_logger(), "%s", message.c_str());
      status = controller_interface::return_type::ERROR;
    }
    else
    {
      status =
        check_preceding_controllers_for_deactivate(controllers, strictness, controller_it, message);
    }

    if (status != controller_interface::return_type::OK)
    {
      RCLCPP_WARN(
        get_logger(),
        "Could not deactivate controller with name '%s'. Check above warnings for more details. "
        "Check the state of the controllers and their required interfaces using "
        "`ros2 control list_controllers -v` CLI to get more information.",
        (*ctrl_it).c_str());
      if (strictness == controller_manager_msgs::srv::SwitchController::Request::BEST_EFFORT)
      {
        // remove controller that can not be activated from the activation request and step-back
        // iterator to correctly step to the next element in the list in the loop
        deactivate_request_.erase(ctrl_it);
        message.clear();
        --ctrl_it;
      }
      if (strictness == controller_manager_msgs::srv::SwitchController::Request::STRICT)
      {
        RCLCPP_ERROR(get_logger(), "Aborting, no controller is switched! (::STRICT switch)");
        // reset all lists
        clear_requests();
        return controller_interface::return_type::ERROR;
      }
    }
  }

  // Check after the check if the activate and deactivate list is empty or not
  if (activate_request_.empty() && deactivate_request_.empty())
  {
    message = "After checking the controllers, no controllers need to be activated or deactivated.";
    RCLCPP_INFO(get_logger(), "%s", message.c_str());
    clear_requests();
    return controller_interface::return_type::OK;
  }
  message.clear();

  for (const auto & controller : controllers)
  {
    auto to_chained_mode_list_it = std::find(
      to_chained_mode_request_.begin(), to_chained_mode_request_.end(), controller.info.name);
    bool in_to_chained_mode_list = to_chained_mode_list_it != to_chained_mode_request_.end();

    auto from_chained_mode_list_it = std::find(
      from_chained_mode_request_.begin(), from_chained_mode_request_.end(), controller.info.name);
    bool in_from_chained_mode_list = from_chained_mode_list_it != from_chained_mode_request_.end();

    auto deactivate_list_it =
      std::find(deactivate_request_.begin(), deactivate_request_.end(), controller.info.name);
    bool in_deactivate_list = deactivate_list_it != deactivate_request_.end();

    const bool is_active = is_controller_active(*controller.c);
    const bool is_inactive = is_controller_inactive(*controller.c);

    // restart controllers that need to switch their 'chained mode' - add to (de)activate lists
    if (in_to_chained_mode_list || in_from_chained_mode_list)
    {
      if (is_active && !in_deactivate_list)
      {
        deactivate_request_.push_back(controller.info.name);
        activate_request_.push_back(controller.info.name);
      }
    }

    // get pointers to places in deactivate and activate lists ((de)activate lists have changed)
    deactivate_list_it =
      std::find(deactivate_request_.begin(), deactivate_request_.end(), controller.info.name);
    in_deactivate_list = deactivate_list_it != deactivate_request_.end();

    auto activate_list_it =
      std::find(activate_request_.begin(), activate_request_.end(), controller.info.name);
    bool in_activate_list = activate_list_it != activate_request_.end();

    auto handle_conflict = [&](const std::string & msg)
    {
      if (strictness == controller_manager_msgs::srv::SwitchController::Request::STRICT)
      {
        message = msg;
        RCLCPP_ERROR(get_logger(), "%s", msg.c_str());
        deactivate_request_.clear();
        deactivate_command_interface_request_.clear();
        activate_request_.clear();
        activate_command_interface_request_.clear();
        to_chained_mode_request_.clear();
        from_chained_mode_request_.clear();
        return controller_interface::return_type::ERROR;
      }
      RCLCPP_WARN(get_logger(), "%s", msg.c_str());
      return controller_interface::return_type::OK;
    };

    // check for double stop
    if (!is_active && in_deactivate_list)
    {
      auto conflict_status = handle_conflict(
        "Could not deactivate controller '" + controller.info.name + "' since it is not active");
      if (conflict_status != controller_interface::return_type::OK)
      {
        return conflict_status;
      }
      in_deactivate_list = false;
      deactivate_request_.erase(deactivate_list_it);
    }

    // check for doubled activation
    if (is_active && !in_deactivate_list && in_activate_list)
    {
      auto conflict_status = handle_conflict(
        "Could not activate controller '" + controller.info.name + "' since it is already active");
      if (conflict_status != controller_interface::return_type::OK)
      {
        return conflict_status;
      }
      in_activate_list = false;
      activate_request_.erase(activate_list_it);
    }

    // check for illegal activation of an unconfigured/finalized controller
    if (!is_inactive && !in_deactivate_list && in_activate_list)
    {
      auto conflict_status = handle_conflict(
        "Could not activate controller '" + controller.info.name +
        "' since it is not in inactive state");
      if (conflict_status != controller_interface::return_type::OK)
      {
        return conflict_status;
      }
      in_activate_list = false;
      activate_request_.erase(activate_list_it);
    }

    if (in_activate_list)
    {
      extract_command_interfaces_for_controller(
        controller, resource_manager_, activate_command_interface_request_);
    }
    if (in_deactivate_list)
    {
      extract_command_interfaces_for_controller(
        controller, resource_manager_, deactivate_command_interface_request_);
    }

    // cache mapping between hardware and controllers for stopping when read/write error happens
    // TODO(destogl): This caching approach is suboptimal because the cache can fast become
    // outdated. Keeping it up to date is not easy because of stopping controllers from multiple
    // threads maybe we should not at all cache this but always search for the related controllers
    // to a hardware when error in hardware happens
    if (in_activate_list)
    {
      std::vector<std::string> interface_names = {};

      auto command_interface_config = controller.c->command_interface_configuration();
      if (command_interface_config.type == controller_interface::interface_configuration_type::ALL)
      {
        interface_names = resource_manager_->available_command_interfaces();
      }
      if (
        command_interface_config.type ==
        controller_interface::interface_configuration_type::INDIVIDUAL)
      {
        interface_names = command_interface_config.names;
      }

      std::vector<std::string> interfaces = {};
      auto state_interface_config = controller.c->state_interface_configuration();
      if (state_interface_config.type == controller_interface::interface_configuration_type::ALL)
      {
        interfaces = resource_manager_->available_state_interfaces();
      }
      if (
        state_interface_config.type ==
        controller_interface::interface_configuration_type::INDIVIDUAL)
      {
        interfaces = state_interface_config.names;
      }

      interface_names.insert(interface_names.end(), interfaces.begin(), interfaces.end());

      resource_manager_->cache_controller_to_hardware(controller.info.name, interface_names);
    }
  }

  if (activate_request_.empty() && deactivate_request_.empty())
  {
    message = "After checking the controllers, no controllers need to be activated or deactivated.";
    RCLCPP_INFO(get_logger(), "Empty activate and deactivate list, not requesting switch");
    clear_requests();
    return controller_interface::return_type::OK;
  }

  if (
    check_for_interfaces_availability_to_activate(controllers, activate_request_, message) !=
    controller_interface::return_type::OK)
  {
    clear_requests();
    return controller_interface::return_type::ERROR;
  }

  RCLCPP_DEBUG(get_logger(), "Request for command interfaces from activating controllers:");
  for (const auto & interface : activate_command_interface_request_)
  {
    RCLCPP_DEBUG(get_logger(), " - %s", interface.c_str());
  }
  RCLCPP_DEBUG(get_logger(), "Release of command interfaces from deactivating controllers:");
  for (const auto & interface : deactivate_command_interface_request_)
  {
    RCLCPP_DEBUG(get_logger(), " - %s", interface.c_str());
  }

  // wait for deactivating async controllers to finish their current cycle
  for (const auto & controller : deactivate_request_)
  {
    auto controller_it = std::find_if(
      controllers.begin(), controllers.end(),
      std::bind(controller_name_compare, std::placeholders::_1, controller));
    if (controller_it != controllers.end())
    {
      controller_it->c->prepare_for_deactivation();
    }
  }

  if (
    !activate_command_interface_request_.empty() || !deactivate_command_interface_request_.empty())
  {
    if (!resource_manager_->prepare_command_mode_switch(
          activate_command_interface_request_, deactivate_command_interface_request_))
    {
      message = "Could not switch controllers since prepare command mode switch was rejected.";
      RCLCPP_ERROR(get_logger(), "%s", message.c_str());
      clear_requests();
      return controller_interface::return_type::ERROR;
    }
  }

  // start the atomic controller switching
  switch_params_.strictness = strictness;
  switch_params_.activate_asap = activate_asap;
  if (timeout == rclcpp::Duration{0, 0})
  {
    RCLCPP_INFO_ONCE(get_logger(), "Switch controller timeout is set to 0, using default 1s!");
    switch_params_.timeout = std::chrono::nanoseconds(1'000'000'000);
  }
  else
  {
    switch_params_.timeout = timeout.to_chrono<std::chrono::nanoseconds>();
  }
  switch_params_.do_switch = true;

  // wait until switch is finished
  RCLCPP_DEBUG(get_logger(), "Requested atomic controller switch from realtime loop");
  std::unique_lock<std::mutex> switch_params_guard(switch_params_.mutex);
  if (!switch_params_.cv.wait_for(
        switch_params_guard, switch_params_.timeout, [this] { return !switch_params_.do_switch; }))
  {
    message = fmt::format(
      FMT_COMPILE("Switch controller timed out after {} seconds!"),
      static_cast<double>(switch_params_.timeout.count()) / 1e9);
    RCLCPP_ERROR(get_logger(), "%s", message.c_str());
    clear_requests();
    return controller_interface::return_type::ERROR;
  }

  // copy the controllers spec from the used to the unused list
  std::vector<ControllerSpec> & to = rt_controllers_wrapper_.get_unused_list(guard);
  to = controllers;

  // update the claimed interface controller info
  auto switch_result = evaluate_switch_result(
    resource_manager_, activate_request_, deactivate_request_, strictness, get_logger(), to,
    message);

  // switch lists
  rt_controllers_wrapper_.switch_updated_list(guard);
  // clear unused list
  rt_controllers_wrapper_.get_unused_list(guard).clear();

  clear_requests();

  return switch_result;
}

controller_interface::ControllerInterfaceBaseSharedPtr ControllerManager::add_controller_impl(
  const ControllerSpec & controller)
{
  // lock controllers
  std::lock_guard<std::recursive_mutex> guard(rt_controllers_wrapper_.controllers_lock_);

  std::vector<ControllerSpec> & to = rt_controllers_wrapper_.get_unused_list(guard);
  const std::vector<ControllerSpec> & from = rt_controllers_wrapper_.get_updated_list(guard);

  // Copy all controllers from the 'from' list to the 'to' list
  to = from;

  auto found_it = std::find_if(
    to.begin(), to.end(),
    std::bind(controller_name_compare, std::placeholders::_1, controller.info.name));
  // Checks that we're not duplicating controllers
  if (found_it != to.end())
  {
    to.clear();
    RCLCPP_ERROR(
      get_logger(), "A controller named '%s' was already loaded inside the controller manager",
      controller.info.name.c_str());
    return nullptr;
  }

  const rclcpp::NodeOptions controller_node_options = determine_controller_node_options(controller);
  // Catch whatever exception the controller might throw
  try
  {
    if (
      controller.c->init(
        controller.info.name, robot_description_, get_update_rate(), get_namespace(),
        controller_node_options) == controller_interface::return_type::ERROR)
    {
      to.clear();
      RCLCPP_ERROR(
        get_logger(), "Could not initialize the controller named '%s'",
        controller.info.name.c_str());
      return nullptr;
    }
  }
  catch (const std::exception & e)
  {
    to.clear();
    RCLCPP_ERROR(
      get_logger(), "Caught exception of type : %s while initializing controller '%s': %s",
      typeid(e).name(), controller.info.name.c_str(), e.what());
    return nullptr;
  }
  catch (...)
  {
    to.clear();
    RCLCPP_ERROR(
      get_logger(), "Caught unknown exception while initializing controller '%s'",
      controller.info.name.c_str());
    return nullptr;
  }

  // initialize the data for the controller chain spec once it is loaded. It is needed, so when we
  // sort the controllers later, they will be added to the list
  controller_chain_spec_[controller.info.name] = ControllerChainSpec();
  controller_chained_state_interfaces_cache_[controller.info.name] = {};
  controller_chained_reference_interfaces_cache_[controller.info.name] = {};

  executor_->add_node(controller.c->get_node()->get_node_base_interface());
  to.emplace_back(controller);

  // Destroys the old controllers list when the realtime thread is finished with it.
  RCLCPP_DEBUG(get_logger(), "Realtime switches over to new controller list");
  rt_controllers_wrapper_.switch_updated_list(guard);
  RCLCPP_DEBUG(get_logger(), "Destruct controller");
  std::vector<ControllerSpec> & new_unused_list = rt_controllers_wrapper_.get_unused_list(guard);
  new_unused_list.clear();
  RCLCPP_DEBUG(get_logger(), "Destruct controller finished");

  return to.back().c;
}

void ControllerManager::deactivate_controllers(
  const std::vector<ControllerSpec> & rt_controller_list,
  const std::vector<std::string> controllers_to_deactivate)
{
  // deactivate controllers
  for (const auto & controller_name : controllers_to_deactivate)
  {
    auto found_it = std::find_if(
      rt_controller_list.begin(), rt_controller_list.end(),
      std::bind(controller_name_compare, std::placeholders::_1, controller_name));
    if (found_it == rt_controller_list.end())
    {
      RCLCPP_ERROR(
        get_logger(),
        "Got request to deactivate controller '%s' but it is not in the realtime controller list",
        controller_name.c_str());
      continue;
    }
    auto controller = found_it->c;
    if (is_controller_active(*controller))
    {
      try
      {
        const auto new_state = controller->get_node()->deactivate();
        controller->release_interfaces();

        // if it is a chainable controller, make the reference interfaces unavailable on
        // deactivation
        if (controller->is_chainable())
        {
          resource_manager_->make_controller_exported_state_interfaces_unavailable(controller_name);
          resource_manager_->make_controller_reference_interfaces_unavailable(controller_name);
        }
        if (new_state.id() != lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE)
        {
          RCLCPP_ERROR(
            get_logger(), "After deactivating, controller '%s' is in state '%s', expected Inactive",
            controller_name.c_str(), new_state.label().c_str());
        }
      }
      catch (const std::exception & e)
      {
        RCLCPP_ERROR(
          get_logger(), "Caught exception of type : %s while deactivating the  controller '%s': %s",
          typeid(e).name(), controller_name.c_str(), e.what());
        continue;
      }
      catch (...)
      {
        RCLCPP_ERROR(
          get_logger(), "Caught unknown exception while deactivating the controller '%s'",
          controller_name.c_str());
        continue;
      }
    }
  }
}

void ControllerManager::switch_chained_mode(
  const std::vector<std::string> & chained_mode_switch_list, bool to_chained_mode)
{
  std::vector<ControllerSpec> & rt_controller_list =
    rt_controllers_wrapper_.update_and_get_used_by_rt_list();

  for (const auto & controller_name : chained_mode_switch_list)
  {
    auto found_it = std::find_if(
      rt_controller_list.begin(), rt_controller_list.end(),
      std::bind(controller_name_compare, std::placeholders::_1, controller_name));
    if (found_it == rt_controller_list.end())
    {
      RCLCPP_FATAL(
        get_logger(),
        "Got request to turn %s chained mode for controller '%s', but controller is not in the "
        "realtime controller list. (This should never happen!)",
        (to_chained_mode ? "ON" : "OFF"), controller_name.c_str());
      continue;
    }
    auto controller = found_it->c;
    if (!is_controller_active(*controller))
    {
      if (!controller->set_chained_mode(to_chained_mode))
      {
        RCLCPP_ERROR(
          get_logger(),
          "Got request to turn %s chained mode for controller '%s', but controller refused to do "
          "it! The control will probably not work as expected. Try to restart all controllers. "
          "If "
          "the error persist check controllers' individual configuration.",
          (to_chained_mode ? "ON" : "OFF"), controller_name.c_str());
      }
    }
    else
    {
      RCLCPP_FATAL(
        get_logger(),
        "Got request to turn %s chained mode for controller '%s', but this can not happen if "
        "controller is in '%s' state. (This should never happen!)",
        (to_chained_mode ? "ON" : "OFF"), controller_name.c_str(),
        hardware_interface::lifecycle_state_names::ACTIVE);
    }
  }
}

void ControllerManager::activate_controllers(
  const std::vector<ControllerSpec> & rt_controller_list,
  const std::vector<std::string> controllers_to_activate)
{
  for (const auto & controller_name : controllers_to_activate)
  {
    auto found_it = std::find_if(
      rt_controller_list.begin(), rt_controller_list.end(),
      std::bind(controller_name_compare, std::placeholders::_1, controller_name));
    if (found_it == rt_controller_list.end())
    {
      RCLCPP_ERROR(
        get_logger(),
        "Got request to activate controller '%s' but it is not in the realtime controller list",
        controller_name.c_str());
      continue;
    }
    auto controller = found_it->c;
    // reset the last update cycle time for newly activated controllers
    *found_it->last_update_cycle_time =
      rclcpp::Time(0, 0, this->get_trigger_clock()->get_clock_type());

    bool assignment_successful = true;
    // assign command interfaces to the controller
    auto command_interface_config = controller->command_interface_configuration();
    // default to controller_interface::configuration_type::NONE
    std::vector<std::string> command_interface_names = {};
    if (command_interface_config.type == controller_interface::interface_configuration_type::ALL)
    {
      command_interface_names = resource_manager_->available_command_interfaces();
    }
    if (
      command_interface_config.type ==
      controller_interface::interface_configuration_type::INDIVIDUAL)
    {
      command_interface_names = command_interface_config.names;
    }
    std::vector<hardware_interface::LoanedCommandInterface> command_loans;
    command_loans.reserve(command_interface_names.size());
    for (const auto & command_interface : command_interface_names)
    {
      if (resource_manager_->command_interface_is_claimed(command_interface))
      {
        RCLCPP_ERROR(
          get_logger(),
          "Resource conflict for controller '%s'. Command interface '%s' is already claimed.",
          controller_name.c_str(), command_interface.c_str());
        command_loans.clear();
        assignment_successful = false;
        break;
      }
      try
      {
        command_loans.emplace_back(resource_manager_->claim_command_interface(command_interface));
      }
      catch (const std::exception & e)
      {
        RCLCPP_ERROR(
          get_logger(),
          "Caught exception of type : %s while claiming the command interfaces. Can't activate "
          "controller '%s': %s",
          typeid(e).name(), controller_name.c_str(), e.what());
        command_loans.clear();
        assignment_successful = false;
        break;
      }
    }
    // something went wrong during command interfaces, go skip the controller
    if (!assignment_successful)
    {
      continue;
    }

    // assign state interfaces to the controller
    auto state_interface_config = controller->state_interface_configuration();
    // default to controller_interface::configuration_type::NONE
    std::vector<std::string> state_interface_names = {};
    if (state_interface_config.type == controller_interface::interface_configuration_type::ALL)
    {
      state_interface_names = resource_manager_->available_state_interfaces();
    }
    if (
      state_interface_config.type == controller_interface::interface_configuration_type::INDIVIDUAL)
    {
      state_interface_names = state_interface_config.names;
    }
    std::vector<hardware_interface::LoanedStateInterface> state_loans;
    state_loans.reserve(state_interface_names.size());
    for (const auto & state_interface : state_interface_names)
    {
      try
      {
        state_loans.emplace_back(resource_manager_->claim_state_interface(state_interface));
      }
      catch (const std::exception & e)
      {
        RCLCPP_ERROR(
          get_logger(),
          "Caught exception of type : %s while claiming the state interfaces. Can't activate "
          "controller '%s': %s",
          typeid(e).name(), controller_name.c_str(), e.what());
        assignment_successful = false;
        break;
      }
    }
    // something went wrong during state interfaces, go skip the controller
    if (!assignment_successful)
    {
      continue;
    }
    controller->assign_interfaces(std::move(command_loans), std::move(state_loans));

    try
    {
      found_it->periodicity_statistics->Reset();
      found_it->execution_time_statistics->Reset();
      const auto new_state = controller->get_node()->activate();
      if (new_state.id() != lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE)
      {
        RCLCPP_ERROR(
          get_logger(),
          "After activation, controller '%s' is in state '%s' (%d), expected '%s' (%d).",
          controller->get_node()->get_name(), new_state.label().c_str(), new_state.id(),
          hardware_interface::lifecycle_state_names::ACTIVE,
          lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE);
      }
    }
    catch (const std::exception & e)
    {
      RCLCPP_ERROR(
        get_logger(), "Caught exception of type : %s while activating the controller '%s': %s",
        typeid(e).name(), controller_name.c_str(), e.what());
      controller->release_interfaces();
      continue;
    }
    catch (...)
    {
      RCLCPP_ERROR(
        get_logger(), "Caught unknown exception while activating the controller '%s'",
        controller_name.c_str());
      controller->release_interfaces();
      continue;
    }

    // if it is a chainable controller, make the reference interfaces available on activation
    if (controller->is_chainable())
    {
      // make all the exported interfaces of the controller available
      resource_manager_->make_controller_exported_state_interfaces_available(controller_name);
      resource_manager_->make_controller_reference_interfaces_available(controller_name);
    }
  }
}

void ControllerManager::activate_controllers_asap(
  const std::vector<ControllerSpec> & rt_controller_list,
  const std::vector<std::string> controllers_to_activate)
{
  //  https://github.com/ros-controls/ros2_control/issues/263
  activate_controllers(rt_controller_list, controllers_to_activate);
}

void ControllerManager::list_controllers_srv_cb(
  const std::shared_ptr<controller_manager_msgs::srv::ListControllers::Request>,
  std::shared_ptr<controller_manager_msgs::srv::ListControllers::Response> response)
{
  // lock services
  RCLCPP_DEBUG(get_logger(), "list controller service called");
  std::lock_guard<std::mutex> services_guard(services_lock_);
  RCLCPP_DEBUG(get_logger(), "list controller service locked");

  // lock controllers
  std::lock_guard<std::recursive_mutex> guard(rt_controllers_wrapper_.controllers_lock_);
  const std::vector<ControllerSpec> & controllers = rt_controllers_wrapper_.get_updated_list(guard);
  // create helper containers to create chained controller connections
  std::unordered_map<std::string, std::vector<std::string>> controller_chain_interface_map;
  std::unordered_map<std::string, std::set<std::string>> controller_chain_map;
  std::vector<size_t> chained_controller_indices;
  for (size_t i = 0; i < controllers.size(); ++i)
  {
    controller_chain_map[controllers[i].info.name] = {};
  }

  response->controller.reserve(controllers.size());
  for (size_t i = 0; i < controllers.size(); ++i)
  {
    controller_manager_msgs::msg::ControllerState controller_state;

    controller_state.name = controllers[i].info.name;
    controller_state.type = controllers[i].info.type;
    controller_state.is_async = controllers[i].c->is_async();
    controller_state.update_rate = static_cast<uint16_t>(controllers[i].c->get_update_rate());
    controller_state.claimed_interfaces = controllers[i].info.claimed_interfaces;
    controller_state.state = controllers[i].c->get_lifecycle_state().label();
    controller_state.is_chainable = controllers[i].c->is_chainable();
    controller_state.is_chained = controllers[i].c->is_in_chained_mode();

    // Get information about interfaces if controller are in 'inactive' or 'active' state
    if (is_controller_active(controllers[i].c) || is_controller_inactive(controllers[i].c))
    {
      auto command_interface_config = controllers[i].c->command_interface_configuration();
      if (command_interface_config.type == controller_interface::interface_configuration_type::ALL)
      {
        controller_state.required_command_interfaces = resource_manager_->command_interface_keys();
      }
      else if (
        command_interface_config.type ==
        controller_interface::interface_configuration_type::INDIVIDUAL)
      {
        controller_state.required_command_interfaces = command_interface_config.names;
      }

      auto state_interface_config = controllers[i].c->state_interface_configuration();
      if (state_interface_config.type == controller_interface::interface_configuration_type::ALL)
      {
        controller_state.required_state_interfaces = resource_manager_->state_interface_keys();
      }
      else if (
        state_interface_config.type ==
        controller_interface::interface_configuration_type::INDIVIDUAL)
      {
        controller_state.required_state_interfaces = state_interface_config.names;
      }
      // check for chained interfaces
      for (const auto & interface : controller_state.required_command_interfaces)
      {
        auto prefix_interface_type_pair = split_command_interface(interface);
        auto prefix = prefix_interface_type_pair.first;
        auto interface_type = prefix_interface_type_pair.second;
        if (controller_chain_map.find(prefix) != controller_chain_map.end())
        {
          controller_chain_map[controller_state.name].insert(prefix);
          controller_chain_interface_map[controller_state.name].push_back(interface_type);
        }
      }
      // check reference interfaces only if controller is inactive or active
      if (controllers[i].c->is_chainable())
      {
        auto references =
          resource_manager_->get_controller_reference_interface_names(controllers[i].info.name);
        auto exported_state_interfaces =
          resource_manager_->get_controller_exported_state_interface_names(
            controllers[i].info.name);
        controller_state.reference_interfaces.reserve(references.size());
        controller_state.exported_state_interfaces.reserve(exported_state_interfaces.size());
        for (const auto & reference : references)
        {
          const std::string prefix_name = controllers[i].c->get_node()->get_name();
          const std::string interface_name = reference.substr(prefix_name.size() + 1);
          controller_state.reference_interfaces.push_back(interface_name);
        }
        for (const auto & state_interface : exported_state_interfaces)
        {
          const std::string prefix_name = controllers[i].c->get_node()->get_name();
          const std::string interface_name = state_interface.substr(prefix_name.size() + 1);
          controller_state.exported_state_interfaces.push_back(interface_name);
        }
      }
    }
    response->controller.push_back(controller_state);
    // keep track of controllers that are part of a chain
    if (
      !controller_chain_interface_map[controller_state.name].empty() ||
      controllers[i].c->is_chainable())
    {
      chained_controller_indices.push_back(i);
    }
  }

  // create chain connections for all controllers in a chain
  for (const auto & index : chained_controller_indices)
  {
    auto & controller_state = response->controller[index];
    auto chained_set = controller_chain_map[controller_state.name];
    for (const auto & chained_name : chained_set)
    {
      controller_manager_msgs::msg::ChainConnection connection;
      connection.name = chained_name;
      connection.reference_interfaces = controller_chain_interface_map[controller_state.name];
      controller_state.chain_connections.push_back(connection);
    }
  }

  RCLCPP_DEBUG(get_logger(), "list controller service finished");
}

void ControllerManager::list_controller_types_srv_cb(
  const std::shared_ptr<controller_manager_msgs::srv::ListControllerTypes::Request>,
  std::shared_ptr<controller_manager_msgs::srv::ListControllerTypes::Response> response)
{
  // lock services
  RCLCPP_DEBUG(get_logger(), "list types service called");
  std::lock_guard<std::mutex> guard(services_lock_);
  RCLCPP_DEBUG(get_logger(), "list types service locked");

  auto cur_types = loader_->getDeclaredClasses();
  for (const auto & cur_type : cur_types)
  {
    response->types.push_back(cur_type);
    response->base_classes.push_back(kControllerInterfaceClassName);
    RCLCPP_DEBUG(get_logger(), "%s", cur_type.c_str());
  }
  cur_types = chainable_loader_->getDeclaredClasses();
  for (const auto & cur_type : cur_types)
  {
    response->types.push_back(cur_type);
    response->base_classes.push_back(kChainableControllerInterfaceClassName);
    RCLCPP_DEBUG(get_logger(), "%s", cur_type.c_str());
  }

  RCLCPP_DEBUG(get_logger(), "list types service finished");
}

void ControllerManager::load_controller_service_cb(
  const std::shared_ptr<controller_manager_msgs::srv::LoadController::Request> request,
  std::shared_ptr<controller_manager_msgs::srv::LoadController::Response> response)
{
  // lock services
  RCLCPP_DEBUG(get_logger(), "loading service called for controller '%s' ", request->name.c_str());
  std::lock_guard<std::mutex> guard(services_lock_);
  RCLCPP_DEBUG(get_logger(), "loading service locked");

  response->ok = load_controller(request->name).get() != nullptr;

  RCLCPP_DEBUG(
    get_logger(), "loading service finished for controller '%s' ", request->name.c_str());
}

void ControllerManager::configure_controller_service_cb(
  const std::shared_ptr<controller_manager_msgs::srv::ConfigureController::Request> request,
  std::shared_ptr<controller_manager_msgs::srv::ConfigureController::Response> response)
{
  // lock services
  RCLCPP_DEBUG(
    get_logger(), "configuring service called for controller '%s' ", request->name.c_str());
  std::lock_guard<std::mutex> guard(services_lock_);
  RCLCPP_DEBUG(get_logger(), "configuring service locked");

  response->ok = configure_controller(request->name) == controller_interface::return_type::OK;

  RCLCPP_DEBUG(
    get_logger(), "configuring service finished for controller '%s' ", request->name.c_str());
}

void ControllerManager::reload_controller_libraries_service_cb(
  const std::shared_ptr<controller_manager_msgs::srv::ReloadControllerLibraries::Request> request,
  std::shared_ptr<controller_manager_msgs::srv::ReloadControllerLibraries::Response> response)
{
  // lock services
  RCLCPP_DEBUG(get_logger(), "reload libraries service called");
  std::lock_guard<std::mutex> guard(services_lock_);
  RCLCPP_DEBUG(get_logger(), "reload libraries service locked");

  // only reload libraries if no controllers are active
  std::vector<std::string> loaded_controllers, active_controllers;
  loaded_controllers = get_controller_names();
  {
    // lock controllers
    std::lock_guard<std::recursive_mutex> ctrl_guard(rt_controllers_wrapper_.controllers_lock_);
    for (const auto & controller : rt_controllers_wrapper_.get_updated_list(ctrl_guard))
    {
      if (is_controller_active(*controller.c))
      {
        active_controllers.push_back(controller.info.name);
      }
    }
  }
  if (!active_controllers.empty() && !request->force_kill)
  {
    RCLCPP_ERROR(
      get_logger(),
      "Controller manager: Cannot reload controller libraries because"
      " there are still %i active controllers",
      (int)active_controllers.size());
    response->ok = false;
    return;
  }

  // stop active controllers if requested
  if (!loaded_controllers.empty())
  {
    RCLCPP_INFO(get_logger(), "Controller manager: Stopping all active controllers");
    std::vector<std::string> empty;
    if (
      switch_controller(
        empty, active_controllers,
        controller_manager_msgs::srv::SwitchController::Request::BEST_EFFORT) !=
      controller_interface::return_type::OK)
    {
      RCLCPP_ERROR(
        get_logger(),
        "Controller manager: Cannot reload controller libraries because failed to stop "
        "active controllers");
      response->ok = false;
      return;
    }
    for (const auto & controller : loaded_controllers)
    {
      if (unload_controller(controller) != controller_interface::return_type::OK)
      {
        RCLCPP_ERROR(
          get_logger(),
          "Controller manager: Cannot reload controller libraries because "
          "failed to unload controller '%s'",
          controller.c_str());
        response->ok = false;
        return;
      }
    }
    loaded_controllers = get_controller_names();
  }
  assert(loaded_controllers.empty());

  // Force a reload on all the PluginLoaders (internally, this recreates the plugin loaders)
  loader_ = std::make_shared<pluginlib::ClassLoader<controller_interface::ControllerInterface>>(
    kControllerInterfaceNamespace, kControllerInterfaceClassName);
  chainable_loader_ =
    std::make_shared<pluginlib::ClassLoader<controller_interface::ChainableControllerInterface>>(
      kControllerInterfaceNamespace, kChainableControllerInterfaceClassName);
  RCLCPP_INFO(
    get_logger(), "Controller manager: reloaded controller libraries for '%s'",
    kControllerInterfaceNamespace);

  response->ok = true;

  RCLCPP_DEBUG(get_logger(), "reload libraries service finished");
}

void ControllerManager::switch_controller_service_cb(
  const std::shared_ptr<controller_manager_msgs::srv::SwitchController::Request> request,
  std::shared_ptr<controller_manager_msgs::srv::SwitchController::Response> response)
{
  // lock services
  RCLCPP_DEBUG(get_logger(), "switching service called");
  std::lock_guard<std::mutex> guard(services_lock_);
  RCLCPP_DEBUG(get_logger(), "switching service locked");

  response->ok = switch_controller_cb(
                   request->activate_controllers, request->deactivate_controllers,
                   request->strictness, request->activate_asap, request->timeout,
                   response->message) == controller_interface::return_type::OK;

  RCLCPP_DEBUG(get_logger(), "switching service finished");
}

void ControllerManager::unload_controller_service_cb(
  const std::shared_ptr<controller_manager_msgs::srv::UnloadController::Request> request,
  std::shared_ptr<controller_manager_msgs::srv::UnloadController::Response> response)
{
  // lock services
  RCLCPP_DEBUG(
    get_logger(), "unloading service called for controller '%s' ", request->name.c_str());
  std::lock_guard<std::mutex> guard(services_lock_);
  RCLCPP_DEBUG(get_logger(), "unloading service locked");

  response->ok = unload_controller(request->name) == controller_interface::return_type::OK;

  RCLCPP_DEBUG(
    get_logger(), "unloading service finished for controller '%s' ", request->name.c_str());
}

void ControllerManager::list_hardware_components_srv_cb(
  const std::shared_ptr<controller_manager_msgs::srv::ListHardwareComponents::Request>,
  std::shared_ptr<controller_manager_msgs::srv::ListHardwareComponents::Response> response)
{
  RCLCPP_DEBUG(get_logger(), "list hardware components service called");
  std::lock_guard<std::mutex> guard(services_lock_);
  RCLCPP_DEBUG(get_logger(), "list hardware components service locked");

  auto hw_components_info = resource_manager_->get_components_status();

  response->component.reserve(hw_components_info.size());

  for (const auto & [component_name, component_info] : hw_components_info)
  {
    auto component = controller_manager_msgs::msg::HardwareComponentState();
    component.name = component_name;
    component.type = component_info.type;
    component.is_async = component_info.is_async;
    component.rw_rate = static_cast<uint16_t>(component_info.rw_rate);
    component.plugin_name = component_info.plugin_name;
    component.state.id = component_info.state.id();
    component.state.label = component_info.state.label();

    component.command_interfaces.reserve(component_info.command_interfaces.size());
    for (const auto & interface : component_info.command_interfaces)
    {
      controller_manager_msgs::msg::HardwareInterface hwi;
      hwi.name = interface;
      hwi.data_type = resource_manager_->get_command_interface_data_type(interface);
      hwi.is_available = resource_manager_->command_interface_is_available(interface);
      hwi.is_claimed = resource_manager_->command_interface_is_claimed(interface);
      // TODO(destogl): Add here mapping to controller that has claimed or
      // can be claiming this interface
      // Those should be two variables
      // if (hwi.is_claimed)
      // {
      //   for (const auto & controller : controllers_that_use_interface(interface))
      //   {
      //     if (is_controller_active(controller))
      //     {
      //       hwi.is_claimed_by = controller;
      //     }
      //   }
      // }
      // hwi.is_used_by = controllers_that_use_interface(interface);
      component.command_interfaces.push_back(hwi);
    }

    component.state_interfaces.reserve(component_info.state_interfaces.size());
    for (const auto & interface : component_info.state_interfaces)
    {
      controller_manager_msgs::msg::HardwareInterface hwi;
      hwi.name = interface;
      hwi.data_type = resource_manager_->get_state_interface_data_type(interface);
      hwi.is_available = resource_manager_->state_interface_is_available(interface);
      hwi.is_claimed = false;
      component.state_interfaces.push_back(hwi);
    }

    response->component.push_back(component);
  }

  RCLCPP_DEBUG(get_logger(), "list hardware components service finished");
}

void ControllerManager::list_hardware_interfaces_srv_cb(
  const std::shared_ptr<controller_manager_msgs::srv::ListHardwareInterfaces::Request>,
  std::shared_ptr<controller_manager_msgs::srv::ListHardwareInterfaces::Response> response)
{
  RCLCPP_DEBUG(get_logger(), "list hardware interfaces service called");
  std::lock_guard<std::mutex> guard(services_lock_);
  RCLCPP_DEBUG(get_logger(), "list hardware interfaces service locked");

  auto state_interface_names = resource_manager_->state_interface_keys();
  for (const auto & state_interface_name : state_interface_names)
  {
    controller_manager_msgs::msg::HardwareInterface hwi;
    hwi.name = state_interface_name;
    hwi.is_available = resource_manager_->state_interface_is_available(state_interface_name);
    hwi.data_type = resource_manager_->get_state_interface_data_type(state_interface_name);
    hwi.is_claimed = false;
    response->state_interfaces.push_back(hwi);
  }
  auto command_interface_names = resource_manager_->command_interface_keys();
  for (const auto & command_interface_name : command_interface_names)
  {
    controller_manager_msgs::msg::HardwareInterface hwi;
    hwi.name = command_interface_name;
    hwi.is_available = resource_manager_->command_interface_is_available(command_interface_name);
    hwi.is_claimed = resource_manager_->command_interface_is_claimed(command_interface_name);
    hwi.data_type = resource_manager_->get_command_interface_data_type(command_interface_name);
    response->command_interfaces.push_back(hwi);
  }

  RCLCPP_DEBUG(get_logger(), "list hardware interfaces service finished");
}

void ControllerManager::set_hardware_component_state_srv_cb(
  const std::shared_ptr<controller_manager_msgs::srv::SetHardwareComponentState::Request> request,
  std::shared_ptr<controller_manager_msgs::srv::SetHardwareComponentState::Response> response)
{
  RCLCPP_DEBUG(get_logger(), "set hardware component state service called");
  std::lock_guard<std::mutex> guard(services_lock_);
  RCLCPP_DEBUG(get_logger(), "set hardware component state service locked");

  RCLCPP_DEBUG(get_logger(), "set hardware component state '%s'", request->name.c_str());

  auto hw_components_info = resource_manager_->get_components_status();
  if (hw_components_info.find(request->name) != hw_components_info.end())
  {
    rclcpp_lifecycle::State target_state(
      request->target_state.id,
      // the ternary operator is needed because label in State constructor cannot be an empty string
      request->target_state.label.empty() ? "-" : request->target_state.label);
    response->ok =
      (resource_manager_->set_component_state(request->name, target_state) ==
       hardware_interface::return_type::OK);
    hw_components_info = resource_manager_->get_components_status();
    response->state.id = hw_components_info[request->name].state.id();
    response->state.label = hw_components_info[request->name].state.label();
  }
  else
  {
    RCLCPP_ERROR(
      get_logger(), "hardware component with name '%s' does not exist", request->name.c_str());
    response->ok = false;
  }

  RCLCPP_DEBUG(get_logger(), "set hardware component state service finished");
}

std::vector<std::string> ControllerManager::get_controller_names()
{
  std::vector<std::string> names;

  // lock controllers
  std::lock_guard<std::recursive_mutex> guard(rt_controllers_wrapper_.controllers_lock_);
  for (const auto & controller : rt_controllers_wrapper_.get_updated_list(guard))
  {
    names.push_back(controller.info.name);
  }
  return names;
}

void ControllerManager::read(const rclcpp::Time & time, const rclcpp::Duration & period)
{
  periodicity_stats_.AddMeasurement(1.0 / period.seconds());
  auto [result, failed_hardware_names] = resource_manager_->read(time, period);

  if (result != hardware_interface::return_type::OK)
  {
    rt_buffer_.deactivate_controllers_list.clear();
    // Determine controllers to stop
    for (const auto & hardware_name : failed_hardware_names)
    {
      auto controllers = resource_manager_->get_cached_controllers_to_hardware(hardware_name);
      rt_buffer_.deactivate_controllers_list.insert(
        rt_buffer_.deactivate_controllers_list.end(), controllers.begin(), controllers.end());
    }
    RCLCPP_ERROR(
      get_logger(),
      "Deactivating following hardware components as their read cycle resulted in an error: [ %s]",
      rt_buffer_.get_concatenated_string(failed_hardware_names).c_str());
    RCLCPP_ERROR_EXPRESSION(
      get_logger(), !rt_buffer_.deactivate_controllers_list.empty(),
      "Deactivating following controllers as their hardware components read cycle resulted in an "
      "error: [ %s]",
      rt_buffer_.get_concatenated_string(rt_buffer_.deactivate_controllers_list).c_str());
    std::vector<ControllerSpec> & rt_controller_list =
      rt_controllers_wrapper_.update_and_get_used_by_rt_list();

    // As the hardware is in UNCONFIGURED state with error call, no need to prepare or perform
    // command mode switch
    deactivate_controllers(rt_controller_list, rt_buffer_.deactivate_controllers_list);
    // TODO(destogl): do auto-start of broadcasters
  }
}

void ControllerManager::manage_switch()
{
  std::unique_lock<std::mutex> guard(switch_params_.mutex, std::try_to_lock);
  if (!guard.owns_lock())
  {
    RCLCPP_DEBUG(get_logger(), "Unable to lock switch mutex. Retrying in next cycle.");
    return;
  }
  // Ask hardware interfaces to change mode
  if (!resource_manager_->perform_command_mode_switch(
        activate_command_interface_request_, deactivate_command_interface_request_))
  {
    RCLCPP_ERROR(get_logger(), "Error while performing mode switch.");
  }

  std::vector<ControllerSpec> & rt_controller_list =
    rt_controllers_wrapper_.update_and_get_used_by_rt_list();

  deactivate_controllers(rt_controller_list, deactivate_request_);

  switch_chained_mode(to_chained_mode_request_, true);
  switch_chained_mode(from_chained_mode_request_, false);

  // activate controllers once the switch is fully complete
  if (!switch_params_.activate_asap)
  {
    activate_controllers(rt_controller_list, activate_request_);
  }
  else
  {
    // activate controllers as soon as their required joints are done switching
    activate_controllers_asap(rt_controller_list, activate_request_);
  }

  // All controllers switched --> switching done
  switch_params_.do_switch = false;
  switch_params_.cv.notify_all();
}

controller_interface::return_type ControllerManager::update(
  const rclcpp::Time & time, const rclcpp::Duration & period)
{
  std::vector<ControllerSpec> & rt_controller_list =
    rt_controllers_wrapper_.update_and_get_used_by_rt_list();

  auto ret = controller_interface::return_type::OK;
  ++update_loop_counter_;
  update_loop_counter_ %= update_rate_;

  // Check for valid time
  if (!get_clock()->started())
  {
    if (time == rclcpp::Time(0, 0, this->get_trigger_clock()->get_clock_type()))
    {
      throw std::runtime_error(
        "No clock received, and time argument is zero. Check your controller_manager node's "
        "clock configuration (use_sim_time parameter) and if a valid clock source is "
        "available. Also pass a proper time argument to the update method.");
    }

    // this can happen with use_sim_time=true until the /clock is received
    rclcpp::Clock clock = rclcpp::Clock();
    RCLCPP_WARN_THROTTLE(
      get_logger(), clock, 1000,
      "No clock received, using time argument instead! Check your node's clock "
      "configuration (use_sim_time parameter) and if a valid clock source is available");
  }

  rt_buffer_.deactivate_controllers_list.clear();
  for (const auto & loaded_controller : rt_controller_list)
  {
    // TODO(v-lopez) we could cache this information
    // https://github.com/ros-controls/ros2_control/issues/153
    if (is_controller_active(*loaded_controller.c))
    {
      if (
        switch_params_.do_switch && loaded_controller.c->is_async() &&
        std::find(
          deactivate_request_.begin(), deactivate_request_.end(), loaded_controller.info.name) !=
          deactivate_request_.end())
      {
        RCLCPP_DEBUG(
          get_logger(), "Skipping update for async controller '%s' as it is being deactivated",
          loaded_controller.info.name.c_str());
        continue;
      }
      const auto controller_update_rate = loaded_controller.c->get_update_rate();
      const bool run_controller_at_cm_rate = (controller_update_rate >= update_rate_);
      const auto controller_period =
        run_controller_at_cm_rate ? period
                                  : rclcpp::Duration::from_seconds((1.0 / controller_update_rate));

      bool first_update_cycle = false;
      const rclcpp::Time current_time = get_clock()->started() ? get_trigger_clock()->now() : time;
      if (
        *loaded_controller.last_update_cycle_time ==
        rclcpp::Time(0, 0, this->get_trigger_clock()->get_clock_type()))
      {
        // last_update_cycle_time is zero after activation
        first_update_cycle = true;
        *loaded_controller.last_update_cycle_time = current_time;
        RCLCPP_DEBUG(
          get_logger(), "Setting last_update_cycle_time to %fs for the controller : %s",
          loaded_controller.last_update_cycle_time->seconds(), loaded_controller.info.name.c_str());
      }
      const auto controller_actual_period =
        (current_time - *loaded_controller.last_update_cycle_time);

      /// @note The factor 0.99 is used to avoid the controllers skipping update cycles due to the
      /// jitter in the system sleep cycles.
      // For instance, A controller running at 50 Hz and the CM running at 100Hz, then when we have
      // an update cycle at 0.019s (ideally, the controller should only trigger >= 0.02s), if we
      // wait for next cycle, then trigger will happen at ~0.029 sec and this is creating an issue
      // to keep up with the controller update rate (see issue #1769).
      const bool controller_go =
        run_controller_at_cm_rate ||
        (time == rclcpp::Time(0, 0, this->get_trigger_clock()->get_clock_type())) ||
        (controller_actual_period.seconds() * controller_update_rate >= 0.99) || first_update_cycle;

      RCLCPP_DEBUG(
        get_logger(), "update_loop_counter: '%d ' controller_go: '%s ' controller_name: '%s '",
        update_loop_counter_, controller_go ? "True" : "False",
        loaded_controller.info.name.c_str());

      if (controller_go)
      {
        auto controller_ret = controller_interface::return_type::OK;
        bool trigger_status = true;
        // Catch exceptions thrown by the controller update function
        try
        {
          const auto trigger_result =
            loaded_controller.c->trigger_update(this->now(), controller_actual_period);
          trigger_status = trigger_result.successful;
          controller_ret = trigger_result.result;
          if (trigger_status && trigger_result.execution_time.has_value())
          {
            loaded_controller.execution_time_statistics->AddMeasurement(
              static_cast<double>(trigger_result.execution_time.value().count()) / 1.e3);
          }
          if (!first_update_cycle && trigger_status && trigger_result.period.has_value())
          {
            loaded_controller.periodicity_statistics->AddMeasurement(
              1.0 / trigger_result.period.value().seconds());
          }
        }
        catch (const std::exception & e)
        {
          RCLCPP_ERROR(
            get_logger(), "Caught exception of type : %s while updating controller '%s': %s",
            typeid(e).name(), loaded_controller.info.name.c_str(), e.what());
          controller_ret = controller_interface::return_type::ERROR;
        }
        catch (...)
        {
          RCLCPP_ERROR(
            get_logger(), "Caught unknown exception while updating controller '%s'",
            loaded_controller.info.name.c_str());
          controller_ret = controller_interface::return_type::ERROR;
        }

        *loaded_controller.last_update_cycle_time = current_time;

        if (controller_ret != controller_interface::return_type::OK)
        {
          rt_buffer_.deactivate_controllers_list.push_back(loaded_controller.info.name);
          ret = controller_ret;
        }
      }
    }
  }
  if (!rt_buffer_.deactivate_controllers_list.empty())
  {
    rt_buffer_.fallback_controllers_list.clear();
    rt_buffer_.activate_controllers_using_interfaces_list.clear();

    for (const auto & failed_ctrl : rt_buffer_.deactivate_controllers_list)
    {
      auto ctrl_it = std::find_if(
        rt_controller_list.begin(), rt_controller_list.end(),
        std::bind(controller_name_compare, std::placeholders::_1, failed_ctrl));
      if (ctrl_it != rt_controller_list.end())
      {
        for (const auto & fallback_controller : ctrl_it->info.fallback_controllers_names)
        {
          rt_buffer_.fallback_controllers_list.push_back(fallback_controller);
          get_active_controllers_using_command_interfaces_of_controller(
            fallback_controller, rt_controller_list,
            rt_buffer_.activate_controllers_using_interfaces_list);
        }
      }
    }

    RCLCPP_ERROR(
      get_logger(), "Deactivating controllers : [ %s] as their update resulted in an error!",
      rt_buffer_.get_concatenated_string(rt_buffer_.deactivate_controllers_list).c_str());
    RCLCPP_ERROR_EXPRESSION(
      get_logger(), !rt_buffer_.activate_controllers_using_interfaces_list.empty(),
      "Deactivating controllers : [ %s] using the command interfaces needed for the fallback "
      "controllers to activate.",
      rt_buffer_.get_concatenated_string(rt_buffer_.activate_controllers_using_interfaces_list)
        .c_str());
    RCLCPP_ERROR_EXPRESSION(
      get_logger(), !rt_buffer_.fallback_controllers_list.empty(),
      "Activating fallback controllers : [ %s]",
      rt_buffer_.get_concatenated_string(rt_buffer_.fallback_controllers_list).c_str());
    std::for_each(
      rt_buffer_.activate_controllers_using_interfaces_list.begin(),
      rt_buffer_.activate_controllers_using_interfaces_list.end(),
      [this](const std::string & controller)
      { ros2_control::add_item(rt_buffer_.deactivate_controllers_list, controller); });

    // Retrieve the interfaces to start and stop from the hardware end
    perform_hardware_command_mode_change(
      rt_controller_list, rt_buffer_.fallback_controllers_list,
      rt_buffer_.deactivate_controllers_list, "update");
    deactivate_controllers(rt_controller_list, rt_buffer_.deactivate_controllers_list);
    if (!rt_buffer_.fallback_controllers_list.empty())
    {
      activate_controllers(rt_controller_list, rt_buffer_.fallback_controllers_list);
    }
    // To publish the activity of the failing controllers and the fallback controllers
    publish_activity();
  }
  resource_manager_->enforce_command_limits(period);

  // there are controllers to (de)activate
  if (switch_params_.do_switch)
  {
    manage_switch();
  }

  PUBLISH_ROS2_CONTROL_INTROSPECTION_DATA_ASYNC(hardware_interface::DEFAULT_REGISTRY_KEY);

  return ret;
}

void ControllerManager::write(const rclcpp::Time & time, const rclcpp::Duration & period)
{
  auto [result, failed_hardware_names] = resource_manager_->write(time, period);

  if (result == hardware_interface::return_type::ERROR)
  {
    rt_buffer_.deactivate_controllers_list.clear();
    // Determine controllers to stop
    for (const auto & hardware_name : failed_hardware_names)
    {
      auto controllers = resource_manager_->get_cached_controllers_to_hardware(hardware_name);
      rt_buffer_.deactivate_controllers_list.insert(
        rt_buffer_.deactivate_controllers_list.end(), controllers.begin(), controllers.end());
    }
    RCLCPP_ERROR(
      get_logger(),
      "Deactivating following hardware components as their write cycle resulted in an error: [ "
      "%s]",
      rt_buffer_.get_concatenated_string(failed_hardware_names).c_str());
    RCLCPP_ERROR_EXPRESSION(
      get_logger(), !rt_buffer_.deactivate_controllers_list.empty(),
      "Deactivating following controllers as their hardware components write cycle resulted in an "
      "error: [ %s]",
      rt_buffer_.get_concatenated_string(rt_buffer_.deactivate_controllers_list).c_str());
    std::vector<ControllerSpec> & rt_controller_list =
      rt_controllers_wrapper_.update_and_get_used_by_rt_list();

    // As the hardware is in UNCONFIGURED state with error call, no need to prepare or perform
    // command mode switch
    deactivate_controllers(rt_controller_list, rt_buffer_.deactivate_controllers_list);
    // TODO(destogl): do auto-start of broadcasters
  }
  else if (result == hardware_interface::return_type::DEACTIVATE)
  {
    rt_buffer_.deactivate_controllers_list.clear();
    auto loaded_controllers = get_loaded_controllers();
    // Only stop controllers with active command interfaces to the failed_hardware_names
    for (const auto & hardware_name : failed_hardware_names)
    {
      auto controllers = resource_manager_->get_cached_controllers_to_hardware(hardware_name);
      for (const auto & controller : controllers)
      {
        auto controller_spec = std::find_if(
          loaded_controllers.begin(), loaded_controllers.end(),
          [&](const controller_manager::ControllerSpec & spec)
          { return spec.c->get_name() == controller; });
        if (controller_spec == loaded_controllers.end())
        {
          RCLCPP_WARN(
            get_logger(),
            "Deactivate failed to find controller [%s] in loaded controllers. "
            "This can happen due to multiple returns of 'DEACTIVATE' from [%s] write()",
            controller.c_str(), hardware_name.c_str());
          continue;
        }
        std::vector<std::string> command_interface_names;
        extract_command_interfaces_for_controller(
          *controller_spec, resource_manager_, command_interface_names);
        // if this controller has command interfaces add it to the deactivate_controllers_list
        if (!command_interface_names.empty())
        {
          rt_buffer_.deactivate_controllers_list.push_back(controller);
        }
      }
    }
    RCLCPP_ERROR_EXPRESSION(
      get_logger(), !rt_buffer_.deactivate_controllers_list.empty(),
      "Deactivating controllers [%s] as their command interfaces are tied to DEACTIVATEing "
      "hardware components",
      rt_buffer_.get_concatenated_string(rt_buffer_.deactivate_controllers_list).c_str());
    std::vector<ControllerSpec> & rt_controller_list =
      rt_controllers_wrapper_.update_and_get_used_by_rt_list();

    perform_hardware_command_mode_change(
      rt_controller_list, {}, rt_buffer_.deactivate_controllers_list, "write");
    deactivate_controllers(rt_controller_list, rt_buffer_.deactivate_controllers_list);
  }
}

std::vector<ControllerSpec> &
ControllerManager::RTControllerListWrapper::update_and_get_used_by_rt_list()
{
  used_by_realtime_controllers_index_ = updated_controllers_index_;
  return controllers_lists_[used_by_realtime_controllers_index_];
}

std::vector<ControllerSpec> & ControllerManager::RTControllerListWrapper::get_unused_list(
  const std::lock_guard<std::recursive_mutex> &)
{
  if (!controllers_lock_.try_lock())
  {
    throw std::runtime_error("controllers_lock_ not owned by thread");
  }
  controllers_lock_.unlock();
  // Get the index to the outdated controller list
  int free_controllers_list = get_other_list(updated_controllers_index_);

  // Wait until the outdated controller list is not being used by the realtime thread
  wait_until_rt_not_using(free_controllers_list);
  return controllers_lists_[free_controllers_list];
}

const std::vector<ControllerSpec> & ControllerManager::RTControllerListWrapper::get_updated_list(
  const std::lock_guard<std::recursive_mutex> &) const
{
  if (!controllers_lock_.try_lock())
  {
    throw std::runtime_error("controllers_lock_ not owned by thread");
  }
  controllers_lock_.unlock();
  return controllers_lists_[updated_controllers_index_];
}

void ControllerManager::RTControllerListWrapper::switch_updated_list(
  const std::lock_guard<std::recursive_mutex> &)
{
  if (!controllers_lock_.try_lock())
  {
    throw std::runtime_error("controllers_lock_ not owned by thread");
  }
  controllers_lock_.unlock();
  int former_current_controllers_list_ = updated_controllers_index_;
  updated_controllers_index_ = get_other_list(former_current_controllers_list_);
  wait_until_rt_not_using(former_current_controllers_list_);
  if (on_switch_callback_)
  {
    on_switch_callback_();
  }
}

void ControllerManager::RTControllerListWrapper::set_on_switch_callback(
  std::function<void()> callback)
{
  std::lock_guard<std::recursive_mutex> guard(controllers_lock_);
  on_switch_callback_ = callback;
}

int ControllerManager::RTControllerListWrapper::get_other_list(int index) const
{
  return (index + 1) % 2;
}

void ControllerManager::RTControllerListWrapper::wait_until_rt_not_using(
  int index, std::chrono::microseconds sleep_period) const
{
  while (used_by_realtime_controllers_index_ == index)
  {
    if (!rclcpp::ok())
    {
      throw std::runtime_error("rclcpp interrupted");
    }
    std::this_thread::sleep_for(sleep_period);
  }
}

std::pair<std::string, std::string> ControllerManager::split_command_interface(
  const std::string & command_interface)
{
  auto index = command_interface.find('/');
  auto prefix = command_interface.substr(0, index);
  auto interface_type = command_interface.substr(index + 1, command_interface.size() - 1);
  return {prefix, interface_type};
}

unsigned int ControllerManager::get_update_rate() const { return update_rate_; }

rclcpp::Clock::SharedPtr ControllerManager::get_trigger_clock() const { return trigger_clock_; }

void ControllerManager::perform_hardware_command_mode_change(
  const std::vector<ControllerSpec> & rt_controller_list,
  const std::vector<std::string> & activate_controllers_list,
  const std::vector<std::string> & deactivate_controllers_list, const std::string & rt_cycle_name)
{
  rt_buffer_.interfaces_to_start.clear();
  rt_buffer_.interfaces_to_stop.clear();
  get_controller_list_command_interfaces(
    deactivate_controllers_list, rt_controller_list, resource_manager_,
    rt_buffer_.interfaces_to_stop);
  get_controller_list_command_interfaces(
    activate_controllers_list, rt_controller_list, resource_manager_,
    rt_buffer_.interfaces_to_start);
  if (!rt_buffer_.interfaces_to_stop.empty() || !rt_buffer_.interfaces_to_start.empty())
  {
    if (!(resource_manager_->prepare_command_mode_switch(
            rt_buffer_.interfaces_to_start, rt_buffer_.interfaces_to_stop) &&
          resource_manager_->perform_command_mode_switch(
            rt_buffer_.interfaces_to_start, rt_buffer_.interfaces_to_stop)))
    {
      RCLCPP_ERROR(
        get_logger(),
        "Error while attempting mode switch when deactivating controllers in %s cycle!",
        rt_cycle_name.c_str());
    }
  }
}

void ControllerManager::propagate_deactivation_of_chained_mode(
  const std::vector<ControllerSpec> & controllers)
{
  for (const auto & controller : controllers)
  {
    // get pointers to places in deactivate and activate lists ((de)activate lists have changed)
    auto deactivate_list_it =
      std::find(deactivate_request_.begin(), deactivate_request_.end(), controller.info.name);

    if (deactivate_list_it != deactivate_request_.end())
    {
      // if controller is not active then skip adding following-controllers to "from" chained mode
      // request
      if (!is_controller_active(controller.c))
      {
        RCLCPP_DEBUG(
          get_logger(),
          "Controller with name '%s' can not be deactivated since is not active. "
          "The controller will be removed from the list later."
          "Skipping adding following controllers to 'from' chained mode request.",
          controller.info.name.c_str());
        break;
      }

      const auto ctrl_cmd_itf_names = controller.c->command_interface_configuration().names;
      const auto ctrl_state_itf_names = controller.c->state_interface_configuration().names;
      auto ctrl_itf_names = ctrl_cmd_itf_names;
      ctrl_itf_names.insert(
        ctrl_itf_names.end(), ctrl_state_itf_names.begin(), ctrl_state_itf_names.end());
      for (const auto & ctrl_itf_name : ctrl_itf_names)
      {
        // controller that 'cmd_tf_name' belongs to
        ControllersListIterator following_ctrl_it;
        if (is_interface_a_chained_interface(ctrl_itf_name, controllers, following_ctrl_it))
        {
          // currently iterated "controller" is preceding controller --> add following controller
          // with matching interface name to "from" chained mode list (if not already in it)
          if (
            std::find(
              from_chained_mode_request_.begin(), from_chained_mode_request_.end(),
              following_ctrl_it->info.name) == from_chained_mode_request_.end())
          {
            from_chained_mode_request_.push_back(following_ctrl_it->info.name);
            RCLCPP_DEBUG(
              get_logger(), "Adding controller '%s' in 'from chained mode' request.",
              following_ctrl_it->info.name.c_str());
          }
        }
      }
    }
  }
}

controller_interface::return_type ControllerManager::check_following_controllers_for_activate(
  const std::vector<ControllerSpec> & controllers, int strictness,
  const ControllersListIterator controller_it, std::string & message)
{
  // we assume that the controller exists is checked in advance
  RCLCPP_DEBUG(
    get_logger(), "Checking following controllers of preceding controller with name '%s'.",
    controller_it->info.name.c_str());

  const auto controller_cmd_interfaces = controller_it->c->command_interface_configuration().names;
  const auto controller_state_interfaces = controller_it->c->state_interface_configuration().names;
  // get all interfaces of the controller
  auto controller_interfaces = controller_cmd_interfaces;
  controller_interfaces.insert(
    controller_interfaces.end(), controller_state_interfaces.begin(),
    controller_state_interfaces.end());
  for (const auto & ctrl_itf_name : controller_interfaces)
  {
    RCLCPP_DEBUG(
      get_logger(), "Checking interface '%s' of controller '%s'.", ctrl_itf_name.c_str(),
      controller_it->info.name.c_str());
    ControllersListIterator following_ctrl_it;
    // Check if interface if reference interface and following controller exist.
    if (!is_interface_a_chained_interface(ctrl_itf_name, controllers, following_ctrl_it))
    {
      continue;
    }
    // TODO(destogl): performance of this code could be optimized by adding additional lists with
    // controllers that cache if the check has failed and has succeeded. Then the following would be
    // done only once per controller, otherwise in complex scenarios the same controller is checked
    // multiple times

    // check that all following controllers exits, are either: activated, will be activated, or
    // will not be deactivated
    RCLCPP_DEBUG(
      get_logger(), "Checking following controller with name '%s'.",
      following_ctrl_it->info.name.c_str());

    // check if following controller is chainable
    if (!following_ctrl_it->c->is_chainable())
    {
      message = fmt::format(
        FMT_COMPILE(
          "No state/reference interface from controller : '{}' exist, since the following "
          "controller with name '{}' is not chainable."),
        ctrl_itf_name, following_ctrl_it->info.name);
      RCLCPP_WARN(get_logger(), "%s", message.c_str());
      return controller_interface::return_type::ERROR;
    }

    if (is_controller_active(following_ctrl_it->c))
    {
      // will following controller be deactivated?
      if (
        std::find(
          deactivate_request_.begin(), deactivate_request_.end(), following_ctrl_it->info.name) !=
        deactivate_request_.end())
      {
        message = fmt::format(
          FMT_COMPILE(
            "The following controller with name '{}' is currently active but it is requested to "
            "be deactivated."),
          following_ctrl_it->info.name);
        RCLCPP_WARN(get_logger(), "%s", message.c_str());
        return controller_interface::return_type::ERROR;
      }
    }
    // check if following controller will not be activated
    else if (
      std::find(activate_request_.begin(), activate_request_.end(), following_ctrl_it->info.name) ==
      activate_request_.end())
    {
      message = fmt::format(
        FMT_COMPILE(
          "The following controller with name '{}' is currently inactive and it is not requested "
          "to be activated."),
        following_ctrl_it->info.name);
      RCLCPP_WARN(get_logger(), "%s", message.c_str());
      return controller_interface::return_type::ERROR;
    }

    // Trigger recursion to check all the following controllers only if they are OK, add this
    // controller update chained mode requests
    if (
      check_following_controllers_for_activate(
        controllers, strictness, following_ctrl_it, message) ==
      controller_interface::return_type::ERROR)
    {
      return controller_interface::return_type::ERROR;
    }

    // TODO(destogl): this should be discussed how to it the best - just a placeholder for now
    // else if (strictness ==
    //  controller_manager_msgs::srv::SwitchController::Request::MANIPULATE_CONTROLLERS_CHAIN)
    // {
    // // insert to the begin of activate request list to be activated before preceding controller
    //   activate_request_.insert(activate_request_.begin(), following_ctrl_name);
    // }
    if (!following_ctrl_it->c->is_in_chained_mode())
    {
      auto found_it = std::find(
        to_chained_mode_request_.begin(), to_chained_mode_request_.end(),
        following_ctrl_it->info.name);
      if (found_it == to_chained_mode_request_.end())
      {
        // if it is a chainable controller, make the reference interfaces available on preactivation
        // (This is needed when you activate a couple of chainable controller altogether)
        // make all the exported interfaces of the controller available
        resource_manager_->make_controller_exported_state_interfaces_available(
          following_ctrl_it->info.name);
        if (
          std::find(
            controller_cmd_interfaces.begin(), controller_cmd_interfaces.end(), ctrl_itf_name) !=
          controller_cmd_interfaces.end())
        {
          resource_manager_->make_controller_reference_interfaces_available(
            following_ctrl_it->info.name);
          to_chained_mode_request_.push_back(following_ctrl_it->info.name);
          RCLCPP_DEBUG(
            get_logger(), "Adding controller '%s' in 'to chained mode' request.",
            following_ctrl_it->info.name.c_str());
        }
      }
    }
    else
    {
      // Check if following controller is in 'from' chained mode list and remove it, if so
      auto found_it = std::find(
        from_chained_mode_request_.begin(), from_chained_mode_request_.end(),
        following_ctrl_it->info.name);
      if (found_it != from_chained_mode_request_.end())
      {
        from_chained_mode_request_.erase(found_it);
        RCLCPP_DEBUG(
          get_logger(),
          "Removing controller '%s' in 'from chained mode' request because it "
          "should stay in chained mode.",
          following_ctrl_it->info.name.c_str());
      }
    }
  }
  return controller_interface::return_type::OK;
};

controller_interface::return_type ControllerManager::check_preceding_controllers_for_deactivate(
  const std::vector<ControllerSpec> & controllers, int /*strictness*/,
  const ControllersListIterator controller_it, std::string & message)
{
  // if not chainable no need for any checks
  if (!controller_it->c->is_chainable())
  {
    return controller_interface::return_type::OK;
  }

  RCLCPP_DEBUG(
    get_logger(), "Checking preceding controller of following controller with name '%s'.",
    controller_it->info.name.c_str());

  auto preceding_controllers_list =
    controller_chained_state_interfaces_cache_[controller_it->info.name];
  preceding_controllers_list.insert(
    preceding_controllers_list.end(),
    controller_chained_reference_interfaces_cache_[controller_it->info.name].cbegin(),
    controller_chained_reference_interfaces_cache_[controller_it->info.name].cend());

  for (const auto & preceding_controller : preceding_controllers_list)
  {
    RCLCPP_DEBUG(get_logger(), "\t Preceding controller : '%s'.", preceding_controller.c_str());
    auto found_it = std::find_if(
      controllers.begin(), controllers.end(),
      std::bind(controller_name_compare, std::placeholders::_1, preceding_controller));

    if (found_it != controllers.end())
    {
      if (
        is_controller_inactive(found_it->c) &&
        std::find(activate_request_.begin(), activate_request_.end(), preceding_controller) !=
          activate_request_.end())
      {
        message = fmt::format(
          FMT_COMPILE(
            "Unable to deactivate controller with name '{}' because preceding controller with "
            "name '{}' is inactive and will be activated."),
          controller_it->info.name, preceding_controller);
        RCLCPP_WARN(get_logger(), "%s", message.c_str());
        return controller_interface::return_type::ERROR;
      }
      if (
        is_controller_active(found_it->c) &&
        std::find(deactivate_request_.begin(), deactivate_request_.end(), preceding_controller) ==
          deactivate_request_.end())
      {
        message = fmt::format(
          FMT_COMPILE(
            "Unable to deactivate controller with name '{}' because preceding controller with "
            "name '{}' is currently active and will not be deactivated."),
          controller_it->info.name, preceding_controller);
        RCLCPP_WARN(get_logger(), "%s", message.c_str());
        return controller_interface::return_type::ERROR;
      }
    }
  }

  // TODO(destogl): this should be discussed how to it the best - just a placeholder for now
  // else if (
  //  strictness ==
  //  controller_manager_msgs::srv::SwitchController::Request::MANIPULATE_CONTROLLERS_CHAIN)
  // {
  // // insert to the begin of activate request list to be activated before preceding
  // controller
  //   activate_request_.insert(activate_request_.begin(), preceding_ctrl_name);
  // }

  return controller_interface::return_type::OK;
}

controller_interface::return_type
ControllerManager::check_fallback_controllers_state_pre_activation(
  const std::vector<ControllerSpec> & controllers, const ControllersListIterator controller_it,
  std::string & message)
{
  for (const auto & fb_ctrl : controller_it->info.fallback_controllers_names)
  {
    auto fb_ctrl_it = std::find_if(
      controllers.begin(), controllers.end(),
      std::bind(controller_name_compare, std::placeholders::_1, fb_ctrl));
    if (fb_ctrl_it == controllers.end())
    {
      message = fmt::format(
        FMT_COMPILE(
          "Unable to find the fallback controller : '{}' of the controller : '{}' within the "
          "controller list"),
        fb_ctrl, controller_it->info.name);
      RCLCPP_ERROR(get_logger(), "%s", message.c_str());
      return controller_interface::return_type::ERROR;
    }
    else
    {
      if (!(is_controller_inactive(fb_ctrl_it->c) || is_controller_active(fb_ctrl_it->c)))
      {
        message = fmt::format(
          FMT_COMPILE(
            "Controller with name '{}' cannot be activated, as its fallback controller : '{}' need "
            "to be configured and be in inactive/active state!"),
          controller_it->info.name, fb_ctrl);
        RCLCPP_ERROR(get_logger(), "%s", message.c_str());
        return controller_interface::return_type::ERROR;
      }
      for (const auto & fb_cmd_itf : fb_ctrl_it->c->command_interface_configuration().names)
      {
        if (!resource_manager_->command_interface_is_available(fb_cmd_itf))
        {
          ControllersListIterator following_ctrl_it;
          if (is_interface_a_chained_interface(fb_cmd_itf, controllers, following_ctrl_it))
          {
            // if following_ctrl_it is inactive and it is in the fallback list of the
            // controller_it and then check it it's exported reference interface names list if
            // it's available
            if (is_controller_inactive(following_ctrl_it->c))
            {
              if (
                std::find(
                  controller_it->info.fallback_controllers_names.begin(),
                  controller_it->info.fallback_controllers_names.end(),
                  following_ctrl_it->info.name) !=
                controller_it->info.fallback_controllers_names.end())
              {
                const auto exported_ref_itfs =
                  resource_manager_->get_controller_reference_interface_names(
                    following_ctrl_it->info.name);
                if (
                  std::find(exported_ref_itfs.begin(), exported_ref_itfs.end(), fb_cmd_itf) ==
                  exported_ref_itfs.end())
                {
                  message = fmt::format(
                    FMT_COMPILE(
                      "Controller with name '{}' cannot be activated, as the command interface : "
                      "'{}' required by its fallback controller : '{}' is not exported by the "
                      "controller : '{}' in the current fallback list!"),
                    controller_it->info.name, fb_cmd_itf, fb_ctrl, following_ctrl_it->info.name);
                  RCLCPP_ERROR(get_logger(), "%s", message.c_str());
                  return controller_interface::return_type::ERROR;
                }
              }
              else
              {
                message = fmt::format(
                  FMT_COMPILE(
                    "Controller with name '{}' cannot be activated, as the command interface : "
                    "'{}' required by its fallback controller : '{}' is not available as the "
                    "controller is not in active state!. May be consider adding this controller to "
                    "the fallback list of the controller : '{}' or already have it activated."),
                  controller_it->info.name, fb_cmd_itf, fb_ctrl, following_ctrl_it->info.name);
                RCLCPP_ERROR(get_logger(), "%s", message.c_str());
                return controller_interface::return_type::ERROR;
              }
            }
          }
          else
          {
            message = fmt::format(
              FMT_COMPILE(
                "Controller with name '{}' cannot be activated, as not all of its fallback "
                "controller's : '{}' command interfaces are currently available!"),
              controller_it->info.name, fb_ctrl);
            RCLCPP_ERROR(get_logger(), "%s", message.c_str());
            return controller_interface::return_type::ERROR;
          }
        }
      }
      for (const auto & fb_state_itf : fb_ctrl_it->c->state_interface_configuration().names)
      {
        if (!resource_manager_->state_interface_is_available(fb_state_itf))
        {
          ControllersListIterator following_ctrl_it;
          if (is_interface_a_chained_interface(fb_state_itf, controllers, following_ctrl_it))
          {
            // if following_ctrl_it is inactive and it is in the fallback list of the
            // controller_it and then check it it's exported reference interface names list if
            // it's available
            if (is_controller_inactive(following_ctrl_it->c))
            {
              if (
                std::find(
                  controller_it->info.fallback_controllers_names.begin(),
                  controller_it->info.fallback_controllers_names.end(),
                  following_ctrl_it->info.name) !=
                controller_it->info.fallback_controllers_names.end())
              {
                const auto exported_state_itfs =
                  resource_manager_->get_controller_exported_state_interface_names(
                    following_ctrl_it->info.name);
                if (
                  std::find(exported_state_itfs.begin(), exported_state_itfs.end(), fb_state_itf) ==
                  exported_state_itfs.end())
                {
                  message = fmt::format(
                    FMT_COMPILE(
                      "Controller with name '{}' cannot be activated, as the state interface : "
                      "'{}' required by its fallback controller : '{}' is not exported by the "
                      "controller : '{}' in the current fallback list!"),
                    controller_it->info.name, fb_state_itf, fb_ctrl, following_ctrl_it->info.name);
                  RCLCPP_ERROR(get_logger(), "%s", message.c_str());
                  return controller_interface::return_type::ERROR;
                }
              }
              else
              {
                message = fmt::format(
                  FMT_COMPILE(
                    "Controller with name '{}' cannot be activated, as the state interface : '{}' "
                    "required by its fallback controller : '{}' is not available as the "
                    "controller is not in active state!. May be consider adding this controller to "
                    "the fallback list of the controller : '{}' or already have it activated."),
                  controller_it->info.name, fb_state_itf, fb_ctrl, following_ctrl_it->info.name);
                RCLCPP_ERROR(get_logger(), "%s", message.c_str());
                return controller_interface::return_type::ERROR;
              }
            }
          }
          else
          {
            message = fmt::format(
              FMT_COMPILE(
                "Controller with name '{}' cannot be activated, as not all of its fallback "
                "controller's : '{}' state interfaces are currently available!"),
              controller_it->info.name, fb_ctrl);
            RCLCPP_ERROR(get_logger(), "%s", message.c_str());
            return controller_interface::return_type::ERROR;
          }
        }
      }
    }
  }
  return controller_interface::return_type::OK;
}

void ControllerManager::publish_activity()
{
  controller_manager_msgs::msg::ControllerManagerActivity status_msg;
  status_msg.header.stamp = get_clock()->now();
  {
    // lock controllers
    std::lock_guard<std::recursive_mutex> guard(rt_controllers_wrapper_.controllers_lock_);
    const std::vector<ControllerSpec> & controllers =
      rt_controllers_wrapper_.get_updated_list(guard);
    for (const auto & controller : controllers)
    {
      controller_manager_msgs::msg::NamedLifecycleState lifecycle_info;
      lifecycle_info.name = controller.info.name;
      lifecycle_info.state.id = controller.c->get_lifecycle_state().id();
      lifecycle_info.state.label = controller.c->get_lifecycle_state().label();
      status_msg.controllers.push_back(lifecycle_info);
    }
  }
  {
    const auto hw_components_info = resource_manager_->get_components_status();
    for (const auto & [component_name, component_info] : hw_components_info)
    {
      controller_manager_msgs::msg::NamedLifecycleState lifecycle_info;
      lifecycle_info.name = component_name;
      lifecycle_info.state.id = component_info.state.id();
      lifecycle_info.state.label = component_info.state.label();
      status_msg.hardware_components.push_back(lifecycle_info);
    }
  }
  controller_manager_activity_publisher_->publish(status_msg);
}

controller_interface::return_type ControllerManager::check_for_interfaces_availability_to_activate(
  const std::vector<ControllerSpec> & controllers, const std::vector<std::string> activation_list,
  std::string & message)
{
  for (const auto & controller_name : activation_list)
  {
    auto controller_it = std::find_if(
      controllers.begin(), controllers.end(),
      std::bind(controller_name_compare, std::placeholders::_1, controller_name));
    if (controller_it == controllers.end())
    {
      message = fmt::format(
        FMT_COMPILE("Unable to find the controller : '{}' within the controller list"),
        controller_name);
      RCLCPP_ERROR(get_logger(), "%s", message.c_str());
      return controller_interface::return_type::ERROR;
    }
    const auto controller_cmd_interfaces =
      controller_it->c->command_interface_configuration().names;
    const auto controller_state_interfaces =
      controller_it->c->state_interface_configuration().names;

    // check if the interfaces are available in the first place
    for (const auto & cmd_itf : controller_cmd_interfaces)
    {
      if (!resource_manager_->command_interface_is_available(cmd_itf))
      {
        message = fmt::format(
          FMT_COMPILE(
            "Unable to activate controller '{}' since the "
            "command interface '{}' is not available."),
          controller_it->info.name, cmd_itf);
        RCLCPP_WARN(get_logger(), "%s", message.c_str());
        return controller_interface::return_type::ERROR;
      }
    }
    for (const auto & state_itf : controller_state_interfaces)
    {
      if (!resource_manager_->state_interface_is_available(state_itf))
      {
        message = fmt::format(
          FMT_COMPILE(
            "Unable to activate controller '{}' since the state interface '{}' is not available."),
          controller_it->info.name, state_itf);
        RCLCPP_WARN(get_logger(), "%s", message.c_str());
        return controller_interface::return_type::ERROR;
      }
    }
  }
  return controller_interface::return_type::OK;
}

void ControllerManager::controller_activity_diagnostic_callback(
  diagnostic_updater::DiagnosticStatusWrapper & stat)
{
  bool atleast_one_hw_active = false;
  const auto & hw_components_info = resource_manager_->get_components_status();
  for (const auto & [component_name, component_info] : hw_components_info)
  {
    if (component_info.state.id() == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE)
    {
      atleast_one_hw_active = true;
      break;
    }
  }
  // lock controllers
  std::lock_guard<std::recursive_mutex> guard(rt_controllers_wrapper_.controllers_lock_);
  const std::vector<ControllerSpec> & controllers = rt_controllers_wrapper_.get_updated_list(guard);
  bool all_active = true;
  const std::string periodicity_suffix = ".periodicity";
  const std::string exec_time_suffix = ".execution_time";
  const std::string state_suffix = ".state";

  if (cm_param_listener_->is_old(*params_))
  {
    *params_ = cm_param_listener_->get_params();
  }

  auto make_stats_string =
    [](const auto & statistics_data, const std::string & measurement_unit) -> std::string
  {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "Avg: " << statistics_data.average << " [" << statistics_data.min << " - "
        << statistics_data.max << "] " << measurement_unit
        << ", StdDev: " << statistics_data.standard_deviation;
    return oss.str();
  };

  // Variable to define the overall status of the controller diagnostics
  auto level = diagnostic_msgs::msg::DiagnosticStatus::OK;

  std::vector<std::string> high_exec_time_controllers;
  std::vector<std::string> bad_periodicity_async_controllers;
  for (size_t i = 0; i < controllers.size(); ++i)
  {
    const bool is_async = controllers[i].c->is_async();
    if (!is_controller_active(controllers[i].c))
    {
      all_active = false;
    }
    stat.add(
      controllers[i].info.name + state_suffix, controllers[i].c->get_lifecycle_state().label());
    if (is_controller_active(controllers[i].c))
    {
      const auto periodicity_stats = controllers[i].periodicity_statistics->GetStatistics();
      const auto exec_time_stats = controllers[i].execution_time_statistics->GetStatistics();
      stat.add(
        controllers[i].info.name + exec_time_suffix, make_stats_string(exec_time_stats, "us"));
      const bool publish_periodicity_stats =
        is_async || (controllers[i].c->get_update_rate() != this->get_update_rate());
      if (publish_periodicity_stats)
      {
        stat.add(
          controllers[i].info.name + periodicity_suffix,
          make_stats_string(periodicity_stats, "Hz") +
            " -> Desired : " + std::to_string(controllers[i].c->get_update_rate()) + " Hz");
        const double periodicity_error = std::abs(
          periodicity_stats.average - static_cast<double>(controllers[i].c->get_update_rate()));
        if (
          periodicity_error >
            params_->diagnostics.threshold.controllers.periodicity.mean_error.error ||
          periodicity_stats.standard_deviation >
            params_->diagnostics.threshold.controllers.periodicity.standard_deviation.error)
        {
          level = diagnostic_msgs::msg::DiagnosticStatus::ERROR;
          ros2_control::add_item(bad_periodicity_async_controllers, controllers[i].info.name);
        }
        else if (
          periodicity_error >
            params_->diagnostics.threshold.controllers.periodicity.mean_error.warn ||
          periodicity_stats.standard_deviation >
            params_->diagnostics.threshold.controllers.periodicity.standard_deviation.warn)
        {
          if (level != diagnostic_msgs::msg::DiagnosticStatus::ERROR)
          {
            level = diagnostic_msgs::msg::DiagnosticStatus::WARN;
          }
          ros2_control::add_item(bad_periodicity_async_controllers, controllers[i].info.name);
        }
      }
      const double max_exp_exec_time = is_async ? 1.e6 / controllers[i].c->get_update_rate() : 0.0;
      if (
        (exec_time_stats.average - max_exp_exec_time) >
          params_->diagnostics.threshold.controllers.execution_time.mean_error.error ||
        exec_time_stats.standard_deviation >
          params_->diagnostics.threshold.controllers.execution_time.standard_deviation.error)
      {
        level = diagnostic_msgs::msg::DiagnosticStatus::ERROR;
        high_exec_time_controllers.push_back(controllers[i].info.name);
      }
      else if (
        (exec_time_stats.average - max_exp_exec_time) >
          params_->diagnostics.threshold.controllers.execution_time.mean_error.warn ||
        exec_time_stats.standard_deviation >
          params_->diagnostics.threshold.controllers.execution_time.standard_deviation.warn)
      {
        if (level != diagnostic_msgs::msg::DiagnosticStatus::ERROR)
        {
          level = diagnostic_msgs::msg::DiagnosticStatus::WARN;
        }
        high_exec_time_controllers.push_back(controllers[i].info.name);
      }
    }
  }

  stat.summary(
    diagnostic_msgs::msg::DiagnosticStatus::OK,
    all_active ? "All controllers are active" : "Not all controllers are active");

  if (!high_exec_time_controllers.empty())
  {
    std::string high_exec_time_controllers_string;
    for (const auto & controller : high_exec_time_controllers)
    {
      high_exec_time_controllers_string.append(controller);
      high_exec_time_controllers_string.append(" ");
    }
    stat.mergeSummary(
      level,
      "\nHigh execution jitter or mean error : [ " + high_exec_time_controllers_string + "]");
  }
  if (!bad_periodicity_async_controllers.empty())
  {
    std::string bad_periodicity_async_controllers_string;
    for (const auto & controller : bad_periodicity_async_controllers)
    {
      bad_periodicity_async_controllers_string.append(controller);
      bad_periodicity_async_controllers_string.append(" ");
    }
    stat.mergeSummary(
      level, "\nHigh periodicity jitter or mean error : [ " +
               bad_periodicity_async_controllers_string + "]");
  }

  if (!atleast_one_hw_active)
  {
    stat.mergeSummary(
      diagnostic_msgs::msg::DiagnosticStatus::ERROR,
      "No hardware components are currently active to activate controllers");
  }
  else if (controllers.empty())
  {
    stat.mergeSummary(
      diagnostic_msgs::msg::DiagnosticStatus::WARN, "No controllers are currently loaded");
  }
}

void ControllerManager::hardware_components_diagnostic_callback(
  diagnostic_updater::DiagnosticStatusWrapper & stat)
{
  if (!is_resource_manager_initialized())
  {
    stat.summary(
      diagnostic_msgs::msg::DiagnosticStatus::ERROR, "Resource manager is not yet initialized!");
    return;
  }

  bool all_active = true;
  bool atleast_one_hw_active = false;
  const std::string read_cycle_suffix = ".read_cycle";
  const std::string write_cycle_suffix = ".write_cycle";
  const std::string state_suffix = ".state";
  const auto & hw_components_info = resource_manager_->get_components_status();
  for (const auto & [component_name, component_info] : hw_components_info)
  {
    if (component_info.state.id() != lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE)
    {
      all_active = false;
    }
    else
    {
      atleast_one_hw_active = true;
    }
  }
  if (hw_components_info.empty())
  {
    stat.summary(
      diagnostic_msgs::msg::DiagnosticStatus::ERROR, "No hardware components are loaded!");
    return;
  }
  else if (!atleast_one_hw_active)
  {
    stat.summary(
      diagnostic_msgs::msg::DiagnosticStatus::WARN, "No hardware components are currently active");
    return;
  }

  stat.summary(
    diagnostic_msgs::msg::DiagnosticStatus::OK,
    all_active ? "All hardware components are active" : "Not all hardware components are active");

  if (cm_param_listener_->is_old(*params_))
  {
    *params_ = cm_param_listener_->get_params();
  }

  auto make_stats_string =
    [](const auto & statistics_data, const std::string & measurement_unit) -> std::string
  {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "Avg: " << statistics_data.average << " [" << statistics_data.min << " - "
        << statistics_data.max << "] " << measurement_unit
        << ", StdDev: " << statistics_data.standard_deviation;
    return oss.str();
  };

  // Variable to define the overall status of the controller diagnostics
  auto level = diagnostic_msgs::msg::DiagnosticStatus::OK;

  std::vector<std::string> high_exec_time_hw;
  std::vector<std::string> bad_periodicity_async_hw;

  for (const auto & [component_name, component_info] : hw_components_info)
  {
    stat.add(component_name + state_suffix, component_info.state.label());
    if (component_info.state.id() != lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE)
    {
      all_active = false;
    }
    else
    {
      atleast_one_hw_active = true;
    }
    if (component_info.state.id() == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE)
    {
      auto update_stats =
        [&bad_periodicity_async_hw, &high_exec_time_hw, &stat, &make_stats_string, this](
          const std::string & comp_name, const auto & statistics,
          const std::string & statistics_type_suffix, auto & diag_level, const auto & comp_info,
          const auto & params)
      {
        if (!statistics)
        {
          return;
        }
        const bool is_async = comp_info.is_async;
        const std::string periodicity_suffix = ".periodicity";
        const std::string exec_time_suffix = ".execution_time";
        const auto periodicity_stats = statistics->periodicity.get_statistics();
        const auto exec_time_stats = statistics->execution_time.get_statistics();
        stat.add(
          comp_name + statistics_type_suffix + exec_time_suffix,
          make_stats_string(exec_time_stats, "us"));
        const bool publish_periodicity_stats =
          is_async || (comp_info.rw_rate != this->get_update_rate());
        if (publish_periodicity_stats)
        {
          stat.add(
            comp_name + statistics_type_suffix + periodicity_suffix,
            make_stats_string(periodicity_stats, "Hz") +
              " -> Desired : " + std::to_string(comp_info.rw_rate) + " Hz");
          const double periodicity_error =
            std::abs(periodicity_stats.average - static_cast<double>(comp_info.rw_rate));
          if (
            periodicity_error >
              params->diagnostics.threshold.hardware_components.periodicity.mean_error.error ||
            periodicity_stats.standard_deviation > params->diagnostics.threshold.hardware_components
                                                     .periodicity.standard_deviation.error)
          {
            diag_level = diagnostic_msgs::msg::DiagnosticStatus::ERROR;
            ros2_control::add_item(bad_periodicity_async_hw, comp_name);
          }
          else if (
            periodicity_error >
              params->diagnostics.threshold.hardware_components.periodicity.mean_error.warn ||
            periodicity_stats.standard_deviation >
              params->diagnostics.threshold.hardware_components.periodicity.standard_deviation.warn)
          {
            if (diag_level != diagnostic_msgs::msg::DiagnosticStatus::ERROR)
            {
              diag_level = diagnostic_msgs::msg::DiagnosticStatus::WARN;
            }
            ros2_control::add_item(bad_periodicity_async_hw, comp_name);
          }
        }
        const double max_exp_exec_time =
          is_async ? 1.e6 / static_cast<double>(comp_info.rw_rate) : 0.0;
        if (
          (exec_time_stats.average - max_exp_exec_time) >
            params->diagnostics.threshold.hardware_components.execution_time.mean_error.error ||
          exec_time_stats.standard_deviation > params->diagnostics.threshold.hardware_components
                                                 .execution_time.standard_deviation.error)
        {
          diag_level = diagnostic_msgs::msg::DiagnosticStatus::ERROR;
          high_exec_time_hw.push_back(comp_name);
        }
        else if (
          (exec_time_stats.average - max_exp_exec_time) >
            params->diagnostics.threshold.hardware_components.execution_time.mean_error.warn ||
          exec_time_stats.standard_deviation > params->diagnostics.threshold.hardware_components
                                                 .execution_time.standard_deviation.warn)
        {
          if (diag_level != diagnostic_msgs::msg::DiagnosticStatus::ERROR)
          {
            diag_level = diagnostic_msgs::msg::DiagnosticStatus::WARN;
          }
          high_exec_time_hw.push_back(comp_name);
        }
      };

      // For components : {actuator, sensor and system}
      update_stats(
        component_name, component_info.read_statistics, read_cycle_suffix, level, component_info,
        params_);
      // For components : {actuator and system}
      update_stats(
        component_name, component_info.write_statistics, write_cycle_suffix, level, component_info,
        params_);
    }
  }

  if (!high_exec_time_hw.empty())
  {
    std::string high_exec_time_hw_string;
    for (const auto & hw_comp : high_exec_time_hw)
    {
      high_exec_time_hw_string.append(hw_comp);
      high_exec_time_hw_string.append(" ");
    }
    stat.mergeSummary(
      level, "\nHigh execution jitter or mean error : [ " + high_exec_time_hw_string + "]");
  }
  if (!bad_periodicity_async_hw.empty())
  {
    std::string bad_periodicity_async_hw_string;
    for (const auto & hw_comp : bad_periodicity_async_hw)
    {
      bad_periodicity_async_hw_string.append(hw_comp);
      bad_periodicity_async_hw_string.append(" ");
    }
    stat.mergeSummary(
      level,
      "\nHigh periodicity jitter or mean error : [ " + bad_periodicity_async_hw_string + "]");
  }
}

void ControllerManager::controller_manager_diagnostic_callback(
  diagnostic_updater::DiagnosticStatusWrapper & stat)
{
  const std::string periodicity_stat_name = "periodicity";
  const auto cm_stats = periodicity_stats_.GetStatistics();
  stat.add("update_rate", std::to_string(get_update_rate()));
  stat.add(periodicity_stat_name + ".average", std::to_string(cm_stats.average));
  stat.add(
    periodicity_stat_name + ".standard_deviation", std::to_string(cm_stats.standard_deviation));
  stat.add(periodicity_stat_name + ".min", std::to_string(cm_stats.min));
  stat.add(periodicity_stat_name + ".max", std::to_string(cm_stats.max));
  if (is_resource_manager_initialized())
  {
    stat.summary(diagnostic_msgs::msg::DiagnosticStatus::OK, "Controller Manager is running");
  }
  else
  {
    if (robot_description_.empty())
    {
      stat.summary(
        diagnostic_msgs::msg::DiagnosticStatus::WARN, "Waiting for robot description....");
    }
    else
    {
      stat.summary(
        diagnostic_msgs::msg::DiagnosticStatus::ERROR,
        "Resource Manager is not initialized properly!");
    }
  }

  const double periodicity_error = std::abs(cm_stats.average - get_update_rate());
  const std::string diag_summary = fmt::format(
    FMT_COMPILE("Controller Manager has bad periodicity : {} Hz. Expected consistent {} Hz"),
    cm_stats.average, get_update_rate());
  if (
    periodicity_error >
      params_->diagnostics.threshold.controller_manager.periodicity.mean_error.error ||
    cm_stats.standard_deviation >
      params_->diagnostics.threshold.controller_manager.periodicity.standard_deviation.error)
  {
    stat.mergeSummary(diagnostic_msgs::msg::DiagnosticStatus::ERROR, diag_summary);
  }
  else if (
    periodicity_error >
      params_->diagnostics.threshold.controller_manager.periodicity.mean_error.warn ||
    cm_stats.standard_deviation >
      params_->diagnostics.threshold.controller_manager.periodicity.standard_deviation.warn)
  {
    stat.mergeSummary(diagnostic_msgs::msg::DiagnosticStatus::WARN, diag_summary);
  }
}

void ControllerManager::update_list_with_controller_chain(
  const std::string & ctrl_name, std::vector<std::string>::iterator controller_iterator,
  bool append_to_controller)
{
  auto new_ctrl_it =
    std::find(ordered_controllers_names_.begin(), ordered_controllers_names_.end(), ctrl_name);
  if (new_ctrl_it == ordered_controllers_names_.end())
  {
    RCLCPP_DEBUG(get_logger(), "Adding controller chain : %s", ctrl_name.c_str());

    auto iterator = controller_iterator;
    for (const auto & ctrl : controller_chain_spec_[ctrl_name].following_controllers)
    {
      auto it =
        std::find(ordered_controllers_names_.begin(), ordered_controllers_names_.end(), ctrl);
      if (it != ordered_controllers_names_.end())
      {
        if (
          std::distance(ordered_controllers_names_.begin(), it) <
          std::distance(ordered_controllers_names_.begin(), iterator))
        {
          iterator = it;
        }
      }
    }
    for (const auto & ctrl : controller_chain_spec_[ctrl_name].preceding_controllers)
    {
      auto it =
        std::find(ordered_controllers_names_.begin(), ordered_controllers_names_.end(), ctrl);
      if (it != ordered_controllers_names_.end())
      {
        if (
          std::distance(ordered_controllers_names_.begin(), it) >
          std::distance(ordered_controllers_names_.begin(), iterator))
        {
          iterator = it;
        }
      }
    }

    if (append_to_controller)
    {
      ordered_controllers_names_.insert(iterator + 1, ctrl_name);
    }
    else
    {
      ordered_controllers_names_.insert(iterator, ctrl_name);
    }

    RCLCPP_DEBUG_EXPRESSION(
      get_logger(), !controller_chain_spec_[ctrl_name].following_controllers.empty(),
      "\t[%s] Following controllers : %ld", ctrl_name.c_str(),
      controller_chain_spec_[ctrl_name].following_controllers.size());
    for (const std::string & flwg_ctrl : controller_chain_spec_[ctrl_name].following_controllers)
    {
      new_ctrl_it =
        std::find(ordered_controllers_names_.begin(), ordered_controllers_names_.end(), ctrl_name);
      RCLCPP_DEBUG(get_logger(), "\t\t[%s] : %s", ctrl_name.c_str(), flwg_ctrl.c_str());
      update_list_with_controller_chain(flwg_ctrl, new_ctrl_it, true);
    }
    RCLCPP_DEBUG_EXPRESSION(
      get_logger(), !controller_chain_spec_[ctrl_name].preceding_controllers.empty(),
      "\t[%s] Preceding controllers : %ld", ctrl_name.c_str(),
      controller_chain_spec_[ctrl_name].preceding_controllers.size());
    for (const std::string & preced_ctrl : controller_chain_spec_[ctrl_name].preceding_controllers)
    {
      new_ctrl_it =
        std::find(ordered_controllers_names_.begin(), ordered_controllers_names_.end(), ctrl_name);
      RCLCPP_DEBUG(get_logger(), "\t\t[%s]: %s", ctrl_name.c_str(), preced_ctrl.c_str());
      update_list_with_controller_chain(preced_ctrl, new_ctrl_it, false);
    }
  }
}

rclcpp::NodeOptions ControllerManager::determine_controller_node_options(
  const ControllerSpec & controller) const
{
  auto check_for_element = [](const auto & list, const auto & element)
  { return std::find(list.begin(), list.end(), element) != list.end(); };

  rclcpp::NodeOptions controller_node_options = controller.c->define_custom_node_options();
  std::vector<std::string> node_options_arguments = controller_node_options.arguments();

  for (const std::string & arg : cm_node_options_.arguments())
  {
    if (
      arg.find("__ns") != std::string::npos || arg.find("__node") != std::string::npos ||
      arg.find("robot_description") != std::string::npos)
    {
      if (
        node_options_arguments.back() == RCL_REMAP_FLAG ||
        node_options_arguments.back() == RCL_SHORT_REMAP_FLAG ||
        node_options_arguments.back() == RCL_PARAM_FLAG ||
        node_options_arguments.back() == RCL_SHORT_PARAM_FLAG)
      {
        node_options_arguments.pop_back();
      }
      continue;
    }

    node_options_arguments.push_back(arg);
  }

  // Add deprecation notice if the arguments are from the controller_manager node
  if (
    check_for_element(node_options_arguments, RCL_REMAP_FLAG) ||
    check_for_element(node_options_arguments, RCL_SHORT_REMAP_FLAG))
  {
    RCLCPP_WARN(
      get_logger(),
      "The use of remapping arguments to the controller_manager node is deprecated. Please use the "
      "'--controller-ros-args' argument of the spawner to pass remapping arguments to the "
      "controller node.");
  }

  for (const auto & parameters_file : controller.info.parameters_files)
  {
    if (!check_for_element(node_options_arguments, RCL_ROS_ARGS_FLAG))
    {
      node_options_arguments.push_back(RCL_ROS_ARGS_FLAG);
    }
    node_options_arguments.push_back(RCL_PARAM_FILE_FLAG);
    node_options_arguments.push_back(parameters_file);
  }

  // ensure controller's `use_sim_time` parameter matches controller_manager's
  const rclcpp::Parameter use_sim_time = this->get_parameter("use_sim_time");
  if (use_sim_time.as_bool())
  {
    if (!check_for_element(node_options_arguments, RCL_ROS_ARGS_FLAG))
    {
      node_options_arguments.push_back(RCL_ROS_ARGS_FLAG);
    }
    node_options_arguments.push_back(RCL_PARAM_FLAG);
    node_options_arguments.push_back("use_sim_time:=true");
  }

  // Add options parsed through the spawner
  if (
    !controller.info.node_options_args.empty() &&
    !check_for_element(controller.info.node_options_args, RCL_ROS_ARGS_FLAG))
  {
    node_options_arguments.push_back(RCL_ROS_ARGS_FLAG);
  }
  for (const auto & arg : controller.info.node_options_args)
  {
    node_options_arguments.push_back(arg);
  }

  std::string arguments;
  arguments.reserve(1000);
  for (const auto & arg : node_options_arguments)
  {
    arguments.append(arg);
    arguments.append(" ");
  }
  RCLCPP_INFO(
    get_logger(), "Controller '%s' node arguments: %s", controller.info.name.c_str(),
    arguments.c_str());

  controller_node_options = controller_node_options.arguments(node_options_arguments);
  controller_node_options.use_global_arguments(false);
  return controller_node_options;
}

void ControllerManager::cleanup_controller_exported_interfaces(const ControllerSpec & controller)
{
  if (is_controller_inactive(controller.c) && controller.c->is_chainable())
  {
    RCLCPP_DEBUG(
      get_logger(), "Removing controller '%s' exported interfaces from resource manager.",
      controller.info.name.c_str());
    resource_manager_->remove_controller_exported_state_interfaces(controller.info.name);
    resource_manager_->remove_controller_reference_interfaces(controller.info.name);
  }
}

}  // namespace controller_manager
