// Copyright 2018, Bosch Software Innovations GmbH.
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

#include "rosbag2_transport/player.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rcl/graph.h"

#include "rclcpp/rclcpp.hpp"

#include "rcutils/time.h"

#include "rosbag2_cpp/clocks/time_controller_clock.hpp"
#include "rosbag2_cpp/reader.hpp"
#include "rosbag2_cpp/typesupport_helpers.hpp"

#include "rosbag2_storage/storage_filter.hpp"

#include "qos.hpp"

namespace
{
/**
 * Trivial std::unique_lock wrapper providing constructor that allows Clang Thread Safety Analysis.
 * The std::unique_lock does not have these annotations.
 */
class RCPPUTILS_TSA_SCOPED_CAPABILITY TSAUniqueLock : public std::unique_lock<std::mutex>
{
public:
  explicit TSAUniqueLock(std::mutex & mu) RCPPUTILS_TSA_ACQUIRE(mu)
  : std::unique_lock<std::mutex>(mu)
  {}

  ~TSAUniqueLock() RCPPUTILS_TSA_RELEASE() {}
};

/**
 * Determine which QoS to offer for a topic.
 * The priority of the profile selected is:
 *   1. The override specified in play_options (if one exists for the topic).
 *   2. A profile automatically adapted to the recorded QoS profiles of publishers on the topic.
 *
 * \param topic_name The full name of the topic, with namespace (ex. /arm/joint_status).
 * \param topic_qos_profile_overrides A map of topic to QoS profile overrides.
 * @return The QoS profile to be used for subscribing.
 */
rclcpp::QoS publisher_qos_for_topic(
  const rosbag2_storage::TopicMetadata & topic,
  const std::unordered_map<std::string, rclcpp::QoS> & topic_qos_profile_overrides,
  const rclcpp::Logger & logger)
{
  using rosbag2_transport::Rosbag2QoS;
  auto qos_it = topic_qos_profile_overrides.find(topic.name);
  if (qos_it != topic_qos_profile_overrides.end()) {
    RCLCPP_INFO_STREAM(
      logger,
      "Overriding QoS profile for topic " << topic.name);
    return Rosbag2QoS{qos_it->second};
  } else if (topic.offered_qos_profiles.empty()) {
    return Rosbag2QoS{};
  }

  const auto profiles_yaml = YAML::Load(topic.offered_qos_profiles);
  const auto offered_qos_profiles = profiles_yaml.as<std::vector<Rosbag2QoS>>();
  return Rosbag2QoS::adapt_offer_to_recorded_offers(topic.name, offered_qos_profiles);
}
}  // namespace

namespace rosbag2_transport
{

Player::Player(const std::string & node_name, const rclcpp::NodeOptions & node_options)
: rclcpp::Node(node_name, node_options)
{
  // TODO(karsten1987): Use this constructor later with parameter parsing.
  // The reader, storage_options as well as play_options can be loaded via parameter.
  // That way, the player can be used as a simple component in a component manager.
  throw rclcpp::exceptions::UnimplementedError();
}

Player::Player(
  const rosbag2_storage::StorageOptions & storage_options,
  const rosbag2_transport::PlayOptions & play_options,
  const std::string & node_name,
  const rclcpp::NodeOptions & node_options)
: Player(std::make_unique<rosbag2_cpp::Reader>(),
    storage_options, play_options,
    node_name, node_options)
{}

Player::Player(
  std::unique_ptr<rosbag2_cpp::Reader> reader,
  const rosbag2_storage::StorageOptions & storage_options,
  const rosbag2_transport::PlayOptions & play_options,
  const std::string & node_name,
  const rclcpp::NodeOptions & node_options)
: Player(std::move(reader),
    // only call KeyboardHandler when using default keyboard handler implementation
    std::shared_ptr<KeyboardHandler>(new KeyboardHandler()),
    storage_options, play_options,
    node_name, node_options)
{}

Player::Player(
  std::unique_ptr<rosbag2_cpp::Reader> reader,
  std::shared_ptr<KeyboardHandler> keyboard_handler,
  const rosbag2_storage::StorageOptions & storage_options,
  const rosbag2_transport::PlayOptions & play_options,
  const std::string & node_name,
  const rclcpp::NodeOptions & node_options)
: rclcpp::Node(
    node_name,
    rclcpp::NodeOptions(node_options).arguments(play_options.topic_remapping_options)),
  storage_options_(storage_options),
  play_options_(play_options),
  keyboard_handler_(keyboard_handler)
{
  {
    std::lock_guard<std::mutex> lk(reader_mutex_);
    reader_ = std::move(reader);
    reader_->open(storage_options_, {"", rmw_get_serialization_format()});
    const auto starting_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
      reader_->get_metadata().starting_time.time_since_epoch()).count();
    clock_ = std::make_unique<rosbag2_cpp::TimeControllerClock>(starting_time);
    set_rate(play_options_.rate);

    topic_qos_profile_overrides_ = play_options_.topic_qos_profile_overrides;
    prepare_publishers();

    reader_->close();
  }
  // service callbacks
  srv_pause_ = create_service<rosbag2_interfaces::srv::Pause>(
    "~/pause",
    [this](
      const std::shared_ptr<rmw_request_id_t>/* request_header */,
      const std::shared_ptr<rosbag2_interfaces::srv::Pause::Request>/* request */,
      const std::shared_ptr<rosbag2_interfaces::srv::Pause::Response>/* response */)
    {
      pause();
    });
  srv_resume_ = create_service<rosbag2_interfaces::srv::Resume>(
    "~/resume",
    [this](
      const std::shared_ptr<rmw_request_id_t>/* request_header */,
      const std::shared_ptr<rosbag2_interfaces::srv::Resume::Request>/* request */,
      const std::shared_ptr<rosbag2_interfaces::srv::Resume::Response>/* response */)
    {
      resume();
    });
  srv_toggle_paused_ = create_service<rosbag2_interfaces::srv::TogglePaused>(
    "~/toggle_paused",
    [this](
      const std::shared_ptr<rmw_request_id_t>/* request_header */,
      const std::shared_ptr<rosbag2_interfaces::srv::TogglePaused::Request>/* request */,
      const std::shared_ptr<rosbag2_interfaces::srv::TogglePaused::Response>/* response */)
    {
      toggle_paused();
    });
  srv_is_paused_ = create_service<rosbag2_interfaces::srv::IsPaused>(
    "~/is_paused",
    [this](
      const std::shared_ptr<rmw_request_id_t>/* request_header */,
      const std::shared_ptr<rosbag2_interfaces::srv::IsPaused::Request>/* request */,
      const std::shared_ptr<rosbag2_interfaces::srv::IsPaused::Response> response)
    {
      response->paused = is_paused();
    });
  srv_get_rate_ = create_service<rosbag2_interfaces::srv::GetRate>(
    "~/get_rate",
    [this](
      const std::shared_ptr<rmw_request_id_t>/* request_header */,
      const std::shared_ptr<rosbag2_interfaces::srv::GetRate::Request>/* request */,
      const std::shared_ptr<rosbag2_interfaces::srv::GetRate::Response> response)
    {
      response->rate = get_rate();
    });
  srv_set_rate_ = create_service<rosbag2_interfaces::srv::SetRate>(
    "~/set_rate",
    [this](
      const std::shared_ptr<rmw_request_id_t>/* request_header */,
      const std::shared_ptr<rosbag2_interfaces::srv::SetRate::Request> request,
      const std::shared_ptr<rosbag2_interfaces::srv::SetRate::Response> response)
    {
      response->success = set_rate(request->rate);
    });
  srv_play_next_ = create_service<rosbag2_interfaces::srv::PlayNext>(
    "~/play_next",
    [this](
      const std::shared_ptr<rmw_request_id_t>/* request_header */,
      const std::shared_ptr<rosbag2_interfaces::srv::PlayNext::Request>/* request */,
      const std::shared_ptr<rosbag2_interfaces::srv::PlayNext::Response> response)
    {
      response->success = play_next();
    });
  // keyboard callbacks
  add_keyboard_callbacks();
}

Player::~Player()
{
  // remove callbacks on key_codes to prevent race conditions
  // Note: keyboard_handler handles locks between removing & executing callbacks
  for (auto cb_handle : keyboard_callbacks_) {
    keyboard_handler_->delete_key_press_callback(cb_handle);
  }
  // closes reader
  std::lock_guard<std::mutex> lk(reader_mutex_);
  if (reader_) {
    reader_->close();
  }
}

rosbag2_cpp::Reader * Player::release_reader()
{
  std::lock_guard<std::mutex> lk(reader_mutex_);
  reader_->close();
  return reader_.release();
}

const std::chrono::milliseconds
Player::queue_read_wait_period_ = std::chrono::milliseconds(100);

bool Player::is_storage_completely_loaded() const
{
  if (storage_loading_future_.valid() &&
    storage_loading_future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
  {
    storage_loading_future_.get();
  }
  return !storage_loading_future_.valid();
}

void Player::play()
{
  rclcpp::Duration delay(0, 0);
  if (play_options_.delay >= rclcpp::Duration(0, 0)) {
    delay = play_options_.delay;
  } else {
    RCLCPP_WARN_STREAM(
      get_logger(),
      "Invalid delay value: " << play_options_.delay.nanoseconds() << ". Delay is disabled.");
  }

  try {
    do {
      if (delay > rclcpp::Duration(0, 0)) {
        RCLCPP_INFO_STREAM(get_logger(), "Sleep " << delay.nanoseconds() << " ns");
        std::chrono::nanoseconds duration(delay.nanoseconds());
        std::this_thread::sleep_for(duration);
      }
      rcutils_time_point_value_t starting_time = 0;
      {
        std::lock_guard<std::mutex> lk(reader_mutex_);
        reader_->open(storage_options_, {"", rmw_get_serialization_format()});
        starting_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
          reader_->get_metadata().starting_time.time_since_epoch()).count();
      }
      clock_->jump(starting_time);

      storage_loading_future_ = std::async(std::launch::async, [this]() {load_storage_content();});

      wait_for_filled_queue();
      play_messages_from_queue();
      {
        std::lock_guard<std::mutex> lk(ready_to_play_from_queue_mutex_);
        is_ready_to_play_from_queue_ = false;
        ready_to_play_from_queue_cv_.notify_all();
      }
      std::lock_guard<std::mutex> lk(reader_mutex_);
      reader_->close();
    } while (rclcpp::ok() && play_options_.loop);
  } catch (std::runtime_error & e) {
    RCLCPP_ERROR(get_logger(), "Failed to play: %s", e.what());
  }
  std::lock_guard<std::mutex> lk(ready_to_play_from_queue_mutex_);
  is_ready_to_play_from_queue_ = false;
  ready_to_play_from_queue_cv_.notify_all();
}

void Player::pause()
{
  clock_->pause();
  RCLCPP_INFO_STREAM(get_logger(), "Pausing play.");
}

void Player::resume()
{
  clock_->resume();
  RCLCPP_INFO_STREAM(get_logger(), "Resuming play.");
}

void Player::toggle_paused()
{
  is_paused() ? resume() : pause();
}

bool Player::is_paused() const
{
  return clock_->is_paused();
}

double Player::get_rate() const
{
  return clock_->get_rate();
}

bool Player::set_rate(double rate)
{
  return clock_->set_rate(rate);
}

rosbag2_storage::SerializedBagMessageSharedPtr * Player::peek_next_message_from_queue()
{
  rosbag2_storage::SerializedBagMessageSharedPtr * message_ptr = message_queue_.peek();
  if (message_ptr == nullptr && !is_storage_completely_loaded() && rclcpp::ok()) {
    RCLCPP_WARN(
      get_logger(),
      "Message queue starved. Messages will be delayed. Consider "
      "increasing the --read-ahead-queue-size option.");
    while (message_ptr == nullptr && !is_storage_completely_loaded() && rclcpp::ok()) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      message_ptr = message_queue_.peek();
    }
  }
  // Workaround for race condition between peek and is_storage_completely_loaded()
  // Don't sync with mutex for the sake of the performance
  if (message_ptr == nullptr) {
    message_ptr = message_queue_.peek();
  }
  return message_ptr;
}

bool Player::play_next()
{
  if (!clock_->is_paused()) {
    RCLCPP_WARN_STREAM(get_logger(), "Called play next, but not in paused state.");
    return false;
  }

  RCLCPP_INFO_STREAM(get_logger(), "Playing next message.");

  // Temporary take over playback from play_messages_from_queue()
  std::lock_guard<std::mutex> main_play_loop_lk(skip_message_in_main_play_loop_mutex_);
  skip_message_in_main_play_loop_ = true;
  // Wait for player to be ready for playback messages from queue i.e. wait for Player:play() to
  // be called if not yet and queue to be filled with messages.
  {
    std::unique_lock<std::mutex> lk(ready_to_play_from_queue_mutex_);
    ready_to_play_from_queue_cv_.wait(lk, [this] {return is_ready_to_play_from_queue_;});
  }
  rosbag2_storage::SerializedBagMessageSharedPtr * message_ptr = peek_next_message_from_queue();

  bool next_message_published = false;
  while (message_ptr != nullptr && !next_message_published) {
    {
      rosbag2_storage::SerializedBagMessageSharedPtr message = *message_ptr;
      next_message_published = publish_message(message);
      clock_->jump(message->time_stamp);
    }
    message_queue_.pop();
    message_ptr = peek_next_message_from_queue();
  }
  return next_message_published;
}

void Player::restore_message_queue_from_storage(
  rosbag2_storage::SerializedBagMessageSharedPtr origin_message)
{
  if (!origin_message) {return;}
  reader_->close();
  reader_->open(storage_options_, {"", rmw_get_serialization_format()});
  rosbag2_storage::SerializedBagMessageSharedPtr current_message = reader_->read_next();
  while (current_message->time_stamp != origin_message->time_stamp) {
    current_message = reader_->read_next();
  }
  if (current_message->time_stamp != origin_message->time_stamp) {
    throw std::runtime_error("Fail to restore message queue");
  }
  message_queue_.enqueue(current_message);
  enqueue_up_to_boundary(play_options_.read_ahead_queue_size);  // refill queue
}

bool Player::seek(rcutils_time_point_value_t time_point)
{
  // Temporary stop playback in play_messages_from_queue() and block play_next()
  std::lock_guard<std::mutex> main_play_loop_lk(skip_message_in_main_play_loop_mutex_);
  skip_message_in_main_play_loop_ = true;
  // Wait for player to be ready for playback messages from queue i.e. wait for Player:play() to
  // be called if not yet and queue to be filled with messages.
  {
    std::unique_lock<std::mutex> lk(ready_to_play_from_queue_mutex_);
    ready_to_play_from_queue_cv_.wait(lk, [this] {return is_ready_to_play_from_queue_;});
  }

  cancel_wait_for_next_message_ = true;
  bool message_found = false;
  rosbag2_storage::SerializedBagMessageSharedPtr * message_ptr = peek_next_message_from_queue();
  rosbag2_storage::SerializedBagMessageSharedPtr message_before_seek;
  if (message_ptr != nullptr) {
    message_before_seek = *message_ptr;
  }

  {  // Update message queue and adjust reader pointer
    std::lock_guard<std::mutex> lk(reader_mutex_);
    // Read next current_message
    rosbag2_storage::SerializedBagMessageSharedPtr current_message;
    if (!message_queue_.try_dequeue(current_message)) {
      if (!reader_->has_next()) {
        reader_->close();
        reader_->open(storage_options_, {"", rmw_get_serialization_format()});
        if (!reader_->has_next()) {
          return false;  // The bag is empty
        }
        // else means that we were at the end of the bag when we were started and will try to
        // search from the beginning
        assert(is_storage_completely_loaded());
        storage_loading_future_ =
          std::async(std::launch::async, [this]() {load_storage_content();});
      }
      current_message = reader_->read_next();
    }

    const bool jump_forward = (time_point >= current_message->time_stamp);

    if (jump_forward) {
      if (current_message->time_stamp >= time_point) {
        message_found = true;
      }
      // Pop up current_message queue trying to find current_message with needed timestamp
      while (!message_found && message_queue_.try_dequeue(current_message)) {
        if (current_message->time_stamp >= time_point) {
          message_found = true;
        }
      }
      while (!message_found && reader_->has_next()) {
        current_message = reader_->read_next();
        if (current_message->time_stamp >= time_point) {
          message_found = true;
        }
      }
      if (message_found) {
        // Enqueue current_message and rest of the queue to the queue again to preserve order of
        // messages
        if (message_queue_.peek() != nullptr) {  // if message_queue_ not empty
          decltype(message_queue_) temp_message_queue;
          temp_message_queue.enqueue(current_message);
          // Copy residual messages from message_queue_ to the temp_message_queue
          while (message_queue_.try_dequeue(current_message)) {
            temp_message_queue.enqueue(current_message);
          }
          // Copy all messages back to the message_queue_
          while (temp_message_queue.try_dequeue(current_message)) {
            message_queue_.enqueue(current_message);
          }
          enqueue_up_to_boundary(play_options_.read_ahead_queue_size);  // refill queue
        } else {
          message_queue_.enqueue(current_message);
          enqueue_up_to_boundary(play_options_.read_ahead_queue_size);  // refill queue
        }
      } else {
        restore_message_queue_from_storage(message_before_seek);
      }
    } else {  // jump_backward
      // Purge queue
      while (message_queue_.pop()) {}
      // Reopen bag
      reader_->close();
      reader_->open(storage_options_, {"", rmw_get_serialization_format()});

      // Try to find current_message in bag with timestamp >= time_point
      while (!message_found && reader_->has_next()) {
        current_message = reader_->read_next();
        if (current_message->time_stamp >= time_point) {
          message_found = true;
        }
      }
      if (message_found) {
        message_queue_.enqueue(current_message);
        enqueue_up_to_boundary(play_options_.read_ahead_queue_size);  // refill queue
      } else {
        restore_message_queue_from_storage(message_before_seek);
      }
    }
  }

  if (message_found) {
    clock_->jump(time_point);
    return true;
  } else {
    return false;
  }
}

void Player::wait_for_filled_queue() const
{
  while (
    message_queue_.size_approx() < play_options_.read_ahead_queue_size &&
    !is_storage_completely_loaded() && rclcpp::ok())
  {
    std::this_thread::sleep_for(queue_read_wait_period_);
  }
}

void Player::load_storage_content()
{
  auto queue_lower_boundary =
    static_cast<size_t>(play_options_.read_ahead_queue_size * read_ahead_lower_bound_percentage_);
  auto queue_upper_boundary = play_options_.read_ahead_queue_size;

  while (rclcpp::ok()) {
    TSAUniqueLock lk(reader_mutex_);
    if (!reader_->has_next()) {break;}

    if (message_queue_.size_approx() < queue_lower_boundary) {
      enqueue_up_to_boundary(queue_upper_boundary);
    } else {
      lk.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

void Player::enqueue_up_to_boundary(size_t boundary)
{
  rosbag2_storage::SerializedBagMessageSharedPtr message;
  for (size_t i = message_queue_.size_approx(); i < boundary; i++) {
    if (!reader_->has_next()) {
      break;
    }
    message = reader_->read_next();
    message_queue_.enqueue(message);
  }
}

void Player::play_messages_from_queue()
{
  // Note: We need to use message_queue_.peek() instead of message_queue_.try_dequeue(message)
  // to support play_next() API logic.
  rosbag2_storage::SerializedBagMessageSharedPtr * message_ptr = peek_next_message_from_queue();
  { // Notify play_next() that we are ready for playback
    // Note: We should do notification that we are ready for playback after peeking pointer to
    // the next message. message_queue_.peek() is not allowed to be called from more than one
    // thread concurrently.
    std::lock_guard<std::mutex> lk(ready_to_play_from_queue_mutex_);
    is_ready_to_play_from_queue_ = true;
    ready_to_play_from_queue_cv_.notify_all();
  }
  while (message_ptr != nullptr && rclcpp::ok()) {
    rosbag2_storage::SerializedBagMessageSharedPtr message = *message_ptr;
    // Do not move on until sleep_until returns true
    // It will always sleep, so this is not a tight busy loop on pause
    while (rclcpp::ok() && !clock_->sleep_until(message->time_stamp)) {
      if (std::atomic_exchange(&cancel_wait_for_next_message_, false)) {
        break;
      }
    }
    std::lock_guard<std::mutex> lk(skip_message_in_main_play_loop_mutex_);
    if (rclcpp::ok()) {
      if (skip_message_in_main_play_loop_) {
        skip_message_in_main_play_loop_ = false;
        cancel_wait_for_next_message_ = false;
        message_ptr = peek_next_message_from_queue();
        continue;
      }
      publish_message(message);
    }
    message_queue_.pop();
    message_ptr = peek_next_message_from_queue();
  }
}

void Player::prepare_publishers()
{
  rosbag2_storage::StorageFilter storage_filter;
  storage_filter.topics = play_options_.topics_to_filter;
  reader_->set_filter(storage_filter);

  // Create /clock publisher
  if (play_options_.clock_publish_frequency > 0.f) {
    const auto publish_period = std::chrono::nanoseconds(
      static_cast<uint64_t>(RCUTILS_S_TO_NS(1) / play_options_.clock_publish_frequency));
    // NOTE: PlayerClock does not own this publisher because rosbag2_cpp
    // should not own transport-based functionality
    clock_publisher_ = create_publisher<rosgraph_msgs::msg::Clock>(
      "/clock", rclcpp::ClockQoS());
    clock_publish_timer_ = create_wall_timer(
      publish_period, [this]() {
        auto msg = rosgraph_msgs::msg::Clock();
        msg.clock = rclcpp::Time(clock_->now());
        clock_publisher_->publish(msg);
      });
  }

  // Create topic publishers
  auto topics = reader_->get_all_topics_and_types();
  for (const auto & topic : topics) {
    if (publishers_.find(topic.name) != publishers_.end()) {
      continue;
    }
    // filter topics to add publishers if necessary
    auto & filter_topics = storage_filter.topics;
    if (!filter_topics.empty()) {
      auto iter = std::find(filter_topics.begin(), filter_topics.end(), topic.name);
      if (iter == filter_topics.end()) {
        continue;
      }
    }

    auto topic_qos = publisher_qos_for_topic(
      topic, topic_qos_profile_overrides_,
      get_logger());
    try {
      publishers_.insert(
        std::make_pair(
          topic.name, create_generic_publisher(topic.name, topic.type, topic_qos)));
    } catch (const std::runtime_error & e) {
      // using a warning log seems better than adding a new option
      // to ignore some unknown message type library
      RCLCPP_WARN(
        get_logger(),
        "Ignoring a topic '%s', reason: %s.", topic.name.c_str(), e.what());
    }
  }
}

bool Player::publish_message(rosbag2_storage::SerializedBagMessageSharedPtr message)
{
  bool message_published = false;
  auto publisher_iter = publishers_.find(message->topic_name);
  if (publisher_iter != publishers_.end()) {
    publisher_iter->second->publish(rclcpp::SerializedMessage(*message->serialized_data));
    message_published = true;
  }
  return message_published;
}

void Player::add_key_callback(
  KeyboardHandler::KeyCode key,
  const std::function<void()> & cb,
  const std::string & op_name)
{
  std::string key_str = enum_key_code_to_str(key);
  if (key == KeyboardHandler::KeyCode::UNKNOWN) {
    RCLCPP_ERROR_STREAM(
      get_logger(),
      "Invalid key binding " << key_str << " for " << op_name);
    throw std::invalid_argument("Invalid key binding.");
  }
  keyboard_callbacks_.push_back(
    keyboard_handler_->add_key_press_callback(
      [cb](KeyboardHandler::KeyCode /*key_code*/,
      KeyboardHandler::KeyModifiers /*key_modifiers*/) {cb();},
      key));
  // show instructions
  RCLCPP_INFO_STREAM(
    get_logger(),
    "Press " << key_str << " for " << op_name);
}

void Player::add_keyboard_callbacks()
{
  // skip if disabled
  if (play_options_.disable_keyboard_controls) {
    return;
  }
  RCLCPP_INFO_STREAM(get_logger(), "Adding keyboard callbacks.");
  // check keybindings
  add_key_callback(
    play_options_.pause_resume_toggle_key,
    [this]() {toggle_paused();},
    "Pause/Resume"
  );
  add_key_callback(
    play_options_.play_next_key,
    [this]() {play_next();},
    "Play Next Message"
  );
}

}  // namespace rosbag2_transport
