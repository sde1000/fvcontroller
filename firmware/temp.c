#include <stdio.h>
#include <inttypes.h>
#include <avr/eeprom.h>
#include <avr/interrupt.h>
#include "temp.h"
#include "hardware.h"
#include "registers.h"
#include "owb.h"
#include "alarm.h"
#include "timer.h"

/* The hardware reads out temperatures in multiples of 1/16 degree
   (0.0625).  We then take that and apply calibration data,
   potentially interpolating.  This suggests the natural datatype for
   temperature in the firmware is a ten-thousandth of a degree in an
   int32_t. */

int32_t t0_temp=BAD_TEMP;
int32_t t1_temp=BAD_TEMP;
int32_t t2_temp=BAD_TEMP;
int32_t t3_temp=BAD_TEMP;
/* NB v0_state and desired_v0_state are separate because we don't want
   changes to v0_state made by the "jog" code to affect the
   desired_v0_state hysteresis state in the event that t0 is between
   the low and high set points. */
uint8_t v0_state; /* How we are driving v0 at the moment */
uint8_t desired_v0_state; /* Desired valve state: 0=closed, 1=open */
uint8_t v1_output_on;
uint8_t v2_output_on;
static uint8_t jiggling; /* Are we jiggling the valve to unstick it? */

static void trigger_jog_timer(const struct reg *reg)
{
  struct storage s;
  s=reg_storage(reg);
  cli();
  jog_timer=eeprom_read_word((void *)s.loc.eeprom.start);
  sei();
}

/* NB expects name to be a pointer to a string in progmem */
static int32_t read_probe(const char *name)
{
  uint8_t addr[8];
  const struct reg *r;
  struct storage s;
  char regname[9];

  strncpy_P(regname,name,9);
  strncat_P(regname,PSTR("/id"),9);
  r=reg_by_name(regname);
  s=reg_storage(r);
  eeprom_read_block(addr,(void *)s.loc.eeprom.start,8);
  return owb_read_temp(addr);
}

void read_probes(void)
{
  struct storage s;
  int32_t s_hi,s_lo;
  int32_t a_hi,a_lo;
  int32_t j_hi,j_lo;
  uint8_t valve;

  t0_temp=read_probe(PSTR("t0"));
  t1_temp=read_probe(PSTR("t1"));
  t2_temp=read_probe(PSTR("t2"));
  t3_temp=read_probe(PSTR("t3"));

  /* Don't be a thermostat if we don't have a reading */
  if (t0_temp==BAD_TEMP) {
    SET_ALARM(ALARM_NO_TEMPERATURE);
    return;
  }
  UNSET_ALARM(ALARM_NO_TEMPERATURE);

  /* Read necessary registers from eeprom */
  s=reg_storage(&set_hi);
  eeprom_read_block(&s_hi,(void *)s.loc.eeprom.start,4);
  s=reg_storage(&set_lo);
  eeprom_read_block(&s_lo,(void *)s.loc.eeprom.start,4);
  s=reg_storage(&alarm_hi);
  eeprom_read_block(&a_hi,(void *)s.loc.eeprom.start,4);
  s=reg_storage(&alarm_lo);
  eeprom_read_block(&a_lo,(void *)s.loc.eeprom.start,4);
  s=reg_storage(&jog_hi);
  eeprom_read_block(&j_hi,(void *)s.loc.eeprom.start,4);
  s=reg_storage(&jog_lo);
  eeprom_read_block(&j_lo,(void *)s.loc.eeprom.start,4);
  s=reg_storage(&vtype);
  valve=eeprom_read_byte((void *)s.loc.eeprom.start);

  /* Check alarm temperatures */
  if (t0_temp>a_hi) {
    SET_ALARM(ALARM_TEMPERATURE_HIGH);
  } else {
    UNSET_ALARM(ALARM_TEMPERATURE_HIGH);
  }
  if (t0_temp<a_lo) {
    SET_ALARM(ALARM_TEMPERATURE_LOW);
  } else {
    UNSET_ALARM(ALARM_TEMPERATURE_LOW);
  }
  
  /* Be a thermostat, with valve opened to provide chilling */
  if (t0_temp>s_hi) {
    desired_v0_state=1;
  }
  if (t0_temp<s_lo) {
    desired_v0_state=0;
  }

  v0_state=desired_v0_state;

  /* Check "jog" temperatures.  If temperature is out of bounds, we
     assume that our valve may be stuck and jiggle it to try to free
     it.  We invert v0_state for jog/flip and then wait jog/wait
     before trying again.  We have one timer for this, jog_timer,
     which counts down to zero and then stays there until we reset it.
  */
  if (t0_temp<j_lo || t0_temp>j_hi) {
    SET_ALARM(ALARM_VALVE_STUCK);
    if (jog_timer==0) {
      jiggling=!jiggling;
      if (jiggling) {
	trigger_jog_timer(&jog_flip);
      } else {
	trigger_jog_timer(&jog_wait);
      }
    }
    if (jiggling) v0_state=!v0_state;
  } else {
    /* Temperature is within bounds.  If the flip timer expires, start the
       wait timer. */
    UNSET_ALARM(ALARM_VALVE_STUCK);
    if (jiggling && jog_timer==0) {
      jiggling=0;
      trigger_jog_timer(&jog_wait);
    }
  }

  switch (valve) {
  case 0:
  case 0xff:
    /* Spring-return valve on VALVE1, nothing on VALVE2 */
    if (v0_state && !v1_output_on) {
      trigger_relay(VALVE1_SET);
      v1_output_on=1;
    }
    if (!v0_state && v1_output_on) {
      trigger_relay(VALVE1_RESET);
      v1_output_on=0;
    }
    break;
  case 1: /* Ball valve: open on VALVE1, close on VALVE2, no limit sensors */
  case 2: /* As case 1, but with limit sensors */
    if (v0_state) {
      /* VALVE1 should be energised, VALVE2 should be de-energised */
      if (v2_output_on) {
	trigger_relay(VALVE2_RESET);
	v2_output_on=0;
      }
      if (!v1_output_on) {
	trigger_relay(VALVE1_SET);
	v1_output_on=1;
      }
    } else {
      /* VALVE1 should be de-energised, VALVE2 should be energised */
      if (v1_output_on) {
	trigger_relay(VALVE1_RESET);
	v1_output_on=0;
      }
      if (!v2_output_on) {
	trigger_relay(VALVE2_SET);
	v2_output_on=1;
      }
    }
    break;
  }
}

uint8_t get_valve_state(void)
{
  struct storage s;
  uint8_t valve,v1,v2;

  /* Read valve mode from eeprom */
  s=reg_storage(&vtype);
  valve=eeprom_read_byte((void *)s.loc.eeprom.start);

  v1=read_valve(VALVE1_STATE);
  v2=read_valve(VALVE2_STATE);

  switch (valve) {
  case 0:
  case 0xff:
    /* Spring-return valve on VALVE1, nothing on VALVE2 */
    if (v0_state) {
      if (v1) return VALVE_OPEN;
      else return VALVE_OPENING;
    } else {
      if (v1) return VALVE_CLOSING;
      else return VALVE_CLOSED;
    }
    break;
  case 1: /* Ball valve with no limit sensors */
    if (v0_state) return VALVE_OPEN;
    else return VALVE_CLOSED;
    break;
  case 2: /* Ball valve with open limit sensor on VALVE1 and closed limit
	     sensor on VALVE2 */
    if (v0_state) {
      if (v1 && !v2) return VALVE_OPEN;
      if (v1 && v2) return VALVE_ERROR;
      return VALVE_OPENING;
    } else {
      if (v2 && !v1) return VALVE_CLOSED;
      if (v1 && v2) return VALVE_ERROR;
      return VALVE_CLOSING;
    }
    break;
  }
  return VALVE_ERROR;
}
