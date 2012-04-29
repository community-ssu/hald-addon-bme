/*
 * hald-addon-bme.c: bme addon for maemo http://wiki.maemo.org/Mer
 *
 * Copyright (C) 2012 Ivaylo Dimitrov <freemangordon@abv.bg>
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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <glib/gmain.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <hal/libhal.h>
#include <gio/gio.h>
#include <ctype.h>
#include <memory.h>
#include "hald-addon-bme.h"


typedef struct {
/*  power_supply_name=bq27200-0
  power_supply_type=battery*/
  enum{CHARGING,DISCARGING}power_supply_status;
  uint32 power_supply_present;
  uint32 power_supply_voltage_now;
  uint32 power_supply_current_now;
  uint32 power_supply_capacity;
  uint32 power_supply_temp;
  uint32 power_supply_time_to_empty_now;
  uint32 power_supply_time_to_empty_avg;
  char  power_supply_technology[32];
  uint32 power_supply_charge_full;
  uint32 power_supply_charge_now;
  uint32 power_supply_charge_full_design;
} bq27200;

bq27200 global_battery={0,};

#define BATTERY_CHARGE_LEVEL_DESIGN 8
#define POWER_SUPPLY_CAPACITY_THRESHOLD_FULL 95
#define POWER_SUPPLY_CAPACITY_THRESHOLD_LOW 25
#define POWER_SUPPLY_CAPACITY_THRESHOLD_EMPTY 2

typedef struct {
  struct {
    enum {EMPTY,LOW,OK,FULL,UNKNOWN}capacity_state;
    uint32 current;
    uint32 design;
    uint32 percentage;
  }charge_level;
}bme;

bme global_bme={{0,}};

#define DEBUG
#define DEBUG_FILE      "/tmp/hald-addon-bme.log"

#define UEVENT_FILE_PATH "/sys/class/power_supply/bq27200-0/uevent"
/*
capacity            device              technology          type
charge_full         power               temp                uevent
charge_full_design  present             time_to_empty_avg   voltage_now
charge_now          status              time_to_empty_now
current_now         subsystem           time_to_full_now
  */
/*
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
guint poll_period = 6;

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

static gboolean seng_capacity_state_change()
{
  const char * name ;
  switch(global_bme.charge_level.capacity_state)
  {
    case LOW:name = "battery_low";break;
    case FULL:name = "battery_full";break;
    case EMPTY:name = "battery_empty";break;
    default:return TRUE;
  }
  return send_dbus_signal_(name);
}

static gboolean send_battery_state_changed(uint32 now,uint32 max)
{
  log_print("send dbus signal: %s (bars=%d/%d)\n", "battery_state_changed",now,max);
  return send_dbus_signal("battery_state_changed",DBUS_TYPE_UINT32, &now, DBUS_TYPE_UINT32, &max, DBUS_TYPE_INVALID);
}

static gboolean hald_addon_bme_get_uevent_data(bq27200 * battery_info)
{
  FILE * fp;

  char line[256];
  if((fp = fopen(UEVENT_FILE_PATH,"r")) == NULL)
  {
    log_print("unable to open %s(%s)\n",UEVENT_FILE_PATH,strerror(errno));
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
        battery_info->power_supply_status  = (!strcmp(tmp,"Discharging"))?DISCARGING:CHARGING;
      else if(!strcmp(line,"POWER_SUPPLY_PRESENT"))
        battery_info->power_supply_present = atoi(tmp);
      else if(!strcmp(line,"POWER_SUPPLY_VOLTAGE_NOW"))
        battery_info->power_supply_voltage_now = atoi(tmp)/1000;
      else if(!strcmp(line,"POWER_SUPPLY_CURRENT_NOW"))
        battery_info->power_supply_current_now = atoi(tmp);
      else if(!strcmp(line,"POWER_SUPPLY_TEMP"))
        battery_info->power_supply_temp = atoi(tmp);
      else if(!strcmp(line,"POWER_SUPPLY_TIME_TO_EMPTY_NOW"))
        battery_info->power_supply_time_to_empty_now = atoi(tmp);
      else if(!strcmp(line,"POWER_SUPPLY_TIME_TO_EMPTY_AVG"))
        battery_info->power_supply_time_to_empty_avg = atoi(tmp);
      else if(!strcmp(line,"POWER_SUPPLY_TECHNOLOGY"))
        strncpy(battery_info->power_supply_technology,
                tmp,
                sizeof(battery_info->power_supply_technology)-1);
      else if(!strcmp(line,"POWER_SUPPLY_CHARGE_FULL"))
        battery_info->power_supply_charge_full = atoi(tmp)/1000;
      else if(!strcmp(line,"POWER_SUPPLY_CHARGE_NOW"))
        battery_info->power_supply_charge_now = atoi(tmp)/1000;
      else if(!strcmp(line,"POWER_SUPPLY_CHARGE_FULL_DESIGN"))
        battery_info->power_supply_charge_full_design = atoi(tmp)/1000;
    }
  }
  fclose(fp);

  return TRUE;
}

static gboolean hald_addon_bme_status_info()
{
  /*TODO*/
  log_print("%s\n",__func__);
  return TRUE;
}

static gboolean hald_addon_bme_timeleft_info()
{
  /* TODO */
  log_print("%s\n",__func__);
  return TRUE;
}

static DBusHandlerResult hald_addon_bme_dbus_proxy(DBusConnection *connection, DBusMessage *message, void *user_data)
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

    if(hald_addon_bme_timeleft_info())
      log_print("unable to send timeleft info req to BME\n");
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


static DBusHandlerResult hald_addon_bme_mce_signal(DBusConnection *connection, DBusMessage *message, void *user_data)
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

static gboolean hald_addon_bme_update_hal(bq27200* battery_info,gboolean check_for_changes)
{
#define CHECK_INT(f,fun) if( !check_for_changes || (global_battery.f != battery_info->f)) \
  log_print(#f " changed,updating to %d",battery_info->f);fun

  DBusError error;
  LibHalChangeSet *cs;
  uint32 charge_level_current;
  uint32 capacity_state;

  cs = libhal_device_new_changeset (udi);
  if (cs == NULL)
  {
          log_print("Cannot initialize changeset");
          return FALSE;
  }
  if(!check_for_changes)
  {
    libhal_changeset_set_property_string(cs, "battery.charge_level.capacity_state", "unknown");
    libhal_changeset_set_property_int(cs, "battery.charge_level.design", BATTERY_CHARGE_LEVEL_DESIGN);
    libhal_changeset_set_property_string(cs, "battery.charge_level.unit","bars");
    libhal_changeset_set_property_bool(cs, "battery.is_rechargeable",TRUE);
    libhal_changeset_set_property_bool(cs, "battery.remaining_time.calculate_per_time", FALSE);
    libhal_changeset_set_property_string(cs, "battery.reporting.unit", "mAh");
    libhal_changeset_set_property_string(cs, "battery.type","pda");
    libhal_changeset_set_property_int(cs, "battery.voltage.design",4200);
    libhal_changeset_set_property_string(cs, "battery.voltage.unit","mV");
  }
  CHECK_INT(power_supply_present,
        libhal_changeset_set_property_bool (cs, "battery.present", battery_info->power_supply_present));
  if(!battery_info->power_supply_present)
  {
      global_bme.charge_level.capacity_state = UNKNOWN;
      libhal_changeset_set_property_string(cs, "battery.charge_level.capacity_state", "unknown");
      goto out;
  }

  CHECK_INT(power_supply_voltage_now,
        libhal_changeset_set_property_int (cs, "battery.voltage.current", battery_info->power_supply_voltage_now));

  CHECK_INT(power_supply_charge_now,
        libhal_changeset_set_property_int (cs, "battery.reporting.current", battery_info->power_supply_charge_now));

  CHECK_INT(power_supply_charge_full_design,
        libhal_changeset_set_property_int (cs, "battery.reporting.design", battery_info->power_supply_charge_full_design));


  CHECK_INT(power_supply_status,
        libhal_changeset_set_property_bool(cs, "battery.rechargeable.is_charging", battery_info->power_supply_status == CHARGING);
        libhal_changeset_set_property_bool(cs, "battery.rechargeable.is_discharging", battery_info->power_supply_status == DISCARGING));

  charge_level_current = battery_info->power_supply_charge_now/(battery_info->power_supply_charge_full_design/BATTERY_CHARGE_LEVEL_DESIGN);
  if(global_bme.charge_level.current != charge_level_current)
  {
    global_bme.charge_level.current = charge_level_current;
    libhal_changeset_set_property_int (cs, "battery.charge_level.current",charge_level_current);
    send_battery_state_changed(charge_level_current,BATTERY_CHARGE_LEVEL_DESIGN);
  }
  if(!battery_info->power_supply_capacity)
  {
    /* Battery is not calibrated, calculate the percentage */
    battery_info->power_supply_capacity = (100 * battery_info->power_supply_charge_now) / battery_info->power_supply_charge_full_design;
  }

  CHECK_INT(power_supply_capacity,
        libhal_changeset_set_property_int (cs, "battery.charge_level.percentage", battery_info->power_supply_capacity));

  if(battery_info->power_supply_capacity > POWER_SUPPLY_CAPACITY_THRESHOLD_FULL)
    capacity_state = FULL;
  else if(battery_info->power_supply_capacity < POWER_SUPPLY_CAPACITY_THRESHOLD_LOW)
  {
    if(battery_info->power_supply_capacity < POWER_SUPPLY_CAPACITY_THRESHOLD_EMPTY)
      capacity_state = EMPTY;
    else
      capacity_state = LOW;
  }
  else
    capacity_state = OK;

  if(global_bme.charge_level.capacity_state != capacity_state)
  {
    global_bme.charge_level.capacity_state = capacity_state;
    seng_capacity_state_change();
  }

/* battery.charge_level.capacity_state = 'ok'  (string)
   battery.charge_level.last_full = 0  (0x0)  (int)
    = 53  (0x35)  (int)
   battery.remaining_time = 7200  (0x1c20)  (int)
   battery.remaining_time.calculate_per_time = false  (bool)
   battery.reporting.last_full = 0  (0x0)  (int)
*/
out:
  dbus_error_init (&error);

  libhal_device_commit_changeset (hal_ctx, cs, &error);
  libhal_device_free_changeset (cs);
  if (dbus_error_is_set (&error)) {
          print_dbus_error("Failed to set property", &error);
          dbus_error_free (&error);
          return FALSE;
  }
  return TRUE;
}

static gboolean poll_uevent(gpointer data)
{
  bq27200 battery_info={0,};
  hald_addon_bme_get_uevent_data(&battery_info);
  hald_addon_bme_update_hal(&battery_info,TRUE);

  memcpy(&global_battery,&battery_info,sizeof(global_battery));

  return TRUE;
}

int main (int argc, char **argv)
{
  int result = 1;
  const char * bq27200_poll_period = getenv ("HAL_PROP_BQ27200_POLL_PERIOD_SECONDS");

  log_print (("STARTUP\n\n"));
  global_bme.charge_level.capacity_state = UNKNOWN;

  if(bq27200_poll_period)
    poll_period =  atoi(bq27200_poll_period);
  if(!poll_period)
    poll_period = 1;

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

  hald_addon_bme_update_hal(&global_battery,FALSE);

  mainloop = g_main_loop_new(0,FALSE);
  /* add poll callback */
  g_timeout_add_seconds(poll_period,poll_uevent,0);

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
