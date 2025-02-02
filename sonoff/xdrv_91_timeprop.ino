/*
  xdrv_91_timeprop.ino - Timeprop support for Sonoff-Tasmota
  Copyright (C) 2018 Colin Law and Thomas Herrmann
  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/**
 * Code to drive one or more relays in a time proportioned manner give a
 * required power value.
 *
 * Given required power values in the range 0.0 to 1.0 the relays will be
 * driven on/off in such that the average power suppled will represent
 * the required power.
 * The cycle time is configurable.  If, for example, the
 * period is set to 10 minutes and the power input is 0.2 then the output will
 * be on for two minutes in every ten minutes.
 *
 * A value for actuator dead time may be provided. If you have a device that
 * takes a significant time to open/close then set this to the average of the
 * open and close times.  The algorithim will then adjust the output timing
 * accordingly to ensure that the output is not switched more rapidly than
 * the actuator can cope with.
 *
 * A facility to invert the output is provided which can be useful when used in
 * refrigeration processes and similar.
 *
 * In the case where only one relay is being driven the power value is set by
 * writing the value to the mqtt topic cmnd/timeprop_setpower_0.  If more than
 * one relay is being driven (as might be the case for a heat/cool application
 * where one relay drives the heater and the other the cooler) then the power
 * for the second relay is written to topic cmnd/timeprop_setpower_1 and so on.
 *
 * To cope with the problem of temporary wifi failure etc a
 * TIMEPROP_MAX_UPDATE_INTERVALS value is available. This can be set to the max
 * expected time between power updates and if this time is exceeded then the
 * power will fallback to a given safe value until a new value is provided. Set
 * the interval to 0 to disable this feature.
 *
 * Usage:
 * Place this file in the sonoff folder.
 * Clone the library https://github.com/colinl/process-control.git from Github
 * into a subfolder of lib.
 * In user_config.h or user_config_override.h for a single relay, include
 * code as follows:

 #define USE_TIMEPROP    //  include the timeprop feature (+1.2k)
   // for single output
   #define TIMEPROP_NUM_OUTPUTS          1       // how many outputs to control (with separate alogorithm for each)
   #define TIMEPROP_CYCLETIMES           60      // cycle time seconds
   #define TIMEPROP_DEADTIMES            0       // actuator action time seconds
   #define TIMEPROP_OPINVERTS            false   // whether to invert the output
   #define TIMEPROP_FALLBACK_POWERS      0       // falls back to this if too long betwen power updates
   #define TIMEPROP_MAX_UPDATE_INTERVALS 120     // max no secs that are allowed between power updates (0 to disable)
   #define TIMEPROP_RELAYS               1       // which relay to control 1:8

 * or for two relays:
 #define USE_TIMEPROP    //  include the timeprop feature (+1.2k)
   // for single output
   #define TIMEPROP_NUM_OUTPUTS          2               // how many outputs to control (with separate alogorithm for each)
   #define TIMEPROP_CYCLETIMES           60,     10      // cycle time seconds
   #define TIMEPROP_DEADTIMES            0,      0       // actuator action time seconds
   #define TIMEPROP_OPINVERTS            false,  false   // whether to invert the output
   #define TIMEPROP_FALLBACK_POWERS      0,      0       // falls back to this if too long betwen power updates
   #define TIMEPROP_MAX_UPDATE_INTERVALS 120,    120     // max no secs that are allowed between power updates (0 to disable)
   #define TIMEPROP_RELAYS               1,      2       // which relay to control 1:8

 * Publish values between 0 and 1 to the topic(s) described above
 *
**/


#ifdef USE_TIMEPROP

# include "Timeprop.h"

#define D_CMND_TIMEPROP "timeprop_"
#define D_CMND_TIMEPROP_SETPOWER "setpower_"    // add index no on end (0:8) and data is power 0:1

enum TimepropCommands { CMND_TIMEPROP_SETPOWER };
const char kTimepropCommands[] PROGMEM = D_CMND_TIMEPROP_SETPOWER;

static Timeprop timeprops[TIMEPROP_NUM_OUTPUTS];
static int relayNos[TIMEPROP_NUM_OUTPUTS] = {TIMEPROP_RELAYS};
static long currentRelayStates = 0;  // current actual relay states. Bit 0 first relay

static long timeprop_current_time_secs = 0;  // a counter that counts seconds since initialisation

/* call this from elsewhere if required to set the power value for one of the timeprop instances */
/* index specifies which one, 0 up */
void Timeprop_Set_Power( int index, float power )
{
  if (index >= 0  &&  index < TIMEPROP_NUM_OUTPUTS)
  {
    timeprops[index].setPower( power, timeprop_current_time_secs);
  }
}

void Timeprop_Init()
{
  snprintf_P(log_data, sizeof(log_data), "Timeprop Init");
  AddLog(LOG_LEVEL_INFO);
  int cycleTimes[TIMEPROP_NUM_OUTPUTS] = {TIMEPROP_CYCLETIMES};
  int deadTimes[TIMEPROP_NUM_OUTPUTS] = {TIMEPROP_DEADTIMES};
  int opInverts[TIMEPROP_NUM_OUTPUTS] = {TIMEPROP_OPINVERTS};
  int fallbacks[TIMEPROP_NUM_OUTPUTS] = {TIMEPROP_FALLBACK_POWERS};
  int maxIntervals[TIMEPROP_NUM_OUTPUTS] = {TIMEPROP_MAX_UPDATE_INTERVALS};

  for (int i=0; i<TIMEPROP_NUM_OUTPUTS; i++) {
    timeprops[i].initialise(cycleTimes[i], deadTimes[i], opInverts[i], fallbacks[i],
      maxIntervals[i], timeprop_current_time_secs);
  }
}

void Timeprop_Every_Second() {
  timeprop_current_time_secs++;    // increment time
  for (int i=0; i<TIMEPROP_NUM_OUTPUTS; i++) {
    int newState = timeprops[i].tick(timeprop_current_time_secs);
    if (newState != bitRead(currentRelayStates, relayNos[i]-1)){
      // remove the third parameter below if using tasmota prior to v6.0.0
      ExecuteCommandPower(relayNos[i], newState,SRC_IGNORE);
    }
  }
}

// called by the system each time a relay state is changed
void Timeprop_Xdrv_Power() {
  // for a single relay the state is in the lsb of index, I have think that for
  // multiple outputs then succesive bits will hold the state but have not been
  // able to test that
  currentRelayStates = XdrvMailbox.index;
}

/* struct XDRVMAILBOX { */
/*   uint16_t      valid; */
/*   uint16_t      index; */
/*   uint16_t      data_len; */
/*   int16_t       payload; */
/*   char         *topic; */
/*   char         *data; */
/* } XdrvMailbox; */

// To get here post with topic cmnd/timeprop_setpower_n where n is index into timeprops 0:7
boolean Timeprop_Command()
{
  char command [CMDSZ];
  boolean serviced = true;
  uint8_t ua_prefix_len = strlen(D_CMND_TIMEPROP); // to detect prefix of command
  /*
  snprintf_P(log_data, sizeof(log_data), "Command called: "
    "index: %d data_len: %d payload: %d topic: %s data: %s\n",
    XdrvMailbox.index,
    XdrvMailbox.data_len,
    XdrvMailbox.payload,
    (XdrvMailbox.payload >= 0 ? XdrvMailbox.topic : ""),
    (XdrvMailbox.data_len >= 0 ? XdrvMailbox.data : ""));

    AddLog(LOG_LEVEL_INFO);
  */
  if (0 == strncasecmp_P(XdrvMailbox.topic, PSTR(D_CMND_TIMEPROP), ua_prefix_len)) {
    // command starts with timeprop_
    int command_code = GetCommandCode(command, sizeof(command), XdrvMailbox.topic + ua_prefix_len, kTimepropCommands);
    if (CMND_TIMEPROP_SETPOWER == command_code) {
      /*
      snprintf_P(log_data, sizeof(log_data), "Timeprop command timeprop_setpower: "
        "index: %d data_len: %d payload: %d topic: %s data: %s",
	      XdrvMailbox.index,
	      XdrvMailbox.data_len,
	      XdrvMailbox.payload,
	      (XdrvMailbox.payload >= 0 ? XdrvMailbox.topic : ""),
	      (XdrvMailbox.data_len >= 0 ? XdrvMailbox.data : ""));
        AddLog(LOG_LEVEL_INFO);
      */
      if (XdrvMailbox.index >=0 && XdrvMailbox.index < TIMEPROP_NUM_OUTPUTS) {
        timeprops[XdrvMailbox.index].setPower( atof(XdrvMailbox.data), timeprop_current_time_secs );
      }
      snprintf_P(mqtt_data, sizeof(mqtt_data), PSTR("{\"" D_CMND_TIMEPROP D_CMND_TIMEPROP_SETPOWER "%d\":\"%s\"}"),
        XdrvMailbox.index, XdrvMailbox.data);
    }
    else {
      serviced = false;
    }
  } else {
    serviced = false;
  }
  return serviced;
}

/*********************************************************************************************\
 * Interface
\*********************************************************************************************/

#define XDRV_91       91

boolean Xdrv91(byte function)
//boolean XDRV_91(byte function)
{
  boolean result = false;

  switch (function) {
  case FUNC_INIT:
    Timeprop_Init();
    break;
  case FUNC_EVERY_SECOND:
    Timeprop_Every_Second();
    break;
  case FUNC_COMMAND:
    result = Timeprop_Command();
    break;
  case FUNC_SET_POWER:
    Timeprop_Xdrv_Power();
    break;
  }
  return result;
}

#endif // USE_TIMEPROP
