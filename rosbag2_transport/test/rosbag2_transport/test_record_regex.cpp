// Copyright 2021, Robotec.ai sp. z o.o.
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

#include <gmock/gmock.h>

#include <chrono>
#include <memory>
#include <regex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "rclcpp/rclcpp.hpp"

#include "rosbag2_test_common/publication_manager.hpp"
#include "rosbag2_test_common/wait_for.hpp"
#include "rosbag2_test_common/client_manager.hpp"

#include "rosbag2_transport/recorder.hpp"

#include "test_msgs/msg/arrays.hpp"
#include "test_msgs/msg/basic_types.hpp"
#include "test_msgs/message_fixtures.hpp"
#include "test_msgs/srv/basic_types.hpp"

#include "mock_recorder.hpp"
#include "record_integration_fixture.hpp"

using namespace std::chrono_literals;  // NOLINT

TEST_F(RecordIntegrationTestFixture, regex_topics_recording)
{
  auto test_string_messages = get_messages_strings();
  auto test_array_messages = get_messages_arrays();
  std::string regex = "^/aa$";

  // matching topic
  std::string v1 = "/aa";

  // topics that shouldn't match
  std::string b1 = "/aaa";
  std::string b2 = "/baa";
  std::string b3 = "/baaa";
  std::string b4 = "/aa/aa";

  // checking the test data itself
  std::regex re(regex);
  ASSERT_TRUE(std::regex_match(v1, re));
  ASSERT_FALSE(std::regex_match(b1, re));
  ASSERT_FALSE(std::regex_match(b2, re));
  ASSERT_FALSE(std::regex_match(b3, re));
  ASSERT_FALSE(std::regex_match(b4, re));

  rosbag2_transport::RecordOptions record_options =
  {false, false, false, {}, {}, {}, {}, {}, {}, "rmw_format", 10ms};
  record_options.regex = regex;

  // TODO(karsten1987) Refactor this into publication manager
  rosbag2_test_common::PublicationManager pub_manager;
  pub_manager.setup_publisher(v1, test_string_messages[0], 3);
  pub_manager.setup_publisher(b1, test_string_messages[0], 3);
  pub_manager.setup_publisher(b2, test_string_messages[1], 3);
  pub_manager.setup_publisher(b3, test_string_messages[0], 3);
  pub_manager.setup_publisher(b4, test_string_messages[1], 3);

  auto recorder = std::make_shared<rosbag2_transport::Recorder>(
    std::move(writer_), storage_options_, record_options);
  recorder->record();

  start_async_spin(recorder);
  auto cleanup_process_handle = rcpputils::make_scope_exit([&]() {stop_spinning();});

  ASSERT_TRUE(pub_manager.wait_for_matched(v1.c_str()));

  pub_manager.run_publishers();

  auto & writer = recorder->get_writer_handle();
  MockSequentialWriter & mock_writer =
    static_cast<MockSequentialWriter &>(writer.get_implementation_handle());

  constexpr size_t expected_messages = 3;
  auto ret = rosbag2_test_common::wait_until_condition(
    [ =, &mock_writer]() {
      return mock_writer.get_number_of_recorded_messages() >= expected_messages;
    },
    std::chrono::seconds(5));
  auto recorded_messages = mock_writer.get_messages();
  // We may receive additional messages from rosout, it doesn't matter,
  // as long as we have received at least as many total messages as we expect
  EXPECT_TRUE(ret) << "failed to capture expected messages in time";
  EXPECT_THAT(recorded_messages, SizeIs(Ge(expected_messages)));
  auto recorded_topics = mock_writer.get_topics();
  EXPECT_THAT(recorded_topics, SizeIs(1));
  EXPECT_TRUE(recorded_topics.find(v1) != recorded_topics.end());
}

TEST_F(RecordIntegrationTestFixture, regex_and_exclude_regex_topic_recording)
{
  auto test_string_messages = get_messages_strings();
  auto test_array_messages = get_messages_arrays();
  std::string regex = "/[a-z]+_nice(_.*)";
  std::string topics_regex_to_exclude = "/[a-z]+_nice_[a-z]+/(.*)";

  // matching topics - the only ones that should be recorded
  std::string v1 = "/awesome_nice_topic";
  std::string v2 = "/still_nice_topic";

  // excluded topics
  std::string e1 = "/quite_nice_namespace/but_it_is_excluded";

  // topics that shouldn't match
  std::string b1 = "/numberslike1arenot_nice";
  std::string b2 = "/namespace_before/not_nice";

  // checking the test data itself
  std::regex re(regex);
  std::regex exclude(topics_regex_to_exclude);
  ASSERT_TRUE(std::regex_match(v1, re));
  ASSERT_TRUE(std::regex_match(v2, re));
  ASSERT_FALSE(std::regex_match(b1, re));
  ASSERT_FALSE(std::regex_match(b2, re));

  // this example matches both regexes - should be excluded
  ASSERT_TRUE(std::regex_match(e1, re));
  ASSERT_TRUE(std::regex_match(e1, exclude));

  rosbag2_transport::RecordOptions record_options =
  {false, false, false, {}, {}, {}, {}, {}, {}, "rmw_format", 10ms};
  record_options.regex = regex;
  record_options.exclude_regex = topics_regex_to_exclude;

  // TODO(karsten1987) Refactor this into publication manager
  rosbag2_test_common::PublicationManager pub_manager;
  pub_manager.setup_publisher(v1, test_string_messages[0], 3);
  pub_manager.setup_publisher(v2, test_string_messages[1], 3);
  pub_manager.setup_publisher(b1, test_string_messages[0], 3);
  pub_manager.setup_publisher(b2, test_string_messages[1], 3);
  pub_manager.setup_publisher(e1, test_string_messages[0], 3);

  auto recorder = std::make_shared<rosbag2_transport::Recorder>(
    std::move(writer_), storage_options_, record_options);
  recorder->record();

  start_async_spin(recorder);
  auto cleanup_process_handle = rcpputils::make_scope_exit([&]() {stop_spinning();});

  ASSERT_TRUE(pub_manager.wait_for_matched(v1.c_str()));
  ASSERT_TRUE(pub_manager.wait_for_matched(v2.c_str()));

  pub_manager.run_publishers();

  auto & writer = recorder->get_writer_handle();
  MockSequentialWriter & mock_writer =
    static_cast<MockSequentialWriter &>(writer.get_implementation_handle());

  constexpr size_t expected_messages = 3;
  auto ret = rosbag2_test_common::wait_until_condition(
    [ =, &mock_writer]() {
      return mock_writer.get_number_of_recorded_messages() >= expected_messages;
    },
    std::chrono::seconds(5));
  auto recorded_messages = mock_writer.get_messages();
  // We may receive additional messages from rosout, it doesn't matter,
  // as long as we have received at least as many total messages as we expect
  EXPECT_TRUE(ret) << "failed to capture expected messages in time";
  EXPECT_THAT(recorded_messages, SizeIs(Ge(expected_messages)));

  auto recorded_topics = mock_writer.get_topics();
  EXPECT_THAT(recorded_topics, SizeIs(2));
  EXPECT_TRUE(recorded_topics.find(v1) != recorded_topics.end());
  EXPECT_TRUE(recorded_topics.find(v2) != recorded_topics.end());
}

TEST_F(RecordIntegrationTestFixture, regex_and_exclude_topic_topic_recording)
{
  auto test_string_messages = get_messages_strings();
  auto test_array_messages = get_messages_arrays();
  std::string regex = "/[a-z]+_nice(_.*)";
  std::string topics_exclude = "/quite_nice_namespace/but_it_is_excluded";

  // matching topics - the only ones that should be recorded
  std::string v1 = "/awesome_nice_topic";
  std::string v2 = "/still_nice_topic";

  // excluded topics
  std::string e1 = "/quite_nice_namespace/but_it_is_excluded";

  // topics that shouldn't match
  std::string b1 = "/numberslike1arenot_nice";
  std::string b2 = "/namespace_before/not_nice";

  // checking the test data itself
  std::regex re(regex);
  ASSERT_TRUE(std::regex_match(v1, re));
  ASSERT_TRUE(std::regex_match(v2, re));
  ASSERT_FALSE(std::regex_match(b1, re));
  ASSERT_FALSE(std::regex_match(b2, re));

  // this example matches both regexes - should be excluded
  ASSERT_TRUE(std::regex_match(e1, re));
  ASSERT_TRUE(e1 == topics_exclude);

  rosbag2_transport::RecordOptions record_options =
  {false, false, false, {}, {}, {}, {}, {}, {}, "rmw_format", 10ms};
  record_options.regex = regex;
  record_options.exclude_topics.emplace_back(topics_exclude);

  // TODO(karsten1987) Refactor this into publication manager
  rosbag2_test_common::PublicationManager pub_manager;
  pub_manager.setup_publisher(v1, test_string_messages[0], 3);
  pub_manager.setup_publisher(v2, test_string_messages[1], 3);
  pub_manager.setup_publisher(b1, test_string_messages[0], 3);
  pub_manager.setup_publisher(b2, test_string_messages[1], 3);
  pub_manager.setup_publisher(e1, test_string_messages[0], 3);

  auto recorder = std::make_shared<rosbag2_transport::Recorder>(
    std::move(writer_), storage_options_, record_options);
  recorder->record();

  start_async_spin(recorder);
  auto cleanup_process_handle = rcpputils::make_scope_exit([&]() {stop_spinning();});

  ASSERT_TRUE(pub_manager.wait_for_matched(v1.c_str()));
  ASSERT_TRUE(pub_manager.wait_for_matched(v2.c_str()));

  pub_manager.run_publishers();

  auto & writer = recorder->get_writer_handle();
  MockSequentialWriter & mock_writer =
    static_cast<MockSequentialWriter &>(writer.get_implementation_handle());

  constexpr size_t expected_messages = 3;
  auto ret = rosbag2_test_common::wait_until_condition(
    [ =, &mock_writer]() {
      return mock_writer.get_number_of_recorded_messages() >= expected_messages;
    },
    std::chrono::seconds(5));
  auto recorded_messages = mock_writer.get_messages();
  // We may receive additional messages from rosout, it doesn't matter,
  // as long as we have received at least as many total messages as we expect
  EXPECT_TRUE(ret) << "failed to capture expected messages in time";
  EXPECT_THAT(recorded_messages, SizeIs(Ge(expected_messages)));

  auto recorded_topics = mock_writer.get_topics();
  EXPECT_THAT(recorded_topics, SizeIs(2));
  EXPECT_TRUE(recorded_topics.find(v1) != recorded_topics.end());
  EXPECT_TRUE(recorded_topics.find(v2) != recorded_topics.end());
}

TEST_F(RecordIntegrationTestFixture, regex_and_exclude_regex_service_recording)
{
  std::string regex = "/[a-z]+_nice(_.*)";
  std::string services_regex_to_exclude = "/[a-z]+_nice_[a-z]+/(.*)";

  // matching service
  std::string v1 = "/awesome_nice_service";
  std::string v2 = "/still_nice_service";

  // excluded service
  std::string e1 = "/quite_nice_namespace/but_it_is_excluded";

  // service that shouldn't match
  std::string b1 = "/numberslike1arenot_nice";
  std::string b2 = "/namespace_before/not_nice";

  rosbag2_transport::RecordOptions record_options =
  {false, false, false, {}, {}, {}, {}, {}, {}, "rmw_format", 10ms};
  record_options.regex = regex;
  record_options.exclude_regex = services_regex_to_exclude;

  auto service_manager_v1 =
    std::make_shared<rosbag2_test_common::ClientManager<test_msgs::srv::BasicTypes>>(v1);

  auto service_manager_v2 =
    std::make_shared<rosbag2_test_common::ClientManager<test_msgs::srv::BasicTypes>>(v2);

  auto service_manager_e1 =
    std::make_shared<rosbag2_test_common::ClientManager<test_msgs::srv::BasicTypes>>(e1);

  auto service_manager_b1 =
    std::make_shared<rosbag2_test_common::ClientManager<test_msgs::srv::BasicTypes>>(b1);

  auto service_manager_b2 =
    std::make_shared<rosbag2_test_common::ClientManager<test_msgs::srv::BasicTypes>>(b2);

  auto recorder = std::make_shared<MockRecorder>(
    std::move(writer_), storage_options_, record_options);
  recorder->record();

  start_async_spin(recorder);
  auto cleanup_process_handle = rcpputils::make_scope_exit([&]() {stop_spinning();});

  ASSERT_TRUE(service_manager_v1->wait_for_service_to_be_ready());
  ASSERT_TRUE(service_manager_v2->wait_for_service_to_be_ready());
  ASSERT_TRUE(service_manager_e1->wait_for_service_to_be_ready());
  ASSERT_TRUE(service_manager_b1->wait_for_service_to_be_ready());
  ASSERT_TRUE(service_manager_b2->wait_for_service_to_be_ready());

  // At this point, we expect that the services /still_nice_service and /awesome_nice_service,
  // along with the event topics /still_nice_service/_service_event
  // and /awesome_nice_service/_service_event are available to be recorded.  However,
  // wait_for_service_to_be_ready() only checks the services, not the event topics, so ask the
  // recorder to make sure it has successfully subscribed to all.
  ASSERT_TRUE(recorder->wait_for_topic_to_be_discovered(v1 + "/_service_event"));
  ASSERT_TRUE(recorder->wait_for_topic_to_be_discovered(v2 + "/_service_event"));

  auto & writer = recorder->get_writer_handle();
  auto & mock_writer = dynamic_cast<MockSequentialWriter &>(writer.get_implementation_handle());

  ASSERT_TRUE(service_manager_v1->send_request());
  ASSERT_TRUE(service_manager_v2->send_request());
  ASSERT_TRUE(service_manager_e1->send_request());
  ASSERT_TRUE(service_manager_b1->send_request());
  ASSERT_TRUE(service_manager_b2->send_request());

  constexpr size_t expected_messages = 4;
  auto ret = rosbag2_test_common::wait_until_condition(
    [ =, &mock_writer]() {
      return mock_writer.get_number_of_recorded_messages() >= expected_messages;
    },
    std::chrono::seconds(5));
  EXPECT_TRUE(ret) << "failed to capture expected messages in time";
  auto recorded_messages = mock_writer.get_messages();
  EXPECT_THAT(recorded_messages, SizeIs(expected_messages));

  auto recorded_topics = mock_writer.get_topics();
  EXPECT_THAT(recorded_topics, SizeIs(2));
  EXPECT_TRUE(recorded_topics.find(v1 + "/_service_event") != recorded_topics.end());
  EXPECT_TRUE(recorded_topics.find(v2 + "/_service_event") != recorded_topics.end());
}

TEST_F(RecordIntegrationTestFixture, regex_and_exclude_service_service_recording)
{
  std::string regex = "/[a-z]+_nice(_.*)";
  std::string services_exclude = "/quite_nice_namespace/but_it_is_excluded/_service_event";

  // matching service
  std::string v1 = "/awesome_nice_service";
  std::string v2 = "/still_nice_service";

  // excluded topics
  std::string e1 = "/quite_nice_namespace/but_it_is_excluded";

  // service that shouldn't match
  std::string b1 = "/numberslike1arenot_nice";
  std::string b2 = "/namespace_before/not_nice";

  rosbag2_transport::RecordOptions record_options =
  {false, false, false, {}, {}, {}, {}, {}, {}, "rmw_format", 10ms};
  record_options.regex = regex;
  record_options.exclude_service_events.emplace_back(services_exclude);

  auto service_manager_v1 =
    std::make_shared<rosbag2_test_common::ClientManager<test_msgs::srv::BasicTypes>>(v1);

  auto service_manager_v2 =
    std::make_shared<rosbag2_test_common::ClientManager<test_msgs::srv::BasicTypes>>(v2);

  auto service_manager_e1 =
    std::make_shared<rosbag2_test_common::ClientManager<test_msgs::srv::BasicTypes>>(e1);

  auto service_manager_b1 =
    std::make_shared<rosbag2_test_common::ClientManager<test_msgs::srv::BasicTypes>>(b1);

  auto service_manager_b2 =
    std::make_shared<rosbag2_test_common::ClientManager<test_msgs::srv::BasicTypes>>(b2);

  auto recorder = std::make_shared<MockRecorder>(
    std::move(writer_), storage_options_, record_options);
  recorder->record();

  start_async_spin(recorder);
  auto cleanup_process_handle = rcpputils::make_scope_exit([&]() {stop_spinning();});

  ASSERT_TRUE(service_manager_v1->wait_for_service_to_be_ready());
  ASSERT_TRUE(service_manager_v2->wait_for_service_to_be_ready());
  ASSERT_TRUE(service_manager_e1->wait_for_service_to_be_ready());
  ASSERT_TRUE(service_manager_b1->wait_for_service_to_be_ready());
  ASSERT_TRUE(service_manager_b2->wait_for_service_to_be_ready());

  // At this point, we expect that the services /still_nice_service and /awesome_nice_service,
  // along with the event topics /still_nice_service/_service_event
  // and /awesome_nice_service/_service_event are available to be recorded.  However,
  // wait_for_service_to_be_ready() only checks the services, not the event topics, so ask the
  // recorder to make sure it has successfully subscribed to all.
  ASSERT_TRUE(recorder->wait_for_topic_to_be_discovered(v1 + "/_service_event"));
  ASSERT_TRUE(recorder->wait_for_topic_to_be_discovered(v2 + "/_service_event"));

  auto & writer = recorder->get_writer_handle();
  auto & mock_writer = dynamic_cast<MockSequentialWriter &>(writer.get_implementation_handle());

  ASSERT_TRUE(service_manager_v1->send_request());
  ASSERT_TRUE(service_manager_v2->send_request());
  ASSERT_TRUE(service_manager_e1->send_request());
  ASSERT_TRUE(service_manager_b1->send_request());
  ASSERT_TRUE(service_manager_b2->send_request());

  constexpr size_t expected_messages = 4;
  auto ret = rosbag2_test_common::wait_until_condition(
    [ =, &mock_writer]() {
      return mock_writer.get_number_of_recorded_messages() >= expected_messages;
    },
    std::chrono::seconds(5));
  EXPECT_TRUE(ret) << "failed to capture expected messages in time";
  auto recorded_messages = mock_writer.get_messages();
  EXPECT_THAT(recorded_messages, SizeIs(expected_messages));

  auto recorded_topics = mock_writer.get_topics();
  EXPECT_THAT(recorded_topics, SizeIs(2));
  EXPECT_TRUE(recorded_topics.find(v1 + "/_service_event") != recorded_topics.end());
  EXPECT_TRUE(recorded_topics.find(v2 + "/_service_event") != recorded_topics.end());
}

TEST_F(RecordIntegrationTestFixture, regex_discovery_stops_after_timeout_for_delay)
{
  // With a regex the recorder can never tell that "all requested topics" are subscribed, so
  // only timeout_for_delay ends the discovery. Topics that appear inside that window are
  // recorded, topics that appear after it are not.
  // three listed topics that never appear, more than the regex matches before the window
  // ends, so that the subscription count cannot equal the number of listed topics
  const std::string listed_never = "/listed_never_published";
  const std::vector<std::string> listed = {listed_never, "/listed_never_2", "/listed_never_3"};
  const std::string rx_at_start = "/rx_at_start";
  const std::string rx_in_window = "/rx_in_window";
  const std::string rx_after_window = "/rx_after_window";
  auto message = get_messages_strings()[0];

  rosbag2_transport::RecordOptions record_options =
  {false, false, false, listed, {}, {}, {}, {}, {}, "rmw_format", 20ms};
  record_options.regex = "^/rx_";
  record_options.timeout_for_delay = 1.5f;

  rosbag2_test_common::PublicationManager pub_at_start;
  pub_at_start.setup_publisher(rx_at_start, message, 1);

  auto recorder = std::make_shared<MockRecorder>(
    std::move(writer_), storage_options_, record_options);
  recorder->record();
  start_async_spin(recorder);
  auto cleanup_process_handle = rcpputils::make_scope_exit([&]() {stop_spinning();});

  auto & writer = recorder->get_writer_handle();
  auto & mock_writer = dynamic_cast<MockSequentialWriter &>(writer.get_implementation_handle());
  auto topic_recorded = [&mock_writer](const std::string & topic) {
      return mock_writer.get_topics().count(topic) > 0;
    };

  ASSERT_TRUE(
    rosbag2_test_common::wait_until_condition(
      [&]() {return topic_recorded(rx_at_start);}, 5s));

  rosbag2_test_common::PublicationManager pub_in_window;
  pub_in_window.setup_publisher(rx_in_window, message, 1);
  EXPECT_TRUE(
    rosbag2_test_common::wait_until_condition(
      [&]() {return topic_recorded(rx_in_window);}, 1s)) <<
    "a topic that appears inside the discovery window must be recorded";

  ASSERT_TRUE(
    rosbag2_test_common::wait_until_condition(
      [&]() {return !recorder->is_discovery_running();}, 5s)) <<
    "discovery must stop after timeout_for_delay although a regex is set";

  rosbag2_test_common::PublicationManager pub_after_window;
  pub_after_window.setup_publisher(rx_after_window, message, 1);
  ASSERT_TRUE(recorder->wait_for_topic_to_be_discovered(rx_after_window));
  // 25 polling intervals: the recorder would have subscribed if discovery were still running
  std::this_thread::sleep_for(500ms);
  EXPECT_FALSE(topic_recorded(rx_after_window));
  EXPECT_FALSE(topic_recorded(listed_never));
}

TEST_F(RecordIntegrationTestFixture, regex_matches_do_not_end_discovery_before_listed_topics)
{
  // Two topics listed, one present at start, and one regex match: the subscription count
  // equals the number of listed topics, but /listed_late is still missing and must be
  // picked up when it appears.
  const std::string listed_present = "/listed_present";
  const std::string listed_late = "/listed_late";
  const std::string rx_match = "/rx_match";
  auto message = get_messages_strings()[0];

  rosbag2_transport::RecordOptions record_options =
  {false, false, false, {listed_present, listed_late}, {}, {}, {}, {}, {}, "rmw_format", 20ms};
  record_options.regex = "^/rx_";

  rosbag2_test_common::PublicationManager pub_at_start;
  pub_at_start.setup_publisher(listed_present, message, 1);
  pub_at_start.setup_publisher(rx_match, message, 1);

  auto recorder = std::make_shared<MockRecorder>(
    std::move(writer_), storage_options_, record_options);
  ASSERT_TRUE(recorder->wait_for_topic_to_be_discovered(listed_present));
  ASSERT_TRUE(recorder->wait_for_topic_to_be_discovered(rx_match));
  recorder->record();
  start_async_spin(recorder);
  auto cleanup_process_handle = rcpputils::make_scope_exit([&]() {stop_spinning();});

  auto & writer = recorder->get_writer_handle();
  auto & mock_writer = dynamic_cast<MockSequentialWriter &>(writer.get_implementation_handle());
  auto topic_recorded = [&mock_writer](const std::string & topic) {
      return mock_writer.get_topics().count(topic) > 0;
    };
  ASSERT_TRUE(
    rosbag2_test_common::wait_until_condition(
      [&]() {return topic_recorded(listed_present) && topic_recorded(rx_match);}, 5s));
  // give the discovery loop time to evaluate its stop condition with two subscriptions
  std::this_thread::sleep_for(200ms);

  rosbag2_test_common::PublicationManager pub_late;
  pub_late.setup_publisher(listed_late, message, 1);
  EXPECT_TRUE(
    rosbag2_test_common::wait_until_condition(
      [&]() {return topic_recorded(listed_late);}, 5s));
}
