/*
 * behavior_planner.cpp
 *
 *  Created on: Sep 16, 2017
 *      Author: pierluigiferrari
 */

#include <iostream>
#include <vector>
#include <math.h>
#include <numeric>
#include <algorithm>
#include "behavior_planner.h"
#include "trajectory_generator.h"
#include "predictor.h"
#include "helper_functions.h"

using namespace std;

behavior_planner::behavior_planner(int num_lanes,
                                   double speed_limit,
                                   vector<double> map_waypoints_x,
                                   vector<double> map_waypoints_y,
                                   vector<double> map_waypoints_s,
                                   double planning_horizon,
                                   double frontal_buffer,
                                   double lateral_buffer,
                                   double speed_tolerance,
                                   vector<double> cost_weights)
{
  current_state_ = "keep";
  cycle_ref_speed_ = 0;
  planning_horizon_ = planning_horizon;
  frontal_buffer_ = frontal_buffer;
  lateral_buffer_ = lateral_buffer;
  num_lanes_ = num_lanes;
  speed_limit_ = speed_limit;
  cost_weights_ = cost_weights;
  speed_tolerance_ = speed_tolerance;
  map_waypoints_x_ = map_waypoints_x;
  map_waypoints_y_ = map_waypoints_y;
  map_waypoints_s_ = map_waypoints_s;
}

behavior_planner::~behavior_planner() {}

vector<vector<double>> behavior_planner::transition(double car_x,
                                                    double car_y,
                                                    double car_s,
                                                    double car_d,
                                                    double car_yaw,
                                                    double car_speed,
                                                    vector<double> previous_path_x,
                                                    vector<double> previous_path_y,
                                                    double end_path_s,
                                                    double end_path_d,
                                                    vector<vector<double>> sensor_fusion)
{
  car_x_ = car_x;
  car_y_ = car_y;
  car_s_ = car_s;
  car_d_ = car_d;
  car_yaw_ = car_yaw;
  car_speed_ = car_speed;
  previous_path_x_ = previous_path_x;
  previous_path_y_ = previous_path_y;
  end_path_s_ = end_path_s;
  end_path_d_ = end_path_d;
  sensor_fusion_ = sensor_fusion;

  /***************************************************************************
   * 1: Get all generally possible successor states from the current state.
   ***************************************************************************/

  vector<string> successor_states;

  // Compute the car's current lane
  int car_lane = (int) car_d / 4;
  // For the "keep" state, the car's current lane dictates the possible successor states.
  // If we are in the "left" or "right" state, we can only change the state once we have
  // completed the lane change, i.e. once we have arrived in the target lane.
  if      (current_state_ == "keep")
  {
    if      (car_lane == 0)              successor_states = {"keep", "right"};
    else if (car_lane >= num_lanes_ - 1) successor_states = {"keep", "left"};
    else                                 successor_states = {"keep", "left", "right"};
  }
  else if (current_state_ == "left")
  {
    if      (car_lane != target_lane_)   successor_states = {"left"};
    else                                 successor_states = {"keep", "left"};
  }
  else if (current_state_ == "right")
  {
    if      (car_lane != target_lane_)   successor_states = {"right"};
    else                                 successor_states = {"keep", "right"};
  }

  /***************************************************************************
   * 2: Check each of the possible successor states for safety and optimality,
   *    in this order.
   ***************************************************************************/

  vector<string> safe_successor_states; // Store all possible successor states that are safe.
  // Store the following for each safe successor state:
  vector<double> safe_successor_states_cost; // The cost
  vector<vector<vector<double>>> trajectories; // The generated trajectory
  vector<int> target_lanes; // The target lane
  vector<double> target_velocities; // The target velocities
  vector<double> times_to_reach_tv; // The time in seconds allowed to reach the target velocity
  vector<double> times_to_reach_tl; // The time in seconds allowed to reach the target lane

  for (int i = 0; i < successor_states.size(); i++) // Iterate over all possible successor states
  {
    string test_state = successor_states[i];

    /*************************************************************************
     * 2.1: Compute a trajectory for this test state.
     *************************************************************************/

    // In order to figure out whether this successor state would be safe and
    // optimal, compute a trajectory for it so that we know what it would
    // actually mean to transition into this state.

    trajectory_generator tra_gen(map_waypoints_x_, map_waypoints_y_, map_waypoints_s_);

    // Compute the target lane for this test state.
    int target_lane;
    if (test_state == "keep")  target_lane = car_lane;
    if (test_state == "left")  target_lane = car_lane - 1;
    if (test_state == "right") target_lane = car_lane + 1;

    cout << "target_lane: " << target_lane << endl;

    // Identify the leading and trailing vehicles in the target lane so that we can adjust our speed.
    vector<int> leading_trailing = get_leading_trailing(target_lane);

    // Compute the target velocity based on the vehicles around us and the speed limit.
    double target_velocity = compute_target_velocity(leading_trailing);
    double time_to_reach_tv = fabs(target_velocity - cycle_ref_speed_) / 4.0; // Under normal circumstances, reach the target velocity at an acceleration of 4 m/s^2.

    cout << "target_velocity before safety_brake: " << target_velocity << endl;

    // Check if we need to correct the target velocity and the time to reach it to keep a safe distance to the leading vehicle.
    vector<double> safety_velocity = safety_brake();
    if (safety_velocity[0] > 0)
    {
      target_velocity = safety_velocity[0];
      time_to_reach_tv = safety_velocity[1];
    }
    cout << "target_velocity after safety_brake: " << target_velocity << endl;

    double ref_d = get_ref_d();
    double dist_to_target_lane = fabs(ref_d - (2 + target_lane_ * 4));
    // The further we are still away from the target lane, the more time we allow to reach it.
    double time_to_reach_tl = 2.0; // 1.0 + dist_to_target_lane / 2.0;

    // Compute the trajectory we would follow if we chose this state.
    vector<vector<double>> test_trajectory = tra_gen.generate_trajectory(target_lane,
                                                                         target_velocity,
                                                                         time_to_reach_tl,
                                                                         time_to_reach_tv,
                                                                         planning_horizon_,
                                                                         true,
                                                                         car_x,
                                                                         car_y,
                                                                         car_s,
                                                                         car_d,
                                                                         car_yaw,
                                                                         cycle_ref_speed_,
                                                                         previous_path_x,
                                                                         previous_path_y,
                                                                         end_path_s,
                                                                         end_path_d);

    /*************************************************************************
     * 2.2: Check whether this state is safe to transition into.
     *      Need to avoid getting too close to other objects.
     *************************************************************************/

    // If the current test state under consideration is a lane change,
    // check if performing this lane change at this time would be safe with regard
    // to other traffic in the target lane.
    bool safe;
    if (test_state == "left" || test_state == "right") safe = lane_change_safe(test_trajectory, target_lane);
    else if (test_state == "keep") safe = true; // In our simple world it's always safe to stay in the current lane.

    // If the test state is safe, add it to the list of safe successor states.
    if (safe)
    {
      safe_successor_states.push_back(test_state);
      trajectories.push_back(test_trajectory);
      target_lanes.push_back(target_lane);
      target_velocities.push_back(target_velocity);
      times_to_reach_tv.push_back(time_to_reach_tv);
      times_to_reach_tl.push_back(time_to_reach_tl);
    }
    else continue; // If this test state is not safe, continue with the next test state.

    /*************************************************************************
     * 2.3: Compute the cost of this state.
     *************************************************************************/

    // If this test state is safe, compute its cost.
    double cost = 0;
    cost += cost_weights_[0] * cost_velocity(test_trajectory);
    cost += cost_weights_[1] * cost_speed_limit(test_trajectory);
    cost += cost_weights_[2] * cost_lane_change(target_lane);
    // We could add other cost functions here, but let's keep it simple for now.

    // Add the cost for this state to the list.
    safe_successor_states_cost.push_back(cost);
  }

  // TODO: In case the list of safe successor states is empty at this point
  //       (this can happen if we're in the middle of a lane change and the
  //       lane change suddenly turns out to no longer be safe),
  //       generate a "keep" trajectory.

  /***************************************************************************
   * 3: Select the state with the lowest cost.
   ***************************************************************************/

  // Find the minimum cost element...
  int arg_min = distance(safe_successor_states_cost.begin(), min_element(safe_successor_states_cost.begin(), safe_successor_states_cost.end()));
  // ...and select the corresponding state.
  current_state_ = safe_successor_states[arg_min];

  /***************************************************************************
   * 4: Compute the final trajectory.
   ***************************************************************************/

  // As opposed to the test trajectories, which extended further into the future,
  // the final trajectory will be a combination of newly generated path points
  // and the remaining previously generated path points.

  vector<double> next_x_vals = previous_path_x_;
  vector<double> next_y_vals = previous_path_y_;
  // Fill up the previously generated path points with the newly generated ones
  // from the best successor state.
  int num_new_path_points = planning_horizon_ / 0.02 - previous_path_x_.size();
  cout << "num_new_path_points: " << num_new_path_points << endl;
  cout << endl;
  for (int i = 0; i < num_new_path_points; i++)
  {
    next_x_vals.push_back(trajectories[arg_min][0][i]);
    next_y_vals.push_back(trajectories[arg_min][1][i]);
    cout << "ref_speed: " << trajectories[arg_min][2][i] << endl;
  }
  cout << endl;
  // Set the cycle reference speed as the speed at the last point of this trajectory
  cycle_ref_speed_ = trajectories[arg_min][2][num_new_path_points - 1];

  // Store the target lane, it will be our memory in future iterations for checking
  // when (and whether) a lane change was successful.
  target_lane_ = target_lanes[arg_min];

  return {next_x_vals, next_y_vals};
}

double behavior_planner::get_ref_s()
{
  if (previous_path_x_.size() > 0)
  {
    return end_path_s_;
  }
  else return car_s_;
}

double behavior_planner::get_ref_d()
{
  if (previous_path_x_.size() > 0)
  {
    return end_path_d_;
  }
  else return car_d_;
}

double behavior_planner::get_ref_t()
{
  return previous_path_x_.size() * 0.02;
}

vector<int> behavior_planner::get_leading_trailing(int lane)
{
  int leading_car = -1; // The index of the leading car in the sensor fusion table
  int trailing_car = -1; // The index of the trailing car in the sensor fusion table

  // Set the reference s coordinate. This is the s coordinate where the ego car
  // will be at the end of the current path.
  double ref_s = get_ref_s();

  double leading_s = ref_s + 10000; // Just some large enough number
  double trailing_s = ref_s - 10000; // Just some small enough number

  // Instantiate a predictor so that we can predict where the other vehicles will be
  // by the time the ego car will be at `ent_path_s_`.
  predictor pred(sensor_fusion_);

  // The reference time is the time of the last path point of the current path.
  // This is the time for which we want to predict the positions of the other vehicles.
  double ref_t = get_ref_t();

  for (int i = 0; i < sensor_fusion_.size(); i++) // Iterate over all cars in the sensor fusion data.
  {
    vector<double> other_car_pos = pred.position(i, ref_t); // Predict this car's position at the reference time.

    int other_car_lane = (int) other_car_pos[2];
    double other_car_s = other_car_pos[0];

    if (other_car_lane == lane) // If this car will be in the lane of interest...
    {
      if (other_car_s > end_path_s_ && other_car_s < leading_s) // ...and if it will be both ahead of us and closer than any other leading car we have found so far...
      {
        leading_car = i; // ...then this becomes our new closest leading car.
        leading_s = other_car_s;
      }
      if (other_car_s < end_path_s_ && other_car_s > trailing_s) // If this car will be both behind us and closer than any other trailing car we have found so far...
      {
        trailing_car = i; // ...then this becomes our new closest trailing car.
        trailing_s = other_car_s;
      }
    }
  }

  return {leading_car, trailing_car};
}

double behavior_planner::compute_target_velocity(vector<int> leading_trailing)
{
  int leading_car = leading_trailing[0];

  // If there is no leading car in the lane of interest, go the speed limit.
  if (leading_car == -1) {
    cout << "nobody in front of us, go at speed limit!" << endl;
    return speed_limit_ - speed_tolerance_;
  }

  // Check how far away the leading vehicle is. We only adjust our speed if it is
  // within our frontal buffer distance.
  double ref_s = get_ref_s();
  double ref_t = get_ref_t();

  // Instantiate a predictor so that we can predict where the leading vehicle will be
  // by the time the ego car will be at `ent_path_s_`.
  predictor pred(sensor_fusion_);
  vector<double> leading_car_pos = pred.position(leading_car, ref_t); // Predict the leading car's position at the reference time.
  double leading_car_s = leading_car_pos[0];

  // If the leading car is far away, go at the speed limit
  if (fabs(leading_car_s - ref_s) > frontal_buffer_) {
    cout << "leading car is far away, go at speed limit!" << endl;
    return speed_limit_ - speed_tolerance_;
  }

  // We assume that the leading car is driving at constant velocity,
  // i.e. it will be as fast at time t_0 (the time at which the ego
  // car will reach `end_path_s_`) as it is now.
  double leading_car_vx = sensor_fusion_[leading_car][3];
  double leading_car_vy = sensor_fusion_[leading_car][4];

  double leading_car_v = sqrt(leading_car_vx * leading_car_vx + leading_car_vy * leading_car_vy);

  // We always adjust our speed to follow the leading car if there is one,
  // unless the leading car is going faster than the speed limit.
  if (leading_car_v > speed_limit_ - speed_tolerance_) {
    cout << "car in front of us, but faster than speed limit, go at speed limit!" << endl;
    return speed_limit_ - speed_tolerance_;
  }
  else {
    cout << "car in front of us, go at their speed!" << endl;
    return leading_car_v;
  }
}

vector<double> behavior_planner::safety_brake()
{
  // If we are not sensing any other objects around us, we definitely don't need to brake for safety reasons.
  if (sensor_fusion_.size() == 0) return {-1.0, -1.0};

  double target_velocity = -1.0;
  double time_to_tv = -1.0; // How fast we want to slow down to the target velocity

  // Register how close the closest car in front of the ego vehicle is and how fast it is going.
  double frontal_space = 10000; // Just some large enough number
  double leading_speed;

  // Set the reference s coordinate. This is the s coordinate where the ego car
  // will be at the end of the current path.
  double ref_s = get_ref_s();
  double ref_d = get_ref_d();

  // Instantiate a predictor so that we can predict where the other vehicles will be
  // by the time the ego car will be at `ent_path_s_`.
  predictor pred(sensor_fusion_);

  // The reference time is the time of the last path point of the current path.
  // This is the time for which we want to predict the positions of the other vehicles.
  double ref_t = get_ref_t();

  for (int i = 0; i < sensor_fusion_.size(); i++) // Iterate over all cars in the sensor fusion data.
  {
    vector<double> other_car_pos = pred.position(i, ref_t); // Predict this car's position at the reference time.

    double other_car_s = other_car_pos[0];
    double other_car_d = other_car_pos[1];

    if (other_car_d > (ref_d - lateral_buffer_) && other_car_d < (ref_d + lateral_buffer_))
    { // ...and if the other car too close to the ego car LATERALLY
      // (in other words, if it's kind of in our way),...

      // ...check what the longitudinal distance will be between us and the other car
      double s_distance = other_car_s - ref_s;
      if (other_car_s > ref_s && s_distance < frontal_buffer_ && s_distance < frontal_space)
      { // If the other car will be in front of the ego car AND too close AND closer than any other car in front of us so far...
        frontal_space = s_distance; // ...then this car determines the space we will have in front of the ego car.
        leading_speed = pred.velocity(i);
      }
    }
  }

  if (frontal_space < frontal_buffer_)
  {
    cout << "car too close in front of us, slow down!" << endl;
    target_velocity = leading_speed - 2.28; // Slow down by 2.28 m/s ≈ 10 km/h.
    time_to_tv = 0.1 * frontal_space; // E.g.: If the frontal space was 10 meters, the time to slow down would be 1 second.

    return {target_velocity, time_to_tv};
  }
  else return {-1.0, -1.0};
}

bool behavior_planner::lane_change_safe(vector<vector<double>> trajectory, int target_lane)
{
  // Check whether following the suggested trajectory would likely get the
  // ego car too close to other vehicles.
  // For each time point in this trajectory, check if the ego car would be
  // too close to any other vehicle's position at that same time point.

  // Set up a predictor object to predict the position of the other vehicles in the future.
  predictor pred(sensor_fusion_);

  // The trajectory to be tested begins at the reference time.
  double ref_t = get_ref_t();

  bool safe = true; // If `true`, this trajectory is safe.

  for (int i = 0; i < trajectory[0].size(); i++) // Iterate over all trajectory points.
  {
    for (int j = 0; j < sensor_fusion_.size(); j++) // Iterate over all other vehicles on the road.
    {
      vector<double> other_car_pos = pred.position(j, ref_t + i * 0.02); // Predict this car's position at time t_i = ref_t + i * 0.02s.

      int other_car_lane = other_car_pos[2];

      if (other_car_lane == target_lane) // If this car will be in the target lane at time t_i...
      {
        double other_car_s = other_car_pos[0];
        double other_car_d = other_car_pos[1];

        // Convert to Cartesian coordinates so we can measure the Euclidean distance.
        vector<double> other_car_xy = get_xy(other_car_s, other_car_d, map_waypoints_s_, map_waypoints_x_, map_waypoints_y_);

        double ego_car_x = trajectory[0][i];
        double ego_car_y = trajectory[1][i];

        // Compute the predicted l2 distance between the ego car and the other car at time t_i.
        double l2_norm = l2_distance(ego_car_x, ego_car_y, other_car_xy[0], other_car_xy[1]);
        // If the distance is smaller than the required safety buffer at any point in time, this trajectory is not safe.
        if (l2_norm < lateral_buffer_) safe = false;
      }
    }
  }

  return safe;
}

double behavior_planner::cost_velocity(const vector<vector<double>> &trajectory, double stop_cost)
{
  vector<double> velocity = trajectory[2];
  double average_velocity = accumulate(velocity.begin(), velocity.end(), 0.0) / velocity.size();

  if (average_velocity <= speed_limit_) return (-stop_cost / speed_limit_) * average_velocity + stop_cost;
  else return 0.0; // This cost function doesn't care about the speed limit.
}

double behavior_planner::cost_speed_limit(const vector<vector<double>> &trajectory)
{
  vector<double> velocity = trajectory[2];

  for (int i = 0; i < velocity.size(); i++)
  {
    if (velocity[i] > speed_limit_ + speed_tolerance_) return 1.0;
  }

  return 0.0;
}

double behavior_planner::cost_lane_change(int target_lane)
{
  int car_lane = (int) car_d_ / 4;
  if (target_lane != car_lane) return 1.0;
  return 0.0;
}
