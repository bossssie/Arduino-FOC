#include "StepperMotor.h"

// StepperMotor(int pp)
// - pp            - pole pair number
StepperMotor::StepperMotor(int pp)
: FOCMotor()
{
  // number od pole pairs
  pole_pairs = pp;

  // torque control type is voltage by default 
  // current and foc_current not supported yet
  torque_controller = TorqueControlType::voltage;
}

/**
	Link the driver which controls the motor
*/
void StepperMotor::linkDriver(StepperDriver* _driver) {
  driver = _driver;
}

// init hardware pins   
void StepperMotor::init() {
  if(monitor_port) monitor_port->println(F("MOT: Init variables."));
  
  // sanity check for the voltage limit configuration
  if(voltage_limit > driver->voltage_limit) voltage_limit =  driver->voltage_limit;
  // constrain voltage for sensor alignment
  if(voltage_sensor_align > voltage_limit) voltage_sensor_align = voltage_limit;
  
  // update the controller limits
  PID_velocity.limit = voltage_limit;
  P_angle.limit = velocity_limit;

  _delay(500);
  // enable motor
  if(monitor_port) monitor_port->println(F("MOT: Enable."));
  enable();
  _delay(500);
  
}


// disable motor driver
void StepperMotor::disable()
{
  // set zero to PWM
  driver->setPwm(0, 0);
  // disable driver
  driver->disable();
  // motor status update
  enabled = 0;
}
// enable motor driver
void StepperMotor::enable()
{
  // disable enable
  driver->enable();
  // set zero to PWM
  driver->setPwm(0, 0);
  // motor status update
  enabled = 1;
}


/**
  FOC functions
*/
// FOC initialization function
int  StepperMotor::initFOC( float zero_electric_offset, Direction sensor_direction ) {
  int exit_flag = 1;
  // align motor if necessary
  // alignment necessary for encoders!
  if(zero_electric_offset != NOT_SET){
    // abosolute zero offset provided - no need to align
    zero_electric_angle = zero_electric_offset;
    // set the sensor direction - default CW
    sensor->natural_direction = sensor_direction;
  }else{
    // sensor and motor alignment
    _delay(500);
    exit_flag = alignSensor();
    _delay(500);
    }
  if(monitor_port) monitor_port->println(F("MOT: Motor ready."));

  return exit_flag;
}
// Encoder alignment to electrical 0 angle
int StepperMotor::alignSensor() {
  if(monitor_port) monitor_port->println(F("MOT: Align sensor."));
  // align the electrical phases of the motor and sensor
  // set angle -90(270 = 3PI/2) degrees 
  float start_angle = shaftAngle();
  setPhaseVoltage(voltage_sensor_align, 0,  _3PI_2);
  // move one electrical revolution forward
  _delay(500);
  for (int i = 0; i <=500; i++ ) {
    float angle = _3PI_2 + _2PI * i / 500.0;
    setPhaseVoltage(voltage_sensor_align, 0,  angle);
    _delay(2);
  }
  // take and angle in the middle
  float mid_angle = shaftAngle();
  // move one electrical revolution forward
  for (int i = 500; i >=0; i-- ) {
    float angle = _3PI_2 + _2PI * i / 500.0 ;
    setPhaseVoltage(voltage_sensor_align, 0,  angle);
    _delay(2);
  }
  // determin the direction the sensor moved 
  if (mid_angle < start_angle) {
    if(monitor_port) monitor_port->println(F("MOT: natural_direction==CCW"));
    sensor->natural_direction = Direction::CCW;
  } else if (mid_angle == start_angle) {
    if(monitor_port) monitor_port->println(F("MOT: Sensor failed to notice movement"));
  } else{
    if(monitor_port) monitor_port->println(F("MOT: natural_direction==CW"));
  }

  // let the motor stabilize for1 sec
  _delay(1000);
  // set sensor to zero
  sensor->initRelativeZero();
  _delay(500);
  setPhaseVoltage(0, 0, 0);
  _delay(200);

  // find the index if available
  int exit_flag = absoluteZeroAlign();
  _delay(500);
  if(monitor_port){
    if(exit_flag< 0 ) monitor_port->println(F("MOT: Error: Not found!"));
    if(exit_flag> 0 ) monitor_port->println(F("MOT: Success!"));
    else  monitor_port->println(F("MOT: Not available!"));
  }
  return exit_flag;
}


// Encoder alignment the absolute zero angle 
// - to the index
int StepperMotor::absoluteZeroAlign() {

  if(monitor_port) monitor_port->println(F("MOT: Absolute zero align."));
    // if no absolute zero return
  if(!sensor->hasAbsoluteZero()) return 0;
  

  if(monitor_port && sensor->needsAbsoluteZeroSearch()) monitor_port->println(F("MOT: Searching..."));
  // search the absolute zero with small velocity
  while(sensor->needsAbsoluteZeroSearch() && shaft_angle < _2PI){
    loopFOC();   
    voltage.q = PID_velocity(velocity_index_search - shaftVelocity());
  }
  voltage.q = 0;
  voltage.d = 0;
  // disable motor
  setPhaseVoltage(0, 0, 0);

  // align absolute zero if it has been found
  if(!sensor->needsAbsoluteZeroSearch()){
    // align the sensor with the absolute zero
    float zero_offset = sensor->initAbsoluteZero();
    // remember zero electric angle
    zero_electric_angle = _normalizeAngle(_electricalAngle(zero_offset, pole_pairs));
  }
  // return bool if zero found
  return !sensor->needsAbsoluteZeroSearch() ? 1 : -1;
}

// Iterative function looping FOC algorithm, setting Uq on the Motor
// The faster it can be run the better
void StepperMotor::loopFOC() {
  // if disabled do nothing
  if(!enabled) return; 
  // shaft angle 
  shaft_angle = shaftAngle();
  electrical_angle = _normalizeAngle(_electricalAngle(shaft_angle, pole_pairs) + zero_electric_angle);
  // set the phase voltage - FOC heart function :) 
  setPhaseVoltage(voltage.q, voltage.d, electrical_angle);
}

// Iterative function running outer loop of the FOC algorithm
// Behavior of this function is determined by the motor.controller variable
// It runs either angle, velocity or voltage loop
// - needs to be called iteratively it is asynchronous function
// - if target is not set it uses motor.target value
void StepperMotor::move(float new_target) {
  // if disabled do nothing
  if(!enabled) return; 
  // set internal target variable
  if( new_target != NOT_SET ) target = new_target;
  // get angular velocity
  shaft_velocity = shaftVelocity();
  // choose control loop
  switch (controller) {
    case MotionControlType::torque:
      voltage.q =  target;
      voltage.d = 0;
      break;
    case MotionControlType::angle:
      // angle set point
      // include angle loop
      shaft_angle_sp = target;
      shaft_velocity_sp = P_angle( shaft_angle_sp - shaft_angle );
      voltage.q = PID_velocity(shaft_velocity_sp - shaft_velocity);
      voltage.d = 0;
      break;
    case MotionControlType::velocity:
      // velocity set point
      // include velocity loop
      shaft_velocity_sp = target;
      voltage.q = PID_velocity(shaft_velocity_sp - shaft_velocity);
      voltage.d = 0;
      break;
    case MotionControlType::velocity_openloop:
      // velocity control in open loop
      // loopFOC should not be called
      shaft_velocity_sp = target;
      velocityOpenloop(shaft_velocity_sp);
      break;
    case MotionControlType::angle_openloop:
      // angle control in open loop
      // loopFOC should not be called
      shaft_angle_sp = target;
      angleOpenloop(shaft_angle_sp);
      break;
  }
}


// Method using FOC to set Uq and Ud to the motor at the optimal angle
// Function implementing Sine PWM algorithms
// - space vector not implemented yet
// 
// Function using sine approximation
// regular sin + cos ~300us    (no memory usaage)
// approx  _sin + _cos ~110us  (400Byte ~ 20% of memory)
void StepperMotor::setPhaseVoltage(float Uq, float Ud, float angle_el) {
  // Sinusoidal PWM modulation 
  // Inverse Park transformation

  // angle normalization in between 0 and 2pi
  // only necessary if using _sin and _cos - approximation functions
  float _ca = _cos(angle_el);
  float _sa = _sin(angle_el);
  // Inverse park transform
  Ualpha =  _ca * Ud - _sa * Uq;  // -sin(angle) * Uq;
  Ubeta =  _sa * Ud + _ca * Uq;    //  cos(angle) * Uq;

  // set the voltages in hardware
  driver->setPwm(Ualpha, Ubeta);
}

// Function (iterative) generating open loop movement for target velocity
// - target_velocity - rad/s
// it uses voltage_limit variable
void StepperMotor::velocityOpenloop(float target_velocity){
  // get current timestamp
  unsigned long now_us = _micros();
  // calculate the sample time from last call
  float Ts = (now_us - open_loop_timestamp) * 1e-6;

  // calculate the necessary angle to achieve target velocity
  shaft_angle = _normalizeAngle(shaft_angle + target_velocity*Ts);
    
  // set the maximal allowed voltage (voltage_limit) with the necessary angle
  setPhaseVoltage(voltage_limit, 0, _electricalAngle(shaft_angle,pole_pairs));

  // save timestamp for next call
  open_loop_timestamp = now_us;
}

// Function (iterative) generating open loop movement towards the target angle
// - target_angle - rad
// it uses voltage_limit and velocity_limit variables
void StepperMotor::angleOpenloop(float target_angle){
  // get current timestamp
  unsigned long now_us = _micros();
  // calculate the sample time from last call
  float Ts = (now_us - open_loop_timestamp) * 1e-6;
  
  // calculate the necessary angle to move from current position towards target angle
  // with maximal velocity (velocity_limit)
  if(abs( target_angle - shaft_angle ) > abs(velocity_limit*Ts))
    shaft_angle += _sign(target_angle - shaft_angle) * abs( velocity_limit )*Ts; 
  else
    shaft_angle = target_angle;
  
  // set the maximal allowed voltage (voltage_limit) with the necessary angle
  setPhaseVoltage(voltage_limit,  0, _electricalAngle(shaft_angle,pole_pairs));

  // save timestamp for next call
  open_loop_timestamp = now_us;
}