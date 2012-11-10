/*
 * hald-addon-bme.c: bme addon for maemo http://wiki.maemo.org/Mer
 *
 * Copyright (C) 2012 Ivaylo Dimitrov <freemangordon@abv.bg>
 * Copyright (C) 2012 Pali Rohár <pali.rohar@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include <fcntl.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

#include <glib/gmain.h>

#include <gio/gio.h>

#include <dbus/dbus-glib-lowlevel.h>

#include <hal/libhal.h>

#include <dsme/protocol.h>
#include <dsme/state.h>

typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef int32_t int32;

typedef struct {
  enum{STATUS_FULL,STATUS_CHARGING,STATUS_DISCHARGING}power_supply_status;
  uint32 power_supply_present;
  uint32 power_supply_voltage_design;
  uint32 power_supply_voltage_now;
  int32  power_supply_capacity;
  uint32 power_supply_time_to_empty_avg;
  uint32 power_supply_time_to_full_now;
  uint32 power_supply_charge_design;
  uint32 power_supply_charge_full;
  uint32 power_supply_charge_now;
  int32  power_supply_flags_register;
  char   power_supply_capacity_level[32];
  char   power_supply_mode[32];
} battery;

battery global_battery;

#define POWER_SUPPLY_CAPACITY_THRESHOLD_FULL 95

#define POWER_SUPPLY_CHARGE_THRESHOLD_LOW 200
#define POWER_SUPPLY_CHARGE_THRESHOLD_VERYLOW 80
#define POWER_SUPPLY_CHARGE_THRESHOLD_EMPTY 20

#define POWER_SUPPLY_VOLTAGE_THRESHOLD_FULL 4050
#define POWER_SUPPLY_VOLTAGE_THRESHOLD_LOW 3580
#define POWER_SUPPLY_VOLTAGE_THRESHOLD_VERYLOW 3248
#define POWER_SUPPLY_VOLTAGE_THRESHOLD_EMPTY 3000

typedef struct {
  struct {
    enum {EMPTY,LOW,OK,FULL}capacity_state;
    uint32 current;
    uint32 percentage;
  }charge_level;
}bme;

bme global_bme;

int global_charger_connected = 0;

dsmesock_connection_t * dsme_conn;

enum { PATTERN_NONE, PATTERN_FULL, PATTERN_CHARGING, PATTERN_BOOST } global_pattern;

/*#define DEBUG*/

#define DEBUG_FILE      "/tmp/hald-addon-bme.log"

#define BQ27200_UEVENT_FILE_PATH "/sys/class/power_supply/bq27200-0/uevent"
#define BQ27200_REGISTERS_FILE_PATH "/sys/class/power_supply/bq27200-0/registers"
#define BQ24150A_MODE_FILE_PATH "/sys/class/power_supply/bq24150a-0/mode"
#define BQ24150A_STAT_PIN_FILE_PATH "/sys/class/power_supply/bq24150a-0/stat_pin_enable"
#define RX51_UEVENT_FILE_PATH "/sys/class/power_supply/rx51-battery/uevent"

/*
Standard entries:
  capacity            device              technology          type
  charge_full         power               temp                uevent
  charge_full_design  present             time_to_empty_avg   voltage_now
  charge_now          status              time_to_empty_now
  current_now         subsystem           time_to_full_now

Additional upstream entries:
  capacity_level
  cycle_count
  energy_now

Additional maemo entries:
  registers
*/

/*
BME hal properties:
  battery.charge_level.capacity_state = 'ok'  (string)
  battery.charge_level.current = 5  (0x5)  (int)
  battery.charge_level.design = 8  (0x8)  (int)
  battery.charge_level.last_full = 0  (0x0)  (int)
  battery.charge_level.percentage = 53  (0x35)  (int)
  battery.charge_level.unit = 'bars'  (string)
  battery.is_rechargeable = true  (bool)
  battery.present = true  (bool)
  battery.rechargeable.is_charging = true  (bool)
  battery.rechargeable.is_discharging = false  (bool)
  battery.remaining_time = 7200  (0x1c20)  (int)
  battery.remaining_time.calculate_per_time = false  (bool)
  battery.reporting.current = 682  (0x2aa)  (int)
  battery.reporting.design = 1275  (0x4fb)  (int)
  battery.reporting.last_full = 0  (0x0)  (int)
  battery.reporting.unit = 'mAh'  (string)
  battery.type = 'pda'  (string)
  battery.voltage.current = 3868  (0xf1c)  (int)
  battery.voltage.design = 4200  (0x1068)  (int)
  battery.voltage.unit = 'mV'  (string)
  info.addons = {'hald-addon-bme'} (string list)
  info.capabilities = {'battery'} (string list)
  info.category = 'battery'  (string)
  info.parent = '/org/freedesktop/Hal/devices/computer'  (string)
  info.product = 'Battery (BME-HAL)'  (string)
  info.subsystem = 'unknown'  (string)
  info.udi = '/org/freedesktop/Hal/devices/bme'  (string)
  maemo.charger.connection_status = 'connected'  (string)
  maemo.charger.type = 'host 500 mA'  (string)
  maemo.rechargeable.charging_status = 'on'  (string)
  maemo.rechargeable.positive_rate = false  (bool)
*/
DBusConnection *hal_dbus = 0;
DBusConnection *system_dbus = 0;
DBusConnection *server_dbus = 0;
LibHalContext *hal_ctx = 0;
const char *udi = 0;
GMainLoop *mainloop = 0;
gboolean display_status_ind=FALSE;
guint poll_period = 30;

static void cleanup_system_dbus()
{
  if ( system_dbus )
  {
    dbus_connection_unref(system_dbus);
    system_dbus = 0;
  }
}

static void log_print(const char *fmt,...)
{
#ifdef DEBUG
    static FILE *fp=0;
    va_list va;

    if(!fp)
#ifdef DEBUG_FILE
        fp=fopen(DEBUG_FILE,"wt");
#else
        fp=stdout;
#endif

    fprintf(fp,"hald-addon-bme: ");
    va_start(va,fmt);
    vfprintf(fp,fmt,va);
    va_end(va);
    fprintf(fp,"\n");
    fflush(0);
#else
    (void)fmt;
#endif
}

static void print_dbus_error(const char *text,DBusError *error)
{
  if(text&&error)
  {
      dbus_error_is_set(error);
      log_print("%s (%s: %s)\n",text,error->name,error->message);
      dbus_error_free(error);
  }
}

static gboolean hald_addon_bme_setup_hal(void)
{
  DBusError error;
  gboolean result = FALSE;

  dbus_error_init(&error);

  if( !(hal_ctx = libhal_ctx_init_direct(&error)) )
  {
    if ( dbus_error_is_set(&error) )
      print_dbus_error("hal context init: %s: %s\n",&error);
    else
      log_print(("hal context init: unknown failure\n"));
    goto out;
  }

  if( !(hal_dbus = libhal_ctx_get_dbus_connection(hal_ctx)) )
  {
    log_print("hal context does not have dbus connection\n");
    goto out;
  }

  udi = getenv("UDI");
  if (!udi)
  {
    log_print ("hal UDI not set in the environment\n");
    goto out;
  }
  udi=g_strdup(udi);  /* device id */
  log_print("UDI: %s",udi);

  if( !libhal_device_addon_is_ready(hal_ctx,udi,&error) )
  {
    if ( dbus_error_is_set(&error) )
      print_dbus_error("hal addon is ready: %s: %s\n",&error);
    else
      log_print("hal addon is ready: unknown failure\n");
    goto out;
  }

  dbus_connection_setup_with_g_main(hal_dbus, FALSE);
  dbus_connection_set_exit_on_disconnect(hal_dbus ,FALSE);
  result = TRUE;

out:
  dbus_error_free(&error);
  return result;
}

static gboolean send_dbus_signal(const char *name, int first_arg_type, ...)
{
  DBusMessage * msg;
  gboolean result = FALSE;
  va_list va;

  va_start(va, first_arg_type);

  msg = dbus_message_new_signal("/com/nokia/bme/signal", "com.nokia.bme.signal", name);
  if (msg &&
      dbus_message_append_args_valist(msg, first_arg_type, va) &&
      dbus_connection_send(system_dbus, msg, 0))
  {
    dbus_connection_flush(system_dbus);
    result = TRUE;
  }

  if (msg)
    dbus_message_unref(msg);

  return result;
}

static gboolean send_dbus_signal_(const char *name)
{
  log_print("send dbus signal: %s\n", name);
  return send_dbus_signal(name, DBUS_TYPE_INVALID);
}

static gboolean send_capacity_state_change()
{
  const char * name ;
  switch(global_bme.charge_level.capacity_state)
  {
    case LOW:name = "battery_low";break;
    case FULL:name = "battery_full";break;
    case EMPTY:name = "battery_empty";break;
    default:return TRUE;
  }
  if (global_bme.charge_level.capacity_state == EMPTY)
  {
    DSM_MSGTYPE_SET_BATTERY_STATE msg =
      DSME_MSG_INIT(DSM_MSGTYPE_SET_BATTERY_STATE);
    msg.empty = 1;
    dsmesock_send(dsme_conn, &msg);
  }
  return send_dbus_signal_(name);
}

static gboolean send_battery_state_changed(uint32 now)
{
  uint32 max = 8;
  log_print("send dbus signal: %s (bars=%d/%d)\n", "battery_state_changed",now,max);
  return send_dbus_signal("battery_state_changed",DBUS_TYPE_UINT32, &now, DBUS_TYPE_UINT32, &max, DBUS_TYPE_INVALID);
}

static gboolean hald_addon_bme_get_rx51_data(battery * battery_info)
{
  FILE * fp;

  char line[256];
  if((fp = fopen(RX51_UEVENT_FILE_PATH,"r")) == NULL)
  {
    log_print("unable to open %s(%s)\n",RX51_UEVENT_FILE_PATH,strerror(errno));
    return FALSE;
  }
  while(fgets(line,sizeof(line),fp))
  {
    char*tmp;
    tmp = strchr(line,'=');
    if(tmp)
    {
      *tmp=0;
      tmp++;
      tmp[strlen(tmp)-1] = 0;
      if(!strcmp(line,"POWER_SUPPLY_VOLTAGE_MAX_DESIGN"))
        battery_info->power_supply_voltage_design = atoi(tmp)/1000;
      else if(!strcmp(line,"POWER_SUPPLY_VOLTAGE_NOW"))
        battery_info->power_supply_voltage_now = atoi(tmp)/1000;
      else if(!strcmp(line,"POWER_SUPPLY_CHARGE_FULL_DESIGN"))
      {
        battery_info->power_supply_charge_design = atoi(tmp)/1000;
        if(battery_info->power_supply_charge_design > 0 && global_battery.power_supply_charge_design > 0 && abs(global_battery.power_supply_charge_design - battery_info->power_supply_charge_design) < 100)
          battery_info->power_supply_charge_design = global_battery.power_supply_charge_design;
      }
    }
  }
  fclose(fp);

  return TRUE;
}

static gboolean hald_addon_bme_get_bq27200_data(battery * battery_info)
{
  FILE * fp;

  char line[256];
  if((fp = fopen(BQ27200_UEVENT_FILE_PATH,"r")) == NULL)
  {
    log_print("unable to open %s(%s)\n",BQ27200_UEVENT_FILE_PATH,strerror(errno));
    return FALSE;
  }
  while(fgets(line,sizeof(line),fp))
  {
    char*tmp;
    tmp = strchr(line,'=');
    if(tmp)
    {
      *tmp=0;
      tmp++;
      tmp[strlen(tmp)-1] = 0;
      if(!strcmp(line,"POWER_SUPPLY_CAPACITY"))
        battery_info->power_supply_capacity = atoi(tmp);
      else if(!strcmp(line,"POWER_SUPPLY_STATUS"))
      {
        if (!strcmp(tmp,"Full")) battery_info->power_supply_status = STATUS_FULL;
        else if (!strcmp(tmp,"Charging")) battery_info->power_supply_status = STATUS_CHARGING;
        else battery_info->power_supply_status = STATUS_DISCHARGING;
      }
      else if(!strcmp(line,"POWER_SUPPLY_VOLTAGE_NOW"))
        battery_info->power_supply_voltage_now = atoi(tmp)/1000;
      else if(!strcmp(line,"POWER_SUPPLY_TIME_TO_FULL_NOW"))
        battery_info->power_supply_time_to_full_now = atoi(tmp);
      else if(!strcmp(line,"POWER_SUPPLY_TIME_TO_EMPTY_AVG"))
        battery_info->power_supply_time_to_empty_avg = atoi(tmp);
      else if(!strcmp(line,"POWER_SUPPLY_CHARGE_FULL"))
        battery_info->power_supply_charge_full = atoi(tmp)/1000;
      else if(!strcmp(line,"POWER_SUPPLY_CHARGE_NOW"))
        battery_info->power_supply_charge_now = atoi(tmp)/1000;
      else if(!strcmp(line,"POWER_SUPPLY_CAPACITY_LEVEL"))
        strncpy(battery_info->power_supply_capacity_level,
                tmp,
                sizeof(battery_info->power_supply_capacity_level)-1);
    }
  }
  fclose(fp);

  return TRUE;
}

static gboolean hald_addon_bme_get_bq27200_registers(battery * battery_info)
{
  FILE * fp;

  char line[256];
  if((fp = fopen(BQ27200_REGISTERS_FILE_PATH,"r")) == NULL)
  {
    log_print("unable to open %s(%s)\n",BQ27200_REGISTERS_FILE_PATH,strerror(errno));
    return FALSE;
  }
  while(fgets(line,sizeof(line),fp))
  {
    char*tmp;
    tmp = strchr(line,'=');
    if(tmp)
    {
      *tmp=0;
      tmp++;
      tmp[strlen(tmp)-1] = 0;
      if(!strcmp(line,"0x0a"))
        battery_info->power_supply_flags_register = strtol(tmp, NULL, 16);
    }
  }
  fclose(fp);

  return TRUE;
}

static void hald_addon_bme_status_info()
{
  log_print("%s\n",__func__);
  send_dbus_signal_(global_charger_connected ? "charger_connected" : "charger_disconnected");
  if (global_bme.charge_level.capacity_state != FULL)
    send_dbus_signal_(global_charger_connected ? "charger_charging_on" : "charger_charging_off");
  send_battery_state_changed(global_bme.charge_level.current);
}

static void hald_addon_bme_timeleft_info()
{
  uint32 minutes = 0;
  log_print("%s\n",__func__);
  if (global_battery.power_supply_time_to_empty_avg)
    minutes = global_battery.power_supply_time_to_empty_avg/60;
  else if (global_battery.power_supply_time_to_full_now)
    minutes = global_battery.power_supply_time_to_full_now/60;
  send_dbus_signal("battery_timeleft",
      DBUS_TYPE_UINT32, &minutes,
      DBUS_TYPE_UINT32, &minutes,
      DBUS_TYPE_INVALID);
}

static DBusHandlerResult hald_addon_bme_dbus_proxy(DBusConnection *connection, DBusMessage *message, void *user_data G_GNUC_UNUSED)
{
  const char *interface, *member, *path;
  int type;

  interface = dbus_message_get_interface(message);
  member = dbus_message_get_member(message);
  path = dbus_message_get_path(message);

  type = dbus_message_get_type(message);

  if(!interface ||
     !member ||
     !path ||
     (type != DBUS_MESSAGE_TYPE_SIGNAL && type != DBUS_MESSAGE_TYPE_METHOD_CALL) ||
     strcmp(interface, "com.nokia.bme.request") ||
     strcmp(path, "/com/nokia/bme/request"))
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  log_print("DBUS RECV: %s %s.%s\n\n", path,interface,dbus_message_type_to_string(type));

  if (!strcmp(member, "status_info_req") )
  {
    log_print("got: BME_STATUS_INFO_REQ\n");
    hald_addon_bme_status_info();
  }
  else
  {
    if(strcmp(member, "timeleft_info_req"))
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    log_print("got: BME_TIMELEFT_INFO_REQ\n");
    hald_addon_bme_timeleft_info();
  }

  if(type == DBUS_MESSAGE_TYPE_METHOD_CALL)
  {
    if ( !dbus_message_get_no_reply(message) )
    {
      DBusMessage * msg = dbus_message_new_method_return(message);
      if ( msg )
      {
        dbus_connection_send(connection, msg, 0);
        dbus_message_unref(msg);
      }
    }
  }

  return DBUS_HANDLER_RESULT_HANDLED;
}

static gint hald_addon_bme_setup_dbus_proxy()
{
  DBusError error;
  gint result = -1;

  dbus_error_init(&error);

  system_dbus = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
  if(!system_dbus)
    goto error;
  else
  {
    dbus_connection_setup_with_g_main(system_dbus, 0);
    dbus_connection_set_exit_on_disconnect(system_dbus, FALSE);
    if(dbus_connection_add_filter(system_dbus, hald_addon_bme_dbus_proxy, NULL, NULL))
    {
      dbus_bus_add_match(system_dbus,
                         "type='signal',interface='com.nokia.bme.request'",
                         &error);
      if ( dbus_error_is_set(&error) )
        goto dbus_error;
      else
      {
        if ( !dbus_bus_request_name(system_dbus,
                                   "com.nokia.bme",
                                   DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER,
                                   &error))
        {
          if ( dbus_error_is_set(&error) )
            goto dbus_error;
        }
      }
    }
    else
      goto error;
  }
  result = 0;
  goto out;
error:
  log_print("proxy_init");
  goto out;
dbus_error:
    log_print("proxy_init %s: %s\n",error.name,error.message);
out:
  dbus_error_free(&error);
  return result;
}


static DBusHandlerResult hald_addon_bme_mce_signal(DBusConnection *connection G_GNUC_UNUSED, DBusMessage *message, void *user_data G_GNUC_UNUSED)
{
  const char *interface, *member, *path;
  int type;
  DBusError error;

  dbus_error_init(&error);

  interface = dbus_message_get_interface(message);
  member = dbus_message_get_member(message);
  path = dbus_message_get_path(message);

  type = dbus_message_get_type(message);
  if(interface &&
     member &&
     path &&
     type == DBUS_MESSAGE_TYPE_SIGNAL &&
     !strcmp(interface, "com.nokia.mce.signal") &&
     !strcmp(path, "/com/nokia/mce/signal") &&
     !strcmp(member, "display_status_ind"))
  {
    const char * tmp = 0, *status = "NULL";
    dbus_message_get_args(message,
                          &error,
                          DBUS_TYPE_STRING,
                          &tmp,
                          DBUS_TYPE_INVALID);
    if(tmp)
      status = tmp;

    log_print("MCE RECV: MCE_DISPLAY_SIG '%s'\n\n", status);

    if(!tmp || strcmp(tmp,"on"))
      display_status_ind = FALSE;
    else
    {
      display_status_ind = TRUE;
    }
  }

  dbus_error_free(&error);

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static gint hald_addon_bme_server_dbus_init()
{
  DBusError error;
  gint result = -1;

  dbus_error_init(&error);

  if(!(server_dbus = dbus_bus_get(DBUS_BUS_SYSTEM, &error)))
  {
    if (dbus_error_is_set(&error))
      print_dbus_error("server_dbus_init",&error);
    else
      log_print("server_dbus_init");
    goto out;
  }

  dbus_connection_setup_with_g_main(server_dbus, 0);
  dbus_connection_set_exit_on_disconnect(server_dbus, FALSE);

  if(!dbus_connection_add_filter(system_dbus, hald_addon_bme_mce_signal, NULL, NULL))
  {
    log_print("server_dbus_init");
    goto out;
  }
  dbus_bus_add_match(system_dbus,
                     "type='signal',interface='com.nokia.mce.signal'",
                     &error);
  if (dbus_error_is_set(&error))
  {
    print_dbus_error("server_dbus_init",&error);
    goto out;
  }
  result = 0;

out:
  dbus_error_free(&error);
  return result;
}

static const char * get_capacity_state_string()
{
  switch(global_bme.charge_level.capacity_state)
  {
    case LOW:return "low";
    case FULL:return "full";
    case EMPTY:return "empty";
    default:return "ok";
  }
}

static gboolean hald_addon_bme_update_hal(battery * battery_info,gboolean check_for_changes)
{
#define CHECK_INT(f,fun) if( !check_for_changes || (global_battery.f != battery_info->f)) \
  log_print(#f " changed,updating to %d",battery_info->f);fun

  uint32 charge_level_current;
  uint32 capacity_state;
  int calibrated;
  int charger_connected;
  int capacity;
  int very_low = 0;

  if(battery_info->power_supply_capacity < 0)
    calibrated = 0;
  else
    calibrated = 1;

  if (strstr(battery_info->power_supply_mode, "host") || strstr(battery_info->power_supply_mode, "dedicated"))
    charger_connected = 1;
  else
    charger_connected = 0;

  if(!check_for_changes)
  {
    libhal_device_set_property_string(hal_ctx, udi, "battery.charge_level.capacity_state", "ok", NULL);
    libhal_device_set_property_int(hal_ctx, udi, "battery.charge_level.current", 0, NULL);
    libhal_device_set_property_int(hal_ctx, udi, "battery.charge_level.design", 8, NULL); /* STATIC */
    libhal_device_set_property_int(hal_ctx, udi, "battery.charge_level.last_full", 0, NULL);
    libhal_device_set_property_int(hal_ctx, udi, "battery.charge_level.percentage", 0, NULL);
    libhal_device_set_property_string(hal_ctx, udi, "battery.charge_level.unit", "bars", NULL); /* STATIC */
    libhal_device_set_property_bool(hal_ctx, udi, "battery.is_rechargeable", TRUE, NULL); /* STATIC */
    libhal_device_set_property_bool(hal_ctx, udi, "battery.present", TRUE, NULL); /* STATIC */
    libhal_device_set_property_bool(hal_ctx, udi, "battery.rechargeable.is_charging", FALSE, NULL);
    libhal_device_set_property_bool(hal_ctx, udi, "battery.rechargeable.is_discharging", TRUE, NULL);
    libhal_device_set_property_int(hal_ctx, udi, "battery.remaining_time", 0, NULL);
    libhal_device_set_property_bool(hal_ctx, udi, "battery.remaining_time.calculate_per_time", FALSE, NULL); /* STATIC */
    libhal_device_set_property_int(hal_ctx, udi, "battery.reporting.current", 0, NULL);
    libhal_device_set_property_int(hal_ctx, udi, "battery.reporting.design", 0, NULL);
    libhal_device_set_property_int(hal_ctx, udi, "battery.reporting.last_full", 0, NULL);
    libhal_device_set_property_string(hal_ctx, udi, "battery.reporting.unit", "mAh", NULL); /* STATIC */
    libhal_device_set_property_string(hal_ctx, udi, "battery.type","pda", NULL); /* STATIC */
    libhal_device_set_property_int(hal_ctx, udi, "battery.voltage.current", 0, NULL);
    libhal_device_set_property_int(hal_ctx, udi, "battery.voltage.design", 4200, NULL);
    libhal_device_set_property_string(hal_ctx, udi, "battery.voltage.unit", "mV", NULL); /* STATIC */
    libhal_device_set_property_string(hal_ctx, udi, "maemo.charger.connection_status", "disconnected", NULL);
    libhal_device_set_property_string(hal_ctx, udi, "maemo.charger.type", "none", NULL);
    libhal_device_set_property_string(hal_ctx, udi, "maemo.rechargeable.charging_status", "off", NULL);
    libhal_device_set_property_bool(hal_ctx, udi, "maemo.rechargeable.positive_rate", FALSE, NULL); /* STATIC */
  }

  CHECK_INT(power_supply_voltage_now,
        libhal_device_set_property_int (hal_ctx, udi, "battery.voltage.current", battery_info->power_supply_voltage_now, NULL));

  CHECK_INT(power_supply_voltage_design,
        libhal_device_set_property_int (hal_ctx, udi, "battery.voltage.design", battery_info->power_supply_voltage_design, NULL));

  CHECK_INT(power_supply_charge_design,
        libhal_device_set_property_int (hal_ctx, udi, "battery.reporting.design", battery_info->power_supply_charge_design, NULL));

  /* we should tell capacity is 0% when battery is very low */
  /* very low is someting between empty and low, so EDV1 flag can be used for that */
  /* EDV1 is set when capacity is 6%, so recalculate capacity from 6%-100% to 0%-100% (to tell 0% when battery is very low which means battery has 6%) */

  /* bq27200 report percentage capacity against last full capacity */
  /* we will recalculate percentage capacity against design capacity reported by rx51_battery (if driver is available) */
  if(calibrated && battery_info->power_supply_charge_now > 0 && battery_info->power_supply_charge_design > 0)
    capacity = 100 * (100*battery_info->power_supply_charge_now - 6*battery_info->power_supply_charge_design) / (94*battery_info->power_supply_charge_design);
  else if(calibrated && battery_info->power_supply_charge_now > 0 && battery_info->power_supply_charge_full > 0)
    capacity = 100 * (100*battery_info->power_supply_charge_now - 6*battery_info->power_supply_charge_full) / (94*battery_info->power_supply_charge_full);
  else /* when battery is not calibrated or other data is missing, report some capacity from voltage */
  {
    if (!charger_connected)
    {
      if (battery_info->power_supply_voltage_now <= POWER_SUPPLY_VOLTAGE_THRESHOLD_EMPTY)
        battery_info->power_supply_capacity = 0;
      else if (battery_info->power_supply_voltage_now <= POWER_SUPPLY_VOLTAGE_THRESHOLD_VERYLOW)
        battery_info->power_supply_capacity = 6;
      else if (battery_info->power_supply_voltage_now <= POWER_SUPPLY_VOLTAGE_THRESHOLD_LOW)
        battery_info->power_supply_capacity = 20;
      else if (battery_info->power_supply_voltage_now > POWER_SUPPLY_VOLTAGE_THRESHOLD_FULL)
        battery_info->power_supply_capacity = 100;
      else
        battery_info->power_supply_capacity = 53;
      capacity = 100*(battery_info->power_supply_capacity-6)/94;
    }
    else
    {
      if (battery_info->power_supply_voltage_now <= 4036) /* 11% */
        capacity = 0;
      else if (battery_info->power_supply_voltage_now <= 4089) /* 24% */
        capacity = 13;
      else if (battery_info->power_supply_voltage_now <= 4099) /* 35% */
        capacity = 25;
      else if (battery_info->power_supply_voltage_now <= 4110) /* 47% */
        capacity = 38;
      else if (battery_info->power_supply_voltage_now <= 4120) /* 58% */
        capacity = 50;
      else if (battery_info->power_supply_voltage_now <= 4134) /* 71% */
        capacity = 63;
      else if (battery_info->power_supply_voltage_now <= 4150) /* 82% */
        capacity = 75;
      else if (battery_info->power_supply_voltage_now <= 4168) /* 94% */
        capacity = 88;
      else
        capacity = 100;
      battery_info->power_supply_capacity = capacity*94/100+6;
    }
  }

  if (capacity < 0)
    capacity = 0;

  /* capacity_level is in upstream kernel */
  if(battery_info->power_supply_capacity_level[0])
  {
    if (!strcmp(battery_info->power_supply_capacity_level, "Full"))
      capacity_state = FULL;
    else if (!strcmp(battery_info->power_supply_capacity_level, "High"))
      capacity_state = FULL;
    else if (!strcmp(battery_info->power_supply_capacity_level, "Normal"))
    {
      if (battery_info->power_supply_charge_now <= POWER_SUPPLY_CHARGE_THRESHOLD_LOW && calibrated)
        capacity_state = LOW;
      else if(battery_info->power_supply_voltage_now <= POWER_SUPPLY_VOLTAGE_THRESHOLD_LOW && !calibrated)
        capacity_state = LOW;
      else
        capacity_state = OK;
    }
    else if (!strcmp(battery_info->power_supply_capacity_level, "Low")) {
      capacity_state = LOW;
      very_low = 1;
    } else if (!strcmp(battery_info->power_supply_capacity_level, "Critical"))
      capacity_state = EMPTY;
    else
      capacity_state = OK;
  }
  /* registers is in maemo kernel-power */
  /* code taken from upstream kernel */
  else if (battery_info->power_supply_flags_register >= 0)
  {
    if (battery_info->power_supply_flags_register & 0x20) /* FLAG_FC */
      capacity_state = FULL;
    else if (battery_info->power_supply_flags_register & 0x01) /* FLAG_EDVF */
      capacity_state = EMPTY;
    else if (battery_info->power_supply_flags_register & 0x02) { /* FLAG_EDV1 */
      capacity_state = LOW;
      very_low = 1;
    } else if (battery_info->power_supply_charge_now <= POWER_SUPPLY_CHARGE_THRESHOLD_LOW && calibrated)
      capacity_state = LOW;
    else if(battery_info->power_supply_voltage_now <= POWER_SUPPLY_VOLTAGE_THRESHOLD_LOW && !calibrated)
      capacity_state = LOW;
    else
      capacity_state = OK;
  }
  /* no maemo kernel-power or upstream kernel, but battery is calibrated */
  /* check charge_now threshold */
  else if (calibrated)
  {
    if (battery_info->power_supply_charge_now <= POWER_SUPPLY_CHARGE_THRESHOLD_EMPTY)
      capacity_state = EMPTY;
    else if (battery_info->power_supply_charge_now <= POWER_SUPPLY_CHARGE_THRESHOLD_VERYLOW) {
      very_low = 1;
      capacity_state = LOW;
    } else if (battery_info->power_supply_charge_now <= POWER_SUPPLY_CHARGE_THRESHOLD_LOW)
      capacity_state = LOW;
    else if (battery_info->power_supply_capacity > POWER_SUPPLY_CAPACITY_THRESHOLD_FULL)
      capacity_state = FULL;
    else
      capacity_state = OK;
  }
  /* battery is not calibrated and no access to FC, EDV1 or EDVF flags */
  /* use voltage threshold */
  else
  {
    if (battery_info->power_supply_voltage_now <= POWER_SUPPLY_VOLTAGE_THRESHOLD_EMPTY)
      capacity_state = EMPTY;
    else if (battery_info->power_supply_voltage_now <= POWER_SUPPLY_VOLTAGE_THRESHOLD_VERYLOW) {
      very_low = 1;
      capacity_state = LOW;
    } else if (battery_info->power_supply_voltage_now <= POWER_SUPPLY_VOLTAGE_THRESHOLD_LOW)
      capacity_state = LOW;
    else if (battery_info->power_supply_voltage_now > POWER_SUPPLY_VOLTAGE_THRESHOLD_FULL)
      capacity_state = FULL;
    else
      capacity_state = OK;
  }

  if (battery_info->power_supply_status == STATUS_FULL)
    capacity_state = FULL;

  if ((capacity_state == LOW || capacity_state == EMPTY) && charger_connected)
    capacity_state = OK;

  if (capacity_state == FULL && !calibrated)
  {
    battery_info->power_supply_capacity = 100;
    capacity = 100;
  }

  if (very_low)
    capacity = 0;

  CHECK_INT(power_supply_capacity,
    libhal_device_set_property_int(hal_ctx, udi, "battery.charge_level.percentage", capacity, NULL));

  if(check_for_changes && (
       global_bme.charge_level.capacity_state != capacity_state ||
       capacity_state == EMPTY ||
       (capacity_state == LOW && battery_info->power_supply_capacity != global_battery.power_supply_capacity && battery_info->power_supply_capacity%2 == 0) ||
       (capacity == 0 && battery_info->power_supply_capacity != global_battery.power_supply_capacity)
    ))
  {
    global_bme.charge_level.capacity_state = capacity_state;
    log_print("capacity state changed to %s\n", get_capacity_state_string());
    /* Before changing capacity_state to new value, battery status area plugin needs empty string first */
    libhal_device_set_property_string(hal_ctx, udi, "battery.charge_level.capacity_state", "", NULL);
    libhal_device_set_property_string(hal_ctx, udi, "battery.charge_level.capacity_state", get_capacity_state_string(), NULL);
    send_capacity_state_change();
  }

  if (capacity_state == FULL && charger_connected)
  {
    libhal_device_set_property_string(hal_ctx, udi, "maemo.rechargeable.charging_status", "full", NULL);
    libhal_device_set_property_bool(hal_ctx, udi, "battery.rechargeable.is_discharging", true, NULL);
    libhal_device_set_property_bool(hal_ctx, udi, "battery.rechargeable.is_charging", true, NULL);
  }
  else
  {
    libhal_device_set_property_string(hal_ctx, udi, "maemo.rechargeable.charging_status", charger_connected ? "on" : "off", NULL);
    libhal_device_set_property_bool(hal_ctx, udi, "battery.rechargeable.is_discharging", !charger_connected, NULL);
    libhal_device_set_property_bool(hal_ctx, udi, "battery.rechargeable.is_charging", charger_connected, NULL);
  }

  if (!calibrated && battery_info->power_supply_charge_design)
    battery_info->power_supply_charge_now = capacity*battery_info->power_supply_charge_design/100;

  CHECK_INT(power_supply_charge_now,
        libhal_device_set_property_int (hal_ctx, udi, "battery.reporting.current", battery_info->power_supply_charge_now, NULL));

  if (calibrated)
  {
    if (battery_info->power_supply_charge_design > 0)
    {
      if (battery_info->power_supply_charge_full > battery_info->power_supply_charge_design)
        battery_info->power_supply_charge_full = battery_info->power_supply_charge_design;
      CHECK_INT(power_supply_charge_full,
            libhal_device_set_property_int (hal_ctx, udi, "battery.charge_level.last_full", 8*battery_info->power_supply_charge_full/battery_info->power_supply_charge_design, NULL));
    }
    CHECK_INT(power_supply_charge_full,
          libhal_device_set_property_int (hal_ctx, udi, "battery.reporting.last_full", battery_info->power_supply_charge_full, NULL));
  }

  charge_level_current = 8*(6.25+capacity)/100;
  if(global_bme.charge_level.current != charge_level_current)
  {
    global_bme.charge_level.current = charge_level_current;
    libhal_device_set_property_int (hal_ctx, udi, "battery.charge_level.current",charge_level_current, NULL);
    send_battery_state_changed(charge_level_current);
  }

  if (calibrated)
  {
    if (battery_info->power_supply_status == STATUS_CHARGING)
    {
      CHECK_INT(power_supply_time_to_full_now,
            libhal_device_set_property_int (hal_ctx, udi, "battery.remaining_time", battery_info->power_supply_time_to_full_now, NULL));
    }
    else if (battery_info->power_supply_status == STATUS_DISCHARGING)
    {
      CHECK_INT(power_supply_time_to_empty_avg,
            libhal_device_set_property_int (hal_ctx, udi, "battery.remaining_time", battery_info->power_supply_time_to_empty_avg, NULL));
    }
  }

  if (strstr(battery_info->power_supply_mode, "host"))
  {
    libhal_device_set_property_string(hal_ctx, udi, "maemo.charger.connection_status", "connected", NULL);
    libhal_device_set_property_string(hal_ctx, udi, "maemo.charger.type", "host 500 mA", NULL);
  }
  else if (strstr(battery_info->power_supply_mode, "dedicated"))
  {
    libhal_device_set_property_string(hal_ctx, udi, "maemo.charger.connection_status", "connected", NULL);
    libhal_device_set_property_string(hal_ctx, udi, "maemo.charger.type", "wall charger", NULL);
  }
  else
  {
    libhal_device_set_property_string(hal_ctx, udi, "maemo.charger.connection_status", "disconnected", NULL);
    libhal_device_set_property_string(hal_ctx, udi, "maemo.charger.type", "none", NULL);
  }

  if (global_charger_connected != charger_connected)
  {
    DSM_MSGTYPE_SET_CHARGER_STATE msg =
      DSME_MSG_INIT(DSM_MSGTYPE_SET_CHARGER_STATE);
    global_charger_connected = charger_connected;
    send_dbus_signal_(charger_connected ? "charger_connected" : "charger_disconnected");
    if (capacity_state != FULL)
      send_dbus_signal_(charger_connected ? "charger_charging_on" : "charger_charging_off");
    msg.connected = charger_connected;
    dsmesock_send(dsme_conn, &msg);
  }

  return TRUE;
}

static gboolean mce_pattern_request(const char * pattern, const char * request)
{
  DBusMessage * msg;
  int sent = 0;

  msg = dbus_message_new_method_call("com.nokia.mce", "/com/nokia/mce/request", "com.nokia.mce.request", request);
  if (!msg)
    return FALSE;

  if (dbus_message_append_args(msg, DBUS_TYPE_STRING, &pattern, DBUS_TYPE_INVALID) &&
      dbus_connection_send(system_dbus, msg, 0))
    sent = 1;

  dbus_connection_flush(system_dbus);
  dbus_message_unref(msg);

  if (!sent)
    return FALSE;

  log_print("%s: %s\n", request, pattern);
  return TRUE;
}

static const char * get_pattern_name(unsigned int pattern)
{
  switch(pattern)
  {
    case PATTERN_FULL: return "PatternBatteryFull";
    case PATTERN_CHARGING: return "PatternBatteryCharging";
    case PATTERN_BOOST: return "PatternBoost";
    default: return NULL;
  }
}

static gboolean mce_pattern_activate(unsigned int pattern)
{
  const char * name = get_pattern_name(pattern);
  if (!name)
    return FALSE;
  return mce_pattern_request(name, "req_led_pattern_activate");
}

static gboolean mce_pattern_deactivate(unsigned int pattern)
{
  const char * name = get_pattern_name(pattern);
  if (!name)
    return FALSE;
  return mce_pattern_request(name, "req_led_pattern_deactivate");
}

static gboolean poll_uevent(gpointer data)
{
  unsigned int pattern;
  battery battery_info;
  memset(&battery_info, 0, sizeof(battery_info));
  battery_info.power_supply_capacity = -1;
  battery_info.power_supply_flags_register = -1;
  strcpy(battery_info.power_supply_mode, global_battery.power_supply_mode);

  log_print("poll_uevent");

  hald_addon_bme_get_bq27200_data(&battery_info);
  hald_addon_bme_get_bq27200_registers(&battery_info);
  hald_addon_bme_get_rx51_data(&battery_info);
  hald_addon_bme_update_hal(&battery_info,TRUE);

  memcpy(&global_battery,&battery_info,sizeof(global_battery));

  if (global_bme.charge_level.capacity_state == FULL && global_charger_connected)
    pattern = PATTERN_FULL;
  else if (global_bme.charge_level.capacity_state != FULL && global_charger_connected)
    pattern = PATTERN_CHARGING;
  else if (strstr(global_battery.power_supply_mode, "boost"))
    pattern = PATTERN_BOOST;
  else
    pattern = PATTERN_NONE;

  if (global_pattern != pattern)
  {
    if (global_pattern != PATTERN_NONE && mce_pattern_deactivate(global_pattern))
      global_pattern = PATTERN_NONE;

    if (global_pattern == PATTERN_NONE && mce_pattern_activate(pattern))
      global_pattern = pattern;
  }

  if (data) return FALSE;

  return TRUE;
}

static gboolean hald_addon_bme_bq24150a_setup_poll(gpointer data);

static gboolean hald_addon_bme_bq24150a_cb(GIOChannel *source, GIOCondition condition, gpointer data G_GNUC_UNUSED)
{
  GIOStatus ret;
  gchar *line = NULL;
  gsize len;
  GError *gerror = NULL;

  log_print("hald_addon_bme_bq24150a_cb");

  if (condition & G_IO_IN || condition & G_IO_PRI)
  {
    ret = g_io_channel_read_line(source, &line, &len, NULL, &gerror);
    g_io_channel_seek_position(source, 0, G_SEEK_SET, &gerror);
    if (line)
    {
      strncpy(global_battery.power_supply_mode, line, sizeof(global_battery.power_supply_mode)-1);
      g_free(line);
      poll_uevent(NULL);
      return TRUE;
    }
    log_print("Error");
    g_io_channel_unref(source);
    g_timeout_add_seconds(60,hald_addon_bme_bq24150a_setup_poll,NULL);
    return FALSE;
  }
  else if (condition & G_IO_ERR || condition & G_IO_HUP || condition & G_IO_NVAL)
  {
    log_print("Error");
    g_io_channel_unref(source);
    g_timeout_add_seconds(60,hald_addon_bme_bq24150a_setup_poll,NULL);
    return FALSE;
  }
  else
  {
    log_print("unknown GIOCondition: %d", condition);
    g_io_channel_unref(source);
    g_timeout_add_seconds(60,hald_addon_bme_bq24150a_setup_poll,NULL);
    return FALSE;
  }

  return FALSE;
}

static int hald_addon_bme_disable_stat_pin(void)
{
  FILE * fp;
  int ret;
  if((fp = fopen(BQ24150A_STAT_PIN_FILE_PATH,"w")) == NULL)
  {
    log_print("unable to open %s(%s)\n",BQ24150A_STAT_PIN_FILE_PATH,strerror(errno));
    return -1;
  }
  log_print("disabling stat pin\n");
  ret = fputs("0", fp);
  fclose(fp);
  if (ret < 0)
    return -1;
  else
    return 0;
}

static gboolean hald_addon_bme_bq24150a_setup_poll(gpointer data G_GNUC_UNUSED)
{
  GIOChannel *gioch;
  GError *error = NULL;
  gsize len;
  gchar *line = NULL;
  GIOStatus ret;

  log_print("calling hald_addon_bme_bq24150a_setup_poll\n");

  gioch = g_io_channel_new_file(BQ24150A_MODE_FILE_PATH, "r", &error);
  if (gioch == NULL)
  {
    log_print("g_io_channel_new_file() for %s failed: %s", BQ24150A_MODE_FILE_PATH, error->message);
    g_error_free(error);
    g_timeout_add_seconds(60,hald_addon_bme_bq24150a_setup_poll,NULL);
    return FALSE;
  }

  ret = g_io_channel_read_line(gioch, &line, &len, NULL, &error);
  if (line) {
    strncpy(global_battery.power_supply_mode, line, sizeof(global_battery.power_supply_mode)-1);
    g_free(line);
  }

  /* Have to read the contents even though it's not used */
  ret = g_io_channel_read_to_end(gioch, &line, &len, &error);
  if (ret == G_IO_STATUS_NORMAL)
    g_free(line);

  if (error != NULL) {
    log_print("g_io_channel_read_to_end(): %s", error->message);
    g_error_free (error);
  }

  g_io_channel_seek_position(gioch, 0, G_SEEK_SET, &error);
  if (error != NULL){
    log_print("g_io_channel_seek_position(): %s", error->message);
    g_error_free (error);
  }

  if ( g_io_add_watch(gioch, G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_HUP | G_IO_NVAL, hald_addon_bme_bq24150a_cb, NULL) == 0 )
  {
    g_timeout_add_seconds(60,hald_addon_bme_bq24150a_setup_poll,NULL);
    return FALSE;
  }

  hald_addon_bme_disable_stat_pin();

  mce_pattern_deactivate(PATTERN_FULL);
  mce_pattern_deactivate(PATTERN_CHARGING);
  mce_pattern_deactivate(PATTERN_BOOST);

  if (global_pattern != PATTERN_NONE && mce_pattern_activate(global_pattern))
    global_pattern = PATTERN_NONE;

  return FALSE;
}

int main ()
{
  int result = 1;
  const char * bq27200_poll_period = getenv ("HAL_PROP_BQ27200_POLL_PERIOD_SECONDS");

  log_print (("STARTUP\n\n"));
  global_bme.charge_level.capacity_state = OK;

  if(bq27200_poll_period)
    poll_period =  atoi(bq27200_poll_period);
  if(!poll_period)
    poll_period = 30;

  if(!hald_addon_bme_setup_hal())
  {
    log_print("hal addon setup failed\n\n");
    goto out;
  }

  if(hald_addon_bme_setup_dbus_proxy() == -1)
  {
    log_print("dbus proxy setup failed\n\n");
    goto out;
  }

  if ( hald_addon_bme_server_dbus_init() == -1 )
  {
    log_print("server setup failed\n\n");
    goto out;
  }

  dsme_conn = dsmesock_connect();
  if (!dsme_conn)
  {
    log_print("dsmesock_connect failed\n\n");
    goto out;
  }

  hald_addon_bme_bq24150a_setup_poll(NULL);
  hald_addon_bme_update_hal(&global_battery,FALSE);

  mainloop = g_main_loop_new(0,FALSE);
  /* add poll callback */
  g_timeout_add_seconds(poll_period,poll_uevent,NULL);
  g_timeout_add_seconds(0,poll_uevent,(gpointer)1);

  log_print("ENTER MAIN LOOP\n\n");
  g_main_loop_run(mainloop);
  log_print("LEAVE MAIN LOOP\n\n");

  result = 0;

out:
  log_print("CLEANUP\n\n");
  cleanup_system_dbus();
  log_print("EXIT %d\n\n", result);

  return result;
}
