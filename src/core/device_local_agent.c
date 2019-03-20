/*
 *
 * Copyright (C) 2016-2019  ARRIS Enterprises, LLC
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF 
 * THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/**
 * \file device_local_agent.c
 *
 * Implements the Device.LocalAgent data model object
 *
 */

#include <stdio.h>
#include <time.h>
#include <string.h>

#include "common_defs.h"
#include "usp_api.h"
#include "dm_access.h"
#include "dm_trans.h"
#include "data_model.h"
#include "device.h"
#include "version.h"
#include "iso8601.h"
#include "stomp.h"
#include "vendor_api.h"
#include "nu_macaddr.h"
#include "nu_ipaddr.h"
#include "text_utils.h"
#include "uptime.h"



//------------------------------------------------------------------------------
// Cached version of the endpoint_id, which is populated at boot up by DEVICE_LOCAL_AGENT_SetDefaults()
static char agent_endpoint_id[MAX_DM_SHORT_VALUE_LEN] = {0};

//------------------------------------------------------------------------------
// By default when a stop of USP Agent is scheduled, it just exits rather than rebooting
exit_action_t scheduled_exit_action = kExitAction_Exit;

//------------------------------------------------------------------------------
// Database paths to parameters associated with rebooting and whether firmware has been activated
char *reboot_cause_path = "Internal.Reboot.Cause";
static char *reboot_command_key_path = "Internal.Reboot.CommandKey";
static char *reboot_request_instance_path = "Internal.Reboot.RequestInstance";
static char *last_software_version_path = "Internal.Reboot.LastSoftwareVersion";

static char *local_reboot_cause_str = "LocalReboot";

//------------------------------------------------------------------------------
// Database paths associated with device parameters
static char *manufacturer_oui_path = "Device.DeviceInfo.ManufacturerOUI";
static char *serial_number_path = "Device.DeviceInfo.SerialNumber";
static char *endpoint_id_path = "Device.LocalAgent.EndpointID";

//------------------------------------------------------------------------------
// Number of seconds after reboot at which USP Agent was started
static unsigned usp_agent_start_time;

//------------------------------------------------------------------------------
// Cause of last reboot, and other variables calculated at Boot-up time related to cause of reboot
static reboot_info_t reboot_info;

//------------------------------------------------------------------------------
// Variables relating to Dual Stack preference - whether to prefer IPv4 or IPv6 addresses, when both are available eg on an interface or DNS resolution
char *dual_stack_preference_path = "Internal.DualStackPreference";
static bool dual_stack_prefer_ipv6 = false;

//------------------------------------------------------------------------------
// Forward declarations. Note these are not static, because we need them in the symbol table for USP_LOG_Callstack() to show them
int Validate_DualStackPreference(dm_req_t *req, char *value);
int NotifyChange_DualStackPreference(dm_req_t *req, char *value);
int GetUpTime(dm_req_t *req, char *buf, int len);
int GetCurrentLocalTime(dm_req_t *req, char *buf, int len);
int ScheduleReboot(dm_req_t *req, char *command_key, kv_vector_t *input_args, kv_vector_t *output_args);
int ScheduleFactoryReset(dm_req_t *req, char *command_key, kv_vector_t *input_args, kv_vector_t *output_args);
int GetDefaultOUI(char *buf, int len);
int GetDefaultSerialNumber(char *buf, int len);
int GetDefaultEndpointID(char *buf, int len, char *oui, char *serial_number);
int PopulateRebootInfo(void);
int GetActiveSoftwareVersion(dm_req_t *req, char *buf, int len);
#ifndef REMOVE_DEVICE_INFO
int GetHardwareVersion(dm_req_t *req, char *buf, int len);
#endif

/*********************************************************************//**
**
** DEVICE_LOCAL_AGENT_Init
**
** Initialises this component, and registers all parameters which it implements
**
** \param   None
**
** \return  USP_ERR_OK if successful
**          USP_ERR_INTERNAL_ERROR if any other error occurred
**
**************************************************************************/
int DEVICE_LOCAL_AGENT_Init(void)
{
    int err = USP_ERR_OK;

    // Initialise last reboot cause structure
    memset(&reboot_info, 0, sizeof(reboot_info));

    // Register parameters implemented by this component
    // NOTE: Device.LocalAgent.EndpointID is registered in DEVICE_LOCAL_AGENT_RegisterEndpointID()
    err = USP_ERR_OK;
    err |= USP_REGISTER_VendorParam_ReadOnly("Device.LocalAgent.UpTime", GetUpTime, DM_UINT);

    // Determine which protocol is used
    #ifdef ENABLE_COAP
        #define SUPPORTED_PROTOCOLS      "STOMP, CoAP"
    #else
        #define SUPPORTED_PROTOCOLS      "STOMP"
    #endif

    err |= USP_REGISTER_Param_Constant("Device.LocalAgent.SupportedProtocols", SUPPORTED_PROTOCOLS, DM_STRING);
    err |= USP_REGISTER_Param_Constant("Device.LocalAgent.SoftwareVersion", AGENT_SOFTWARE_VERSION, DM_STRING);

    // Register Reset and Reboot operations
    err |= USP_REGISTER_SyncOperation("Device.Reboot()", ScheduleReboot);
    err |= USP_REGISTER_SyncOperation("Device.FactoryReset()", ScheduleFactoryReset);

    // Register parameters associated with tracking the cause of a reboot
    err |= USP_REGISTER_DBParam_ReadWrite(reboot_cause_path, local_reboot_cause_str, NULL, NULL, DM_STRING);
    err |= USP_REGISTER_DBParam_ReadWrite(reboot_command_key_path, "", NULL, NULL, DM_STRING);
    err |= USP_REGISTER_DBParam_ReadWrite(reboot_request_instance_path, "-1", NULL, NULL, DM_INT);
    err |= USP_REGISTER_DBParam_ReadWrite(last_software_version_path, "", NULL, NULL, DM_STRING);

#ifndef REMOVE_DEVICE_INFO
    err |= USP_REGISTER_VendorParam_ReadOnly("Device.DeviceInfo.SoftwareVersion", GetActiveSoftwareVersion, DM_STRING);
    err |= USP_REGISTER_Param_Constant("Device.DeviceInfo.ProductClass", VENDOR_PRODUCT_CLASS, DM_STRING);
    err |= USP_REGISTER_Param_Constant("Device.DeviceInfo.Manufacturer", VENDOR_MANUFACTURER, DM_STRING);
    err |= USP_REGISTER_Param_Constant("Device.DeviceInfo.ModelName", VENDOR_MODEL_NAME, DM_STRING);
    err |= USP_REGISTER_VendorParam_ReadOnly("Device.DeviceInfo.HardwareVersion", GetHardwareVersion, DM_STRING);

    // NOTE: The default values of these database parameters are setup later in DEVICE_LOCAL_AGENT_SetDefaults()
    err |= USP_REGISTER_DBParam_ReadOnly(manufacturer_oui_path, "", DM_STRING);
    err |= USP_REGISTER_DBParam_ReadOnly(serial_number_path, "", DM_STRING);
#endif

    // NOTE: The default value of this database parameter is setup later in DEVICE_LOCAL_AGENT_SetDefaults()
    err |= USP_REGISTER_DBParam_ReadOnly(endpoint_id_path, "", DM_STRING);


    err |= USP_REGISTER_VendorParam_ReadOnly("Device.Time.CurrentLocalTime", GetCurrentLocalTime, DM_DATETIME);
    err |= USP_REGISTER_DBParam_ReadWrite(dual_stack_preference_path, "IPv4", Validate_DualStackPreference, NotifyChange_DualStackPreference, DM_STRING);
    if (err != USP_ERR_OK)
    {
        return USP_ERR_INTERNAL_ERROR;
    }

    // If the code gets here, then registration was successful
    return USP_ERR_OK;
}

/*********************************************************************//**
**
** DEVICE_LOCAL_AGENT_SetDefaults
**
** Sets the default values for the database parameters: OUI, SerialNumber and EndpointID
** And caches the value of the retrieved EndpointID
** NOTE: This can only be performed after vendor hooks have been registered and after any factory reset (if required)
**
** \param   None
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int DEVICE_LOCAL_AGENT_SetDefaults(void)
{
    int err;
    char default_value[MAX_DM_SHORT_VALUE_LEN];
    char oui[MAX_DM_SHORT_VALUE_LEN];
    char serial_number[MAX_DM_SHORT_VALUE_LEN];

    //-------------------------------------------------------------
    // OUI
    // Exit if unable to get the default value of OUI (ie the value if not overridden by the USP DB)
    err = GetDefaultOUI(default_value, sizeof(default_value));
    if (err != USP_ERR_OK)
    {
        return err;
    }

#ifndef REMOVE_DEVICE_INFO
    // Register the default value of OUI (if DeviceInfo parameters are being registered by USP Agent core)
    err = DM_PRIV_ReRegister_DBParam_Default(manufacturer_oui_path, default_value);
    if (err != USP_ERR_OK)
    {
        return err;
    }
#endif

    // Get the actual value of OUI
    // This may be the value in the USP DB, the default value (if not present in DB) or a value retrieved by vendor hook (if REMOVE_DEVICE_INFO is defined)
    err = DATA_MODEL_GetParameterValue(manufacturer_oui_path, oui, sizeof(oui), 0);

#ifdef REMOVE_DEVICE_INFO
    // If vendor has not registered Device.DeviceInfo.OUI, then ignore the error, and use the default value
    if (err == USP_ERR_INVALID_PATH)
    {
        USP_STRNCPY(oui, default_value, sizeof(oui));
        err = USP_ERR_OK;
    }
#endif

    if (err != USP_ERR_OK)
    {
        return err;
    }

    //-------------------------------------------------------------
    // SERIAL NUMBER
    // Exit if unable to get the default value of Serial Number (ie the value if not overridden by the USP DB)
    err = GetDefaultSerialNumber(default_value, sizeof(default_value));
    if (err != USP_ERR_OK)
    {
        return err;
    }

#ifndef REMOVE_DEVICE_INFO
    // Register the default value of SerialNumber (if DeviceInfo parameters are being registered by USP Agent core)
    err = DM_PRIV_ReRegister_DBParam_Default(serial_number_path, default_value);
    if (err != USP_ERR_OK)
    {
        return err;
    }
#endif

    // Get the actual value of Serial Number
    // This may be the value in the USP DB, the default value (if not present in DB) or a value retrieved by vendor hook (if REMOVE_DEVICE_INFO is defined)
    err = DATA_MODEL_GetParameterValue(serial_number_path, serial_number, sizeof(serial_number), 0);

#ifdef REMOVE_DEVICE_INFO
    // If vendor has not registered Device.DeviceInfo.SerialNumber, then ignore the error, and use the default value
    if (err == USP_ERR_INVALID_PATH)
    {
        USP_STRNCPY(serial_number, default_value, sizeof(serial_number));
        err = USP_ERR_OK;
    }
#endif

    if (err != USP_ERR_OK)
    {
        return err;
    }

    //-------------------------------------------------------------
    // ENDPOINT_ID
    // Exit if unable to get the default value of EndpointID (ie the value if not overridden by the USP DB)
    err = GetDefaultEndpointID(default_value, sizeof(default_value), oui, serial_number);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    // Register the default value of EndpointID (if DeviceInfo parameters are being registered by USP Agent core)
    err = DM_PRIV_ReRegister_DBParam_Default(endpoint_id_path, default_value);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    // Get the actual value of EndpointID
    // This may be the value in the USP DB or the default value (if not present in DB)
    err = DATA_MODEL_GetParameterValue(endpoint_id_path, agent_endpoint_id, sizeof(agent_endpoint_id), 0);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** DEVICE_LOCAL_AGENT_Start
**
** Starts this component, adding all instances to the data model
**
** \param   None
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int DEVICE_LOCAL_AGENT_Start(void)
{
    int err;
    char value[MAX_DM_SHORT_VALUE_LEN];

    // Get the time (after boot) at which USP Agent was started 
    usp_agent_start_time = (unsigned)tu_uptime_secs(); 

    PopulateRebootInfo();

    // Exit if unable to get the Dual stack preference for IPv4 or IPv6
    err = DATA_MODEL_GetParameterValue(dual_stack_preference_path, value, sizeof(value), 0);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    // Cache the Dual stack preference in 'dual_stack_prefer_ipv6'
    NotifyChange_DualStackPreference(NULL, value);

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** DEVICE_LOCAL_AGENT_Stop
**
** Frees all memory used by this component
**
** \param   None
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
void DEVICE_LOCAL_AGENT_Stop(void)
{
    USP_SAFE_FREE(reboot_info.cause);
    USP_SAFE_FREE(reboot_info.command_key);
    USP_SAFE_FREE(reboot_info.cur_software_version);
    USP_SAFE_FREE(reboot_info.last_software_version);
}

/*********************************************************************//**
**
** DEVICE_LOCAL_AGENT_ScheduleReboot
**
** Schedules a reboot to occur once all connections have finished sending. 
**
** \param   exit_action - action to perform on exit
** \param   reboot_cause - cause of reboot
** \param   command_key - pointer to string containing the command key for this operation
** \param   request_instance - instance number of the request that initiated the reboot, or INVALID if reboot was not initiated by an operation
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int DEVICE_LOCAL_AGENT_ScheduleReboot(exit_action_t exit_action, char *reboot_cause, char *command_key, int request_instance)
{
    int err;

    // Exit if unable to persist the cause of reboot
    err = DATA_MODEL_SetParameterValue(reboot_cause_path, reboot_cause, 0);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    // Exit if unable to persist the command key, so that it can be returned in the Boot event
    err = DATA_MODEL_SetParameterValue(reboot_command_key_path, command_key, 0);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    // Exit if unable to persist the request instance of the operation which caused the reboot
    err = DM_ACCESS_SetInteger(reboot_request_instance_path, request_instance);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    scheduled_exit_action = exit_action;
    MTP_EXEC_ScheduleExit();
    return USP_ERR_OK;
}

/*********************************************************************//**
**
** DEVICE_LOCAL_AGENT_GetExitAction
**
** Returns what action to perform when gracefully exiting USP Agent
** This function is called during a scheduled exit, once all responses have been sent,
** to determine whether to just exit, or to reboot, or to factory reset
** NOTE: This function may be called from any thread
**
** \param   None
**
** \return  action to perform
**
**************************************************************************/
exit_action_t DEVICE_LOCAL_AGENT_GetExitAction(void)
{
    return scheduled_exit_action;
}

/*********************************************************************//**
**
** DEVICE_LOCAL_AGENT_GetEndpointID
**
** Returns the cached value of the EndpointID of this device
** NOTE: This function is threadsafe - the value is immutable
**
** \param   None
**
** \return  pointer to string containing EndpointID
**
**************************************************************************/
char *DEVICE_LOCAL_AGENT_GetEndpointID(void)
{
    return agent_endpoint_id;
}

/*********************************************************************//**
**
** DEVICE_LOCAL_AGENT_GetRebootInfo
**
** Gets the cause of the last reboot and associated data
**
** \param   reboot_info - pointer to structure in which to return the information
**
** \return  None
**
**************************************************************************/
void DEVICE_LOCAL_AGENT_GetRebootInfo(reboot_info_t *info)
{
    memcpy(info, &reboot_info, sizeof(reboot_info_t));
}

/*********************************************************************//**
**
** DEVICE_LOCAL_AGENT_GetDualStackPreference
**
** Gets the value of Device.DualStackPreference as a boolean
** NOTE: This function may be called from any thread
**
** \param   None
**
** \return  true if IPv6 is preferred over IPv4, if the WAN interface or DNS lookup supports both
**
**************************************************************************/
bool DEVICE_LOCAL_AGENT_GetDualStackPreference(void)
{
    return dual_stack_prefer_ipv6;
}


/*********************************************************************//**
**
** Validate_DualStackPreference
**
** Function called to validate Internal.DualStackPreference
**
** \param   req - pointer to structure identifying the path
** \param   value - new value of this parameter
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int Validate_DualStackPreference(dm_req_t *req, char *value)
{
    // Exit if new value is valid
    if ((strcmp(value, "IPv4")==0) || (strcmp(value, "IPv6")==0))
    {
        return USP_ERR_OK;
    }

    // Otherwise value is invalid
    USP_ERR_SetMessage("%s: Only allowed values are 'IPv4' or 'IPv6'", __FUNCTION__);
    return USP_ERR_INVALID_VALUE;
}

/*********************************************************************//**
**
** NotifyChange_DualStackPreference
**
** Function called after Internal.DualStackPreference is modified
**
** \param   req - pointer to structure identifying the path
** \param   value - new value of this parameter
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int NotifyChange_DualStackPreference(dm_req_t *req, char *value)
{
    // Set local cached copy of this value
    if (strcmp(value, "IPv6")==0)
    {
        // Prefer IPv6, if interface or DNS resolution has an IPv4 and IPv4 address
        dual_stack_prefer_ipv6 = true;
    }
    else
    {
        // Default to preferring IPv4
        dual_stack_prefer_ipv6 = false;
    }

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** GetUpTime
**
** Gets the number of seconds that the agent software has been running
**
** \param   req - pointer to structure identifying the parameter
** \param   buf - pointer to buffer into which to return the value of the parameter (as a textual string)
** \param   len - length of buffer in which to return the value of the parameter
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int GetUpTime(dm_req_t *req, char *buf, int len)
{
    val_uint = (unsigned)tu_uptime_secs() - usp_agent_start_time;

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** GetCurrentLocalTime
**
** Returns the current local time in ISO8601 format
**
** \param   req - pointer to structure identifying the parameter
** \param   buf - pointer to buffer into which to return the value of the parameter (as a textual string)
** \param   len - length of buffer in which to return the value of the parameter
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int GetCurrentLocalTime(dm_req_t *req, char *buf, int len)
{
    time_t t;
    struct tm tm;

    // Get current UTC time offset from unix epoch
    t = time(NULL);

    // Create split representation of time
    localtime_r(&t, &tm);

    // Finally create the current time string
    iso8601_strftime(buf, len, &tm);

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** ScheduleReboot
**
** Sync Operation handler for the Reboot operation
** The vendor reboot function will be called once all connections have finished sending. 
** eg after the response message for this operation has been sent
**
** \param   req - pointer to structure identifying the operation in the data model
** \param   command_key - pointer to string containing the command key for this operation
** \param   input_args - vector containing input arguments and their values (unused)
** \param   output_args - vector to return output arguments in
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int ScheduleReboot(dm_req_t *req, char *command_key, kv_vector_t *input_args, kv_vector_t *output_args)
{
    int err;

    // Ensure that no output arguments are returned for this sync operation
    KV_VECTOR_Init(output_args);

    err = DEVICE_LOCAL_AGENT_ScheduleReboot(kExitAction_Reboot, "RemoteReboot", command_key, INVALID);

    return err;
}

/*********************************************************************//**
**
** ScheduleFactoryReset
**
** Sync Operation handler for the FactoryReset
** The vendor reboot function will be called once all connections have finished sending. 
** eg after the response message for this operation has been sent
**
** \param   req - pointer to structure identifying the operation in the data model
** \param   command_key - pointer to string containing the command key for this operation
** \param   input_args - vector containing input arguments and their values (unused)
** \param   output_args - vector to return output arguments in
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int ScheduleFactoryReset(dm_req_t *req, char *command_key, kv_vector_t *input_args, kv_vector_t *output_args)
{
    int err;

    // Ensure that no output arguments are returned for this sync operation
    KV_VECTOR_Init(output_args);

    err = DEVICE_LOCAL_AGENT_ScheduleReboot(kExitAction_FactoryReset, "RemoteFactoryReset", command_key, INVALID);

    return err;
}

/*********************************************************************//**
**
** GetDefaultOUI
**
** Gets the default OUI for this CPE
** This is the value of OUI if it is not overriden by a value in the USP DB
**
** \param   buf - pointer to buffer in which to return the default value
** \param   len = length of buffer
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int GetDefaultOUI(char *buf, int len)
{
    char *p;

    // Exit if OUI set by environment variable
    p = getenv("USP_BOARD_OUI");
    if ((p != NULL) && (*p != '\0'))
    {
        USP_STRNCPY(buf, p, len);
        return USP_ERR_OK;
    }

    // Otherwise use compile time OUI
    USP_STRNCPY(buf, VENDOR_OUI, len);

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** GetDefaultSerialNumber
**
** Gets the default serial number for this CPE
** This is the value of serial number if it is not overriden by a value in the USP DB
**
** \param   buf - pointer to buffer in which to return the default value
** \param   len = length of buffer
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int GetDefaultSerialNumber(char *buf, int len)
{
    int err;
    dm_vendor_get_agent_serial_number_cb_t   get_agent_serial_number_cb;
    unsigned char mac_addr[MAC_ADDR_LEN];
    char *p;
    int i;
    int val;

    // Exit if serial number is determined by a vendor hook
    get_agent_serial_number_cb = vendor_hook_callbacks.get_agent_serial_number_cb;
    if (get_agent_serial_number_cb != NULL)
    {
        err = get_agent_serial_number_cb(buf, len);
        if (err != USP_ERR_OK)
        {
            USP_ERR_SetMessage("%s: get_agent_endpoint_id_cb() failed", __FUNCTION__);
            return USP_ERR_INTERNAL_ERROR;
        }

        return USP_ERR_OK;
    }

    // Exit if serial number set by environment variable
    p = getenv("USP_BOARD_SERIAL");
    if ((p != NULL) && (*p != '\0'))
    {
        USP_STRNCPY(buf, p, len);
        return USP_ERR_OK;
    }

    // Otherwise use serial number set by MAC address (default)
    // Exit if unable to get MAC address
    err = nu_macaddr_wan_macaddr(mac_addr);
    if (err != USP_ERR_OK)
    {
        return err;
    }
    
    // Convert MAC address into ASCII string form
    USP_ASSERT(len > 2*MAC_ADDR_LEN+1);
    p = buf;
    for (i=0; i<MAC_ADDR_LEN; i++)
    {
        val = mac_addr[i];
        *p++ = TEXT_UTILS_ValueToHexDigit( (val & 0xF0) >> 4 );
        *p++ = TEXT_UTILS_ValueToHexDigit( val & 0x0F );
    }
    *p = '\0';

    return USP_ERR_OK;
}


/*********************************************************************//**
**
** GetDefaultEndpointID
**
** Gets the default endpoint_id for this CPE
** This is the value of endpoint_id if it is not overriden by a value in the USP DB
**
** \param   endpoint_id - pointer to buffer in which to return the endpoint_id of this CPE
** \param   len - length of endpoint_id return buffer
** \param   oui - pointer to string containing oui of device
** \param   serial_number - pointer to string containing serial number of device
**
** \return  None
**
**************************************************************************/
int GetDefaultEndpointID(char *buf, int len, char *oui, char *serial_number)
{
    int err;
    dm_vendor_get_agent_endpoint_id_cb_t   get_agent_endpoint_id_cb;

    // Exit if endpoint_id is determined by a vendor hook
    get_agent_endpoint_id_cb = vendor_hook_callbacks.get_agent_endpoint_id_cb;
    if (get_agent_endpoint_id_cb != NULL)
    {
        err = get_agent_endpoint_id_cb(buf, len);
        if (err != USP_ERR_OK)
        {
            USP_ERR_SetMessage("%s: get_agent_endpoint_id_cb() failed", __FUNCTION__);
            return USP_ERR_INTERNAL_ERROR;
        }

        return USP_ERR_OK;
    }

    // Otherwise form the EndpointID from the retrieved OUI-SerialNumber
    USP_ASSERT(serial_number[0] != '\0');
    USP_SNPRINTF(buf, len, "os::%s-%s", oui, serial_number);

    return USP_ERR_OK;
}


/*********************************************************************//**
**
** PopulateRebootInfo
**
** Cache the cause (and command key) of the last reboot, then
** setup the default cause and command key for the next reboot. 
** This will be overridden if any other cause occurs
**
** \param   None
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int PopulateRebootInfo(void)
{
    int err;
    char last_value[MAX_DM_SHORT_VALUE_LEN];
    char cur_value[MAX_DM_SHORT_VALUE_LEN];
    char *last_version;

    // Set the default to indicate that the firmware image was not updated
    reboot_info.is_firmware_updated = false;

    //-------------------------------------------
    // Exit if unable to get the cause of the last reboot
    err = DATA_MODEL_GetParameterValue(reboot_cause_path, last_value, sizeof(last_value), 0);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    // Cache the cause of the last reboot
    reboot_info.cause = USP_STRDUP(last_value);

    // Set the default cause of the next reboot (if we need to because it's changed from the last)
    if (strcmp(last_value, local_reboot_cause_str) != 0)
    {
        // Exit if unable to set the default cause of reboot for next time
        err = DATA_MODEL_SetParameterValue(reboot_cause_path, local_reboot_cause_str, 0);
        if (err != USP_ERR_OK)
        {
            return err;
        }
    }

    //-------------------------------------------
    // Exit if unable to get the command_key for the last reboot
    err = DATA_MODEL_GetParameterValue(reboot_command_key_path, last_value, sizeof(last_value), 0);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    // Cache the command key associated with the last reboot
    reboot_info.command_key = USP_STRDUP(last_value);

    // Set the default command key associated with the next reboot (if we need to because it's changed from the last)
    if (last_value[0] != '\0')
    {
        // Exit if unable to set the default command_key for reboot for next time
        DATA_MODEL_SetParameterValue(reboot_command_key_path, "", 0);
        if (err != USP_ERR_OK)
        {
            return err;
        }
    }

    //-------------------------------------------
    // Exit if unable to determine whether the reboot was initiated by an operation
    err = DM_ACCESS_GetInteger(reboot_request_instance_path, &reboot_info.request_instance);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    // Set the default for whether the next reboot was initiated by an operation
    if (reboot_info.request_instance != INVALID)
    {
        // Exit if unable to set the default for next time
        DATA_MODEL_SetParameterValue(reboot_request_instance_path, "-1", 0);
        if (err != USP_ERR_OK)
        {
            return err;
        }
    }

    //-------------------------------------------
    // Exit if unable to get the software version that was used in the last boot cycle
    err = DATA_MODEL_GetParameterValue(last_software_version_path, last_value, sizeof(last_value), 0);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    // Get the software version used in this boot cycle
    err = DATA_MODEL_GetParameterValue("Device.DeviceInfo.SoftwareVersion", cur_value, sizeof(cur_value), 0);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    reboot_info.cur_software_version = USP_STRDUP(cur_value);

    // Save the last software version. Note that if this is from a factory reset, then use the current software version
    last_version = (last_value[0] == '\0') ? cur_value : last_value;
    reboot_info.last_software_version = USP_STRDUP(last_version);
    

    // Save the software version used in this boot cycle, so next boot cycle we can see if its changed
    err = DATA_MODEL_SetParameterValue(last_software_version_path, cur_value, 0);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    // If the software version used in the last boot cycle differs from the one used 
    // in this boot cycle, then the firmware has been updated, unless this was a factory reset
    if ((strcmp(last_value, cur_value) != 0) && (last_value[0] != '\0'))
    {
        reboot_info.is_firmware_updated = true;
    }

    return USP_ERR_OK;
}

#ifndef REMOVE_DEVICE_INFO
/*********************************************************************//**
**
** GetActiveSoftwareVersion
**
** Gets the current running software version
** This must match the software version of the active firmware image
** Wrapper function around VENDOR_GetActiveSoftwareVersion(), so that req does not have to be passed to it
**
** \param   req - pointer to structure containing path information
** \param   buf - pointer to buffer into which to return the value of the parameter (as a textual string)
** \param   len - length of buffer in which to return the value of the parameter
**
** \return  USP_ERR_OK if validated successfully
**
**************************************************************************/
int GetActiveSoftwareVersion(dm_req_t *req, char *buf, int len)
{
    int err;
    get_active_software_version_cb_t   get_active_software_version_cb;

    // Exit if unable to get the active software version from the vendor
    *buf = '\0';
    get_active_software_version_cb = vendor_hook_callbacks.get_active_software_version_cb;
    if (get_active_software_version_cb != NULL)
    {
        err = get_active_software_version_cb(buf, len);
        if (err != USP_ERR_OK)
        {
            USP_ERR_SetMessage("%s: get_active_software_version_cb() failed", __FUNCTION__);
            return err;
        }
    }

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** GetHardwareVersion
**
** Gets the hardware version of the board on which this software is running
**
** \param   req - pointer to structure containing path information
** \param   buf - pointer to buffer into which to return the value of the parameter (as a textual string)
** \param   len - length of buffer in which to return the value of the parameter
**
** \return  USP_ERR_OK if validated successfully
**
**************************************************************************/
int GetHardwareVersion(dm_req_t *req, char *buf, int len)
{
    int err;
    get_hardware_version_cb_t   get_hardware_version_cb;

    // Exit if unable to get the hardware version from the vendor
    *buf = '\0';
    get_hardware_version_cb = vendor_hook_callbacks.get_hardware_version_cb;
    if (get_hardware_version_cb != NULL)
    {
        err = get_hardware_version_cb(buf, len);
        if (err != USP_ERR_OK)
        {
            USP_ERR_SetMessage("%s: get_hardware_version_cb() failed", __FUNCTION__);
            return err;
        }
    }

    return USP_ERR_OK;
}
#endif // REMOVE_DEVICE_INFO