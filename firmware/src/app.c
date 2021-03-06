/*******************************************************************************
* Copyright (C) 2019 Microchip Technology Inc. and its subsidiaries.
*
* Subject to your compliance with these terms, you may use Microchip software
* and any derivatives exclusively with Microchip products. It is your
* responsibility to comply with third party license terms applicable to your
* use of third party software (including open source software) that may
* accompany Microchip software.
*
* THIS SOFTWARE IS SUPPLIED BY MICROCHIP "AS IS". NO WARRANTIES, WHETHER
* EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS SOFTWARE, INCLUDING ANY IMPLIED
* WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY, AND FITNESS FOR A
* PARTICULAR PURPOSE.
*
* IN NO EVENT WILL MICROCHIP BE LIABLE FOR ANY INDIRECT, SPECIAL, PUNITIVE,
* INCIDENTAL OR CONSEQUENTIAL LOSS, DAMAGE, COST OR EXPENSE OF ANY KIND
* WHATSOEVER RELATED TO THE SOFTWARE, HOWEVER CAUSED, EVEN IF MICROCHIP HAS
* BEEN ADVISED OF THE POSSIBILITY OR THE DAMAGES ARE FORESEEABLE. TO THE
* FULLEST EXTENT ALLOWED BY LAW, MICROCHIP'S TOTAL LIABILITY ON ALL CLAIMS IN
* ANY WAY RELATED TO THIS SOFTWARE WILL NOT EXCEED THE AMOUNT OF FEES, IF ANY,
* THAT YOU HAVE PAID DIRECTLY TO MICROCHIP FOR THIS SOFTWARE.
*******************************************************************************/

/*******************************************************************************
  MPLAB Harmony Application Source File

  Company:
    Microchip Technology Inc.

  File Name:
    app.c

  Summary:
    This file contains the source code for the MPLAB Harmony application.

  Description:
    This file contains the source code for the MPLAB Harmony application.  It
    implements the logic of the application's state machine and it may call
    API routines of other MPLAB Harmony modules in the system, such as drivers,
    system services, and middleware.  However, it does not call any of the
    system interfaces (such as the "Initialize" and "Tasks" functions) of any of
    the modules in the system or make any assumptions about when those functions
    are called.  That is the responsibility of the configuration-specific system
    files.
 *******************************************************************************/

// *****************************************************************************
// *****************************************************************************
// Section: Included Files
// *****************************************************************************
// *****************************************************************************
#include <time.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "wdrv_winc_client_api.h"
#include "iot_config/IoT_Sensor_Node_config.h"
#include "services/iot/cloud/crypto_client/cryptoauthlib_main.h"
#include "services/iot/cloud/crypto_client/crypto_client.h"
#include "services/iot/cloud/cloud_service.h"
#include "services/iot/cloud/wifi_service.h"
#include "services/iot/cloud/bsd_adapter/bsdWINC.h"
#include "credentials_storage/credentials_storage.h"
#include "debug_print.h"
#include "led.h"
#include "mqtt/mqtt_core/mqtt_core.h"
#include "services/iot/cloud/mqtt_packetPopulation/mqtt_packetPopulate.h"
#include "services/iot/cloud/mqtt_packetPopulation/mqtt_iothub_packetPopulate.h"
#include "services/iot/cloud/mqtt_packetPopulation/mqtt_iotprovisioning_packetPopulate.h"
#include "azure/core/az_span.h"
#include "azure/core/az_json.h"
#include "azure/iot/az_iot_hub_client.h"

#if CFG_ENABLE_CLI
#include "system/command/sys_command.h"
#endif

#define DEFAULT_START_TEMP_CELSIUS 22

// *****************************************************************************
// *****************************************************************************
// Section: Local Function Prototypes
// *****************************************************************************
// *****************************************************************************
static void APP_SendToCloud(void);
static float APP_GetTempSensorValue(void);
static void APP_DataTask(void);
static void APP_WiFiConnectionStateChanged(uint8_t status);
static void APP_ProvisionRespCb(DRV_HANDLE handle, WDRV_WINC_SSID * targetSSID, WDRV_WINC_AUTH_CONTEXT * authCtx, bool status);
static void APP_DHCPAddressEventCb(DRV_HANDLE handle, uint32_t ipAddress);
static void APP_GetTimeNotifyCb(DRV_HANDLE handle, uint32_t timeUTC);
static void APP_ConnectNotifyCb(DRV_HANDLE handle, WDRV_WINC_CONN_STATE currentState, WDRV_WINC_CONN_ERROR errorCode);

// *****************************************************************************
// *****************************************************************************
// Section: Application Macros
// *****************************************************************************
// *****************************************************************************
#define APP_WIFI_SOFT_AP  0
#define APP_WIFI_DEFAULT  1
#define APP_DATATASK_INTERVAL 1000L //100msec
#define APP_SW_DEBOUNCE_INTERVAL   1460000L

/* WIFI SSID, AUTH and PWD for AP */
#define APP_CFG_MAIN_WLAN_SSID  "MCHP.IOT"
#define APP_CFG_MAIN_WLAN_AUTH  M2M_WIFI_SEC_WPA_PSK
#define APP_CFG_MAIN_WLAN_PSK   "microchip"

#define CFG_APP_WINC_DEBUG       0 //define this to print WINC debug messages

// *****************************************************************************
// *****************************************************************************
// Section: Global Data Definitions
// *****************************************************************************
// *****************************************************************************
#define SN_STRING "sn"

/* Place holder for ECC608A unique serial id */
ATCA_STATUS appCryptoClientSerialNumber;
ATCA_STATUS retValCryptoClientSerialNumber;
char* attDeviceID;
char attDeviceID_buf[25] = "BAAAAADD1DBAAADD1D";

shared_networking_params_t shared_networking_params;

/* Various NTP Host servers that application relies upon for time sync */
#define WORLDWIDE_NTP_POOL_HOSTNAME     "*.pool.ntp.org"
#define ASIA_NTP_POOL_HOSTNAME          "asia.pool.ntp.org"
#define EUROPE_NTP_POOL_HOSTNAME        "europe.pool.ntp.org"
#define NAMERICA_NTP_POOL_HOSTNAME      "north-america.pool.ntp.org"
#define OCEANIA_NTP_POOL_HOSTNAME       "oceania.pool.ntp.org"
#define SAMERICA_NTP_POOL_HOSTNAME      "south-america.pool.ntp.org"
#define NTP_HOSTNAME                    "pool.ntp.org"

/* Driver handle for WINC1510 */
static DRV_HANDLE wdrvHandle;
uint8_t mode = WIFI_DEFAULT;

SYS_TIME_HANDLE App_DataTaskHandle = SYS_TIME_HANDLE_INVALID;
volatile bool App_DataTaskTmrExpired = false;
SYS_TIME_HANDLE App_CloudTaskHandle = SYS_TIME_HANDLE_INVALID;  
volatile bool App_CloudTaskTmrExpired = false;

extern az_iot_hub_client hub_client;
extern pf_MQTT_CLIENT pf_mqqt_iotprovisioning_client;
extern pf_MQTT_CLIENT pf_mqqt_iothub_client;

// *****************************************************************************
/* Application Data

  Summary:
    Holds application data

  Description:
    This structure holds the application's data.

  Remarks:
    This structure should be initialized by the APP_Initialize function.

    Application strings and buffers are be defined outside this structure.
*/
// * PnP Values *
// The model id is the JSON document (also called the Digital Twins Model Identifier or DTMI)
// which defines the capability of your device. The functionality of the device should match what
// is described in the corresponding DTMI. Should you choose to program your own PnP capable device,
// the functionality would need to match the DTMI and you would need to update the below 'model_id'.
// Please see the sample README for more information on this DTMI.
const az_span device_model_id = AZ_SPAN_LITERAL_FROM_STR("dtmi:com:example:Thermostat;1");
// ISO8601 Time Format
static const char iso_spec_time_format[] = "%Y-%m-%dT%H:%M:%S.000%zZ";

// IoT Hub Connection Values
static int32_t request_id_int;
static char request_id_buf[8];

// IoT Hub Telemetry Values
char telemetry_topic[128];
static const az_span telemetry_name = AZ_SPAN_LITERAL_FROM_STR("temperature");
static char telemetry_payload[256];

// IoT Hub Commands Values
static char commands_response_topic[128];
static const az_span report_command_name_span = AZ_SPAN_LITERAL_FROM_STR("getMaxMinReport");
static const az_span report_max_temp_name_span = AZ_SPAN_LITERAL_FROM_STR("maxTemp");
static const az_span report_min_temp_name_span = AZ_SPAN_LITERAL_FROM_STR("minTemp");
static const az_span report_avg_temp_name_span = AZ_SPAN_LITERAL_FROM_STR("avgTemp");
static const az_span report_start_time_name_span = AZ_SPAN_LITERAL_FROM_STR("startTime");
static const az_span report_end_time_name_span = AZ_SPAN_LITERAL_FROM_STR("endTime");
static const az_span report_error_payload = AZ_SPAN_LITERAL_FROM_STR("{}");
static char end_time_buffer[32];
static char commands_response_payload[256];
static char incoming_since_value[32];

// IoT Hub Twin Values
//static char twin_get_topic[128];
static char reported_property_topic[128];
static const az_span desired_property_name = AZ_SPAN_LITERAL_FROM_STR("desired");
static const az_span desired_property_version_name = AZ_SPAN_LITERAL_FROM_STR("$version");
static const az_span desired_temp_property_name = AZ_SPAN_LITERAL_FROM_STR("targetTemperature");
static const az_span desired_temp_response_value_name = AZ_SPAN_LITERAL_FROM_STR("value");
static const az_span desired_temp_ack_code_name = AZ_SPAN_LITERAL_FROM_STR("ac");
static const az_span desired_temp_ack_version_name = AZ_SPAN_LITERAL_FROM_STR("av");
static const az_span desired_temp_ack_description_name = AZ_SPAN_LITERAL_FROM_STR("ad");
static const az_span max_temp_reported_property_name
    = AZ_SPAN_LITERAL_FROM_STR("maxTempSinceLastReboot");
static char reported_property_payload[128];

// PnP Device Values
static bool max_temp_changed = false;
static int32_t current_device_temp;
static int32_t device_temperature_avg_total;
static uint32_t device_temperature_avg_count = 0;
static int32_t device_max_temp;
static int32_t device_min_temp;
static int32_t device_avg_temp;

#define RETURN_IF_AZ_RESULT_FAILED(exp) \
  do                                    \
  {                                     \
    az_result const _az_result = (exp); \
    if (az_result_failed(_az_result))   \
    {                                   \
      return _az_result;                \
    }                                   \
  } while (0)

APP_DATA appData;

// *****************************************************************************
// *****************************************************************************
// Section: Application Callback Functions
// *****************************************************************************
// *****************************************************************************
void APP_CloudTaskcb(uintptr_t context)
{
    App_CloudTaskTmrExpired = true;
}
void APP_DataTaskcb(uintptr_t context)
{
    App_DataTaskTmrExpired = true;
}
// *****************************************************************************
// *****************************************************************************
// Section: Application Local Functions
// *****************************************************************************
// *****************************************************************************

// React to the WIFI state change here. Status of 1 means connected, Status of 0 means disconnected
static void  APP_WiFiConnectionStateChanged(uint8_t status)
{
   // If we have no AP access we want to retry
   if (status != 1)
   {
      // Restart the WIFI module if we get disconnected from the WiFi Access Point (AP)
      CLOUD_reset();
   } 
}


// *****************************************************************************
// *****************************************************************************
// Section: Application Initialization and State Machine Functions
// *****************************************************************************
// *****************************************************************************


/*******************************************************************************
  Function:
    void APP_Initialize ( void )

  Remarks:
    See prototype in app.h.
 */
void APP_Initialize(void)
{
    /* Place the App state machine in its initial state. */
    appData.state = APP_STATE_CRYPTO_INIT;
    //uint8_t mode = WIFI_DEFAULT;
	uint32_t sw0CurrentVal = 0;
	uint32_t sw1CurrentVal = 0;
    uint32_t i = 0;
    
    LED_test();
    // Blocking debounce
    for(i = 0; i < APP_SW_DEBOUNCE_INTERVAL; i++)
    {
        sw0CurrentVal += SW0_GPIO_PA00_Get() ;
        sw1CurrentVal += SW1_GPIO_PA01_Get() ;
    }
    if(sw0CurrentVal < (APP_SW_DEBOUNCE_INTERVAL/2))
    {
        if(sw1CurrentVal < (APP_SW_DEBOUNCE_INTERVAL/2))
        {
            strcpy(ssid, APP_CFG_MAIN_WLAN_SSID);
            strcpy(pass, APP_CFG_MAIN_WLAN_PSK);
            sprintf((char*)authType, "%d", APP_CFG_MAIN_WLAN_AUTH);
            LED_startBlinkingGreen();
        }
        else
        {
            mode = WIFI_SOFT_AP;
        }
    }
    /* Open I2C driver client */
    ADC_Enable();
    LED_test();
#if (CFG_APP_WINC_DEBUG == 1)    
    WDRV_WINC_DebugRegisterCallback(debug_printf);
#endif
}

static void APP_ConnectNotifyCb(DRV_HANDLE handle, WDRV_WINC_CONN_STATE currentState, WDRV_WINC_CONN_ERROR errorCode)
{
    if (WDRV_WINC_CONN_STATE_CONNECTED == currentState)
    {
        WiFi_ConStateCb(M2M_WIFI_CONNECTED);
    }
    else if(WDRV_WINC_CONN_STATE_DISCONNECTED == currentState)
    {
        WiFi_ConStateCb(M2M_WIFI_DISCONNECTED);
    }
}

static void APP_GetTimeNotifyCb(DRV_HANDLE handle, uint32_t timeUTC)
{
    //checking > 0 is not recommended, even if getsystime returns null, utctime value will be > 0
    if (timeUTC != 0x86615400U)
    {
        tstrSystemTime pSysTime;
        struct tm theTime;

        WDRV_WINC_UTCToLocalTime(timeUTC, &pSysTime);
        theTime.tm_hour = pSysTime.u8Hour;
        theTime.tm_min = pSysTime.u8Minute;
        theTime.tm_sec = pSysTime.u8Second;
        theTime.tm_year = pSysTime.u16Year-1900;
        theTime.tm_mon = pSysTime.u8Month-1;
        theTime.tm_mday = pSysTime.u8Day;
        theTime.tm_isdst = 0;      
        RTC_RTCCTimeSet(&theTime);
    }
}

static void APP_DHCPAddressEventCb(DRV_HANDLE handle, uint32_t ipAddress)
{
    WiFi_HostLookupCb();    
}
             
static void APP_ProvisionRespCb(DRV_HANDLE handle, WDRV_WINC_SSID * targetSSID, 
                                WDRV_WINC_AUTH_CONTEXT * authCtx, bool status)
{
    uint8_t sectype;
	uint8_t* ssid;
	uint8_t* password; 
    
    if (status == M2M_SUCCESS)
    {
        sectype = authCtx->authType;
        ssid = targetSSID->name;
        password = authCtx->authInfo.WPAPerPSK.key;
        WiFi_ProvisionCb(sectype, ssid, password);
    }    
}

#ifdef CFG_MQTT_PROVISIONING_HOST
void iot_provisioning_completed(void)
{
    debug_printGOOD("Azure IoT Provisioning Completed.");
    CLOUD_init_host(hub_hostname, attDeviceID, &pf_mqqt_iothub_client);
    CLOUD_disconnect();
    CLOUD_reset();
    App_DataTaskHandle = SYS_TIME_CallbackRegisterMS(APP_DataTaskcb, 0, APP_DATATASK_INTERVAL, SYS_TIME_PERIODIC);
}
#endif //CFG_MQTT_PROVISIONING_HOST 

void APP_Tasks(void)
{
    switch(appData.state)
    {
        case APP_STATE_CRYPTO_INIT:
        {
            debug_init(attDeviceID);
            cryptoauthlib_init(); 
            if (cryptoDeviceInitialized == false)
            {
               debug_printError("APP: CryptoAuthInit failed");
            }
            
#ifdef HUB_DEVICE_ID
            attDeviceID = HUB_DEVICE_ID;
#else 
            char serialNumber_buf[25];
            appCryptoClientSerialNumber = CRYPTO_CLIENT_printSerialNumber(serialNumber_buf);
            if(appCryptoClientSerialNumber != ATCA_SUCCESS )
            {
               switch(appCryptoClientSerialNumber)
               {
                  case ATCA_GEN_FAIL:
                        debug_printError("APP: DeviceID generation failed, unspecified error");
                  break;
                  case ATCA_BAD_PARAM:
                        debug_printError("APP: DeviceID generation failed, bad argument");
                  default:
                        debug_printError("APP: DeviceID generation failed");
                  break;
               }
            }
            else
            {
                // To use Azure provisioning service, attDeviceID should match with the device cert CN,
                // which is the serial number of ECC608 prefixed with "sn" if you are using the 
                // the microchip provisioning tool for PIC24.
                strcpy(attDeviceID_buf, SN_STRING);
                strcat(attDeviceID_buf, serialNumber_buf);
                attDeviceID = attDeviceID_buf;
                debug_printInfo("CRYPTO_CLIENT_printSerialNumber %s\n", attDeviceID);
            }
#endif
            
#if CFG_ENABLE_CLI   
            set_deviceId(attDeviceID);
#endif             
            debug_setPrefix(attDeviceID);
            CLOUD_setdeviceId(attDeviceID);
            appData.state = APP_STATE_WDRV_INIT;
            break;
        }
        case APP_STATE_WDRV_INIT:
        {
            if (SYS_STATUS_READY == WDRV_WINC_Status(sysObj.drvWifiWinc)) {
                appData.state = APP_STATE_WDRV_INIT_READY;
            }
            break;
        }
        case APP_STATE_WDRV_INIT_READY:
        {
            wdrvHandle = WDRV_WINC_Open(0, 0);

            if (DRV_HANDLE_INVALID != wdrvHandle) {
                appData.state = APP_STATE_WDRV_OPEN;
            }
            
            break;
        }

        case APP_STATE_WDRV_OPEN:
        {
            m2m_wifi_configure_sntp((uint8_t *)NTP_HOSTNAME, strlen(NTP_HOSTNAME), SNTP_ENABLE_DHCP);
            m2m_wifi_enable_sntp(1); 
            WDRV_WINC_DCPT *pDcpt = (WDRV_WINC_DCPT *)wdrvHandle;
            pDcpt->pfProvConnectInfoCB = APP_ProvisionRespCb;
            wifi_init(APP_WiFiConnectionStateChanged, mode);

            if (mode == WIFI_DEFAULT) {
                
                /* Enable use of DHCP for network configuration, DHCP is the default
                but this also registers the callback for notifications. */
                WDRV_WINC_IPUseDHCPSet(wdrvHandle, &APP_DHCPAddressEventCb);
            
                App_CloudTaskHandle = SYS_TIME_CallbackRegisterMS(APP_CloudTaskcb, 0, 500, SYS_TIME_PERIODIC);
                WDRV_WINC_BSSReconnect(wdrvHandle, &APP_ConnectNotifyCb); 
                WDRV_WINC_SystemTimeGetCurrent(wdrvHandle, &APP_GetTimeNotifyCb);
            }

#ifdef CFG_MQTT_PROVISIONING_HOST
            pf_mqqt_iotprovisioning_client.MQTT_CLIENT_task_completed = iot_provisioning_completed;
            CLOUD_init_host(CFG_MQTT_PROVISIONING_HOST, attDeviceID, &pf_mqqt_iotprovisioning_client);
#else
            CLOUD_init_host(hub_hostname, attDeviceID, &pf_mqqt_iothub_client);
#endif //CFG_MQTT_PROVISIONING_HOST 
    
            appData.state = APP_STATE_WDRV_ACTIV;
            break;
        }

        case APP_STATE_WDRV_ACTIV:
        {
            if(App_CloudTaskTmrExpired == true) {
                App_CloudTaskTmrExpired = false;
                CLOUD_task();
            }
            if(App_DataTaskTmrExpired == true) {
                App_DataTaskTmrExpired = false;
                APP_DataTask();
            }
            CLOUD_sched();
            wifi_sched();
            MQTT_sched();
            LED_sched();
            break;
        }    
        default:
        {
            /* TODO: Handle error in application's state machine. */
            break;
        }
    }
}

// This gets called by the scheduler approximately every 100ms
static void APP_DataTask(void)
{
   static time_t previousTransmissionTime = 0;
   
   // Get the current time. This uses the C standard library time functions
   time_t timeNow;// = time(NULL);

   struct tm sys_time;
   RTC_RTCCTimeGet(&sys_time);
   timeNow = mktime(&sys_time);

   // Example of how to send data when MQTT is connected every 1 second based on the system clock
    if (CLOUD_isConnected()) {
        // How many seconds since the last time this loop ran?
        int32_t delta = difftime(timeNow,previousTransmissionTime);

        if (delta >= CFG_SEND_INTERVAL) {
           previousTransmissionTime = timeNow;

           // Call the data task in main.c
           APP_SendToCloud();
        }
    } 
    if (!shared_networking_params.haveAPConnection) {
        LED_BLUE_SetHigh();
    } else {
        LED_BLUE_SetLow();
    }
    if (!shared_networking_params.haveERROR) {
        LED_RED_SetHigh();
    } else {
        LED_RED_SetLow();
    }
    if (LED_isBlinkingGreen() == false) {
        if (!CLOUD_isConnected()) {
            LED_GREEN_SetHigh();
        } else {
            LED_GREEN_SetLow();
        }
    }
}

// Create request id span which increments source int each call. Capable of holding 8 digit number.
static az_span get_request_id(void)
{
  az_span remainder;
  az_span out_span = az_span_create((uint8_t*)request_id_buf, sizeof(request_id_buf));
  az_result result = az_span_i32toa(out_span, request_id_int++, &remainder);
  (void)remainder;
  (void)result;
  return out_span;
}

static int mqtt_publish_message(char* topic, az_span payload, int qos)
{
    mqttPublishPacket cloudPublishPacket;
    // Fixed header
    cloudPublishPacket.publishHeaderFlags.duplicate = 0;
    cloudPublishPacket.publishHeaderFlags.qos = qos;
    cloudPublishPacket.publishHeaderFlags.retain = 0;
    // Variable header
    cloudPublishPacket.topic = (uint8_t*)topic;

    // Payload
    cloudPublishPacket.payload = az_span_ptr(payload);
    cloudPublishPacket.payloadLength = az_span_size(payload);

    if (MQTT_CreatePublishPacket(&cloudPublishPacket) != true)
    {
      debug_printError("MQTT: Connection lost PUBLISH failed");
    }

    return 0;
}

// Send the response of the command invocation
static int send_command_response(
    az_iot_hub_client_method_request* request,
    uint16_t status,
    az_span response)
{
    int rc;
    // Get the response topic to publish the command response
    if (az_result_failed(
            rc = az_iot_hub_client_methods_response_get_publish_topic(
                &hub_client,
                request->request_id,
                status,
                commands_response_topic,
                sizeof(commands_response_topic),
                NULL)))
    {
      debug_printError("Unable to get command response publish topic");
      return rc;
    }

    debug_printInfo("Command Status: %u", status);

    // Send the commands response
    if ((rc = mqtt_publish_message(commands_response_topic, response, 0)) == 0)
    {
      debug_printInfo("Sent command response");
    }

    return rc;
}

static az_result build_command_response_payload(
    az_json_writer* json_builder,
    az_span start_time_span,
    az_span end_time_span,
    az_span* response_payload)
{
  // Build the command response payload
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_append_begin_object(json_builder));
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_append_property_name(json_builder, report_max_temp_name_span));
  RETURN_IF_AZ_RESULT_FAILED(
      az_json_writer_append_int32(json_builder, device_max_temp));
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_append_property_name(json_builder, report_min_temp_name_span));
  RETURN_IF_AZ_RESULT_FAILED(
      az_json_writer_append_int32(json_builder, device_min_temp));
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_append_property_name(json_builder, report_avg_temp_name_span));
  RETURN_IF_AZ_RESULT_FAILED(
      az_json_writer_append_int32(json_builder, device_avg_temp));
  RETURN_IF_AZ_RESULT_FAILED(
      az_json_writer_append_property_name(json_builder, report_start_time_name_span));
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_append_string(json_builder, start_time_span));
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_append_property_name(json_builder, report_end_time_name_span));
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_append_string(json_builder, end_time_span));
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_append_end_object(json_builder));

  *response_payload = az_json_writer_get_bytes_used_in_destination(json_builder);

  return AZ_OK;
}

static az_result invoke_getMaxMinReport(az_span payload, az_span response, az_span* out_response)
{
    if (az_span_size(payload) == 0)
    {
        return AZ_ERROR_ITEM_NOT_FOUND;
    }
    
  // Parse the "since" field in the payload.
  az_span start_time_span = AZ_SPAN_EMPTY;
  az_json_reader jp;
  RETURN_IF_AZ_RESULT_FAILED(az_json_reader_init(&jp, payload, NULL)); 
  RETURN_IF_AZ_RESULT_FAILED(az_json_reader_next_token(&jp));
  int32_t incoming_since_value_len;
  RETURN_IF_AZ_RESULT_FAILED(az_json_token_get_string(
      &jp.token, incoming_since_value, sizeof(incoming_since_value), &incoming_since_value_len));
  start_time_span = az_span_create((uint8_t*)incoming_since_value, incoming_since_value_len);

  // Set the response payload to error if the "since" field was not sent
  if (az_span_ptr(start_time_span) == NULL)
  {
    response = report_error_payload;
    return AZ_ERROR_ITEM_NOT_FOUND;
  }

  // Get the current time as a string
  time_t rawtime;
  struct tm* timeinfo;
  time(&rawtime);
  timeinfo = localtime(&rawtime);
  size_t len = strftime(end_time_buffer, sizeof(end_time_buffer), iso_spec_time_format, timeinfo);
  az_span end_time_span = az_span_create((uint8_t*)end_time_buffer, (int32_t)len);

  az_json_writer json_builder;
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_init(&json_builder, response, NULL));
  RETURN_IF_AZ_RESULT_FAILED(
      build_command_response_payload(&json_builder, start_time_span, end_time_span, out_response));

  return AZ_OK;
}

// Invoke the requested command if supported and return status | Return error otherwise
static void handle_command_message(
    az_span payload,
    az_iot_hub_client_method_request* command_request)
{

  if (az_span_is_content_equal(report_command_name_span, command_request->name))
  {
    az_span command_response_span = AZ_SPAN_FROM_BUFFER(commands_response_payload);

    // Invoke command
    uint16_t return_code;
    az_result response = invoke_getMaxMinReport(
        payload, command_response_span, &command_response_span);
    if (response != AZ_OK)
    {
      return_code = 400;
    }
    else
    {
      return_code = 200;
    }

    // Send command response with report as JSON payload
    int rc;
    if ((rc = send_command_response(command_request, return_code, command_response_span)) != 0)
    {
      debug_printError("Unable to send %u response, status %d", return_code, rc);
    }
  }
  else
  {
    // Unsupported command
    debug_printError(
        "Unsupported command received: %.*s.",
        az_span_size(command_request->name),
        az_span_ptr(command_request->name));

    int rc;
    if ((rc = send_command_response(command_request, 404, report_error_payload)) != 0)
    {
      debug_printError("Unable to send %d response, status %d", 404, rc);
    }
  }
}

void APP_ReceivedFromCloud_methods(uint8_t* topic, uint8_t* payload)
{
    debug_printInfo("Methods");
	az_iot_hub_client_method_request method_request;
	az_result result = az_iot_hub_client_methods_parse_received_topic(&hub_client, az_span_create_from_str((char*)topic), &method_request);
	if (az_result_failed(result))
	{
		debug_printError("az_iot_hub_client_methods_parse_received_topic failed");
		return;
	}

  handle_command_message(az_span_create_from_str((char*)payload), &method_request);
}

// Parse the desired temperature property from the incoming JSON
static az_result parse_twin_desired_temperature_property(
    az_span twin_payload_span,
    bool is_twin_get,
    int32_t* parsed_value,
    int32_t* version_number)
{
  az_json_reader jp;
  bool desired_found = false;
  RETURN_IF_AZ_RESULT_FAILED(az_json_reader_init(&jp, twin_payload_span, NULL));
  RETURN_IF_AZ_RESULT_FAILED(az_json_reader_next_token(&jp));
  if (jp.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
  {
    return AZ_ERROR_UNEXPECTED_CHAR;
  }

  if (is_twin_get)
  {
    // If is twin get payload, we have to parse one level deeper for "desired" wrapper
    RETURN_IF_AZ_RESULT_FAILED(az_json_reader_next_token(&jp));
    while (jp.token.kind != AZ_JSON_TOKEN_END_OBJECT)
    {
      if (az_json_token_is_text_equal(&jp.token, desired_property_name))
      {
        desired_found = true;
        RETURN_IF_AZ_RESULT_FAILED(az_json_reader_next_token(&jp));
        break;
      }
      else
      {
        // else ignore token.
        RETURN_IF_AZ_RESULT_FAILED(az_json_reader_skip_children(&jp));
      }

      RETURN_IF_AZ_RESULT_FAILED(az_json_reader_next_token(&jp));
    }
  }
  else
  {
    desired_found = true;
  }

  if (!desired_found)
  {
    debug_printError("Desired property object not found in twin");
    return AZ_ERROR_ITEM_NOT_FOUND;
  }

  if (jp.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
  {
    return AZ_ERROR_UNEXPECTED_CHAR;
  }
  RETURN_IF_AZ_RESULT_FAILED(az_json_reader_next_token(&jp));

  bool temp_found = false;
  bool version_found = false;
  while (!(temp_found && version_found) || (jp.token.kind != AZ_JSON_TOKEN_END_OBJECT))
  {
    if (az_json_token_is_text_equal(&jp.token, desired_temp_property_name))
    {
      RETURN_IF_AZ_RESULT_FAILED(az_json_reader_next_token(&jp));
      RETURN_IF_AZ_RESULT_FAILED(az_json_token_get_int32(&jp.token, parsed_value));
      temp_found = true;
    }
    else if (az_json_token_is_text_equal(&jp.token, desired_property_version_name))
    {
      RETURN_IF_AZ_RESULT_FAILED(az_json_reader_next_token(&jp));
      RETURN_IF_AZ_RESULT_FAILED(az_json_token_get_int32(&jp.token, version_number));
      version_found = true;
    }
    else
    {
      // else ignore token.
      RETURN_IF_AZ_RESULT_FAILED(az_json_reader_skip_children(&jp));
    }
    RETURN_IF_AZ_RESULT_FAILED(az_json_reader_next_token(&jp));
  }

  if (temp_found && version_found)
  {
    debug_printInfo("Desired temperature: %d\tVersion number: %d", (int)*parsed_value, (int)*version_number);
    return AZ_OK;
  }

  return AZ_ERROR_ITEM_NOT_FOUND;
}

// Build the JSON payload for the reported property
static az_result build_confirmed_reported_property(
    az_json_writer* json_builder,
    az_span property_name,
    int32_t property_val,
    int32_t ack_code_value,
    int32_t ack_version_value,
    az_span ack_description_value)
{
  RETURN_IF_AZ_RESULT_FAILED(
      az_json_writer_init(json_builder, AZ_SPAN_FROM_BUFFER(reported_property_payload), NULL));
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_append_begin_object(json_builder));
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_append_property_name(json_builder, property_name));
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_append_begin_object(json_builder));
  RETURN_IF_AZ_RESULT_FAILED(
      az_json_writer_append_property_name(json_builder, desired_temp_response_value_name));
  RETURN_IF_AZ_RESULT_FAILED(
      az_json_writer_append_int32(json_builder, property_val));
  RETURN_IF_AZ_RESULT_FAILED(
      az_json_writer_append_property_name(json_builder, desired_temp_ack_code_name));
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_append_int32(json_builder, ack_code_value));
  RETURN_IF_AZ_RESULT_FAILED(
      az_json_writer_append_property_name(json_builder, desired_temp_ack_version_name));
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_append_int32(json_builder, ack_version_value));
  RETURN_IF_AZ_RESULT_FAILED(
      az_json_writer_append_property_name(json_builder, desired_temp_ack_description_name));
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_append_string(json_builder, ack_description_value));
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_append_end_object(json_builder));
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_append_end_object(json_builder));

  return AZ_OK;
}

static az_result build_reported_property(
    az_json_writer* json_builder,
    az_span property_name,
    int32_t property_val)
{
  RETURN_IF_AZ_RESULT_FAILED(
      az_json_writer_init(json_builder, AZ_SPAN_FROM_BUFFER(reported_property_payload), NULL));
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_append_begin_object(json_builder));
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_append_property_name(json_builder, property_name));
  RETURN_IF_AZ_RESULT_FAILED(
      az_json_writer_append_int32(json_builder, property_val));
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_append_end_object(json_builder));

  return AZ_OK;
}

static int send_reported_temperature_property(
    int32_t temp_value,
    int32_t version,
    bool is_max_reported_prop)
{
  int rc;
  debug_printInfo("Sending twin reported property");

  // Get the topic used to send a reported property update
  az_span request_id_span = get_request_id();
  if (az_result_failed(
          rc = az_iot_hub_client_twin_patch_get_publish_topic(
              &hub_client,
              request_id_span,
              reported_property_topic,
              sizeof(reported_property_topic),
              NULL)))
  {
    debug_printError("Unable to get twin document publish topic, return code %d", rc);
    return rc;
  }

  // Twin reported properties must be in JSON format. The payload is constructed here.
  az_json_writer json_builder;
  if (is_max_reported_prop)
  {
    if (az_result_failed(
            rc
            = build_reported_property(&json_builder, max_temp_reported_property_name, temp_value)))
    {
      return rc;
    }
  }
  else
  {
    if (az_result_failed(
            rc = build_confirmed_reported_property(
                &json_builder,
                desired_temp_property_name,
                temp_value,
                200,
                version,
                AZ_SPAN_FROM_STR("success"))))
    {
      return rc;
    }
  }
  az_span json_payload = az_json_writer_get_bytes_used_in_destination(&json_builder);

  // Publish the reported property payload to IoT Hub
  rc = mqtt_publish_message(reported_property_topic, json_payload, 0);

  max_temp_changed = false;

  return rc;
}

// Switch on the type of twin message and handle accordingly | On desired prop, respond with max
// temp reported prop.
static void handle_twin_message(
    az_span payload,
    az_iot_hub_client_twin_response* twin_response)
{
  az_result result;

  int32_t desired_temp;
  int32_t version_num;
  // Determine what type of incoming twin message this is. Print relevant data for the message.
  switch (twin_response->response_type)
  {
    // A response from a twin GET publish message with the twin document as a payload.
    case AZ_IOT_HUB_CLIENT_TWIN_RESPONSE_TYPE_GET:
      debug_printInfo("A twin GET response was received");
      if (az_result_failed(
              result = parse_twin_desired_temperature_property(
                  payload, true, &desired_temp, &version_num)))
      {
        // If the item can't be found, the desired temp might not be set so take no action
        break;
      }
      else
      {
        send_reported_temperature_property(desired_temp, version_num, false);
      }
      break;
    // An update to the desired properties with the properties as a JSON payload.
    case AZ_IOT_HUB_CLIENT_TWIN_RESPONSE_TYPE_DESIRED_PROPERTIES:
      debug_printInfo("A twin desired properties message was received");

      // Get the new temperature
      if (az_result_failed(
              result = parse_twin_desired_temperature_property(
                  payload, false, &desired_temp, &version_num)))
      {
        debug_printError("Could not parse desired temperature property, az_result %04x", result);
        break;
      }
      send_reported_temperature_property(desired_temp, version_num, false);

      break;

    // A response from a twin reported properties publish message. With a successful update of
    // the reported properties, the payload will be empty and the status will be 204.
    case AZ_IOT_HUB_CLIENT_TWIN_RESPONSE_TYPE_REPORTED_PROPERTIES:
      debug_printInfo("A twin reported properties response message was received");
      break;
  }
}

void APP_ReceivedFromCloud_twin(uint8_t* topic, uint8_t* payload)
{
    debug_printInfo("Device Twin Get");
	if (topic == NULL)
	{
        debug_printError("NULL topic");
		return;
	}

	az_iot_hub_client_twin_response twin_response;
	az_span twin_topic = az_span_create_from_str((char*)topic);
	az_result result = az_iot_hub_client_twin_parse_received_topic(&hub_client, twin_topic, &twin_response);
	if (az_result_failed(result))
	{
		debug_printError("az_iot_hub_client_twin_parse_received_topic failed");
		return;
	}

	if (az_span_size(twin_response.request_id) != 0 && IOT_DEBUG_PRINT)
	{
		char request_id_buf[50];
		az_span_to_str(request_id_buf, sizeof(request_id_buf), twin_response.request_id);
		debug_printInfo("Twin request, request_id:%s, status: %d", request_id_buf, twin_response.status);
	}

	if (payload == NULL)
	{
        debug_printError("NULL payload");
		return; // no payload, nothing to process
	}

  handle_twin_message(az_span_create_from_str((char*)payload), &twin_response);
}

void APP_ReceivedFromCloud_patch(uint8_t* topic, uint8_t* payload)
{
    debug_printInfo("Device Twin Patch");
	if (topic == NULL)
	{
        debug_printError("NULL topic");
		return;
	}

	az_iot_hub_client_twin_response twin_response;
	az_span twin_topic = az_span_create_from_str((char*)topic);
	az_result result = az_iot_hub_client_twin_parse_received_topic(&hub_client, twin_topic, &twin_response);
	if (az_result_failed(result))
	{
		debug_printError("az_iot_hub_client_twin_parse_received_topic failed");
		return;
    }

    if (payload)
    {
        debug_printGOOD("Payload: %s", payload);
    }
    else
    {
        debug_printError("NULL payload");
    }

    handle_twin_message(az_span_create_from_str((char*)payload), &twin_response);
}

static float APP_GetTempSensorValue(void)
{
    float retVal = 0;
    /* TA: AMBIENT TEMPERATURE REGISTER ADDRESS: 0x5 */
    uint8_t registerAddr = 0x5;
    /* Temp sensor read buffer */
    uint8_t rxBuffer[2];
    
    while (SERCOM3_I2C_IsBusy() == true);
    if (SERCOM3_I2C_WriteRead(0x18, (uint8_t *)&registerAddr, 1, (uint8_t *)rxBuffer, 2) == true) {        
        /* Wait for the I2C transfer to complete */
        while (SERCOM3_I2C_IsBusy() == true);

        /* Transfer complete. Check if the transfer was successful */
        if (SERCOM3_I2C_ErrorGet() == SERCOM_I2C_ERROR_NONE) {
            rxBuffer[0] = rxBuffer[0] & 0x1F; //Clear flag bits
            if ((rxBuffer[0] & 0x10) == 0x10) { 
                rxBuffer[0] = rxBuffer[0] & 0x0F; //Clear SIGN
                retVal = 256.0 - (rxBuffer[0] * 16.0 + rxBuffer[1] / 16.0);
            } else {
                retVal = ((rxBuffer[0] * 16.0) + (rxBuffer[1] / 16.0));
            }
        }                   
    }            
    return retVal;
}        

static void update_device_temp(void)
{
    int16_t temp = APP_GetTempSensorValue();
    current_device_temp = (int)(temp / 100);

    bool ret = false;
    if (current_device_temp > device_max_temp)
    {
      device_max_temp = current_device_temp;
      ret = true;
    }
    if (current_device_temp < device_min_temp)
    {
      device_min_temp = current_device_temp;
    }

    // Increment the avg count, add the new temp to the total, and calculate the new avg
    device_temperature_avg_count++;
    device_temperature_avg_total += current_device_temp;
    device_avg_temp = device_temperature_avg_total / device_temperature_avg_count;

    max_temp_changed = ret;
}

static az_result build_telemetry_message(az_span* out_payload)
{
  az_json_writer json_builder;
  RETURN_IF_AZ_RESULT_FAILED(
      az_json_writer_init(&json_builder, AZ_SPAN_FROM_BUFFER(telemetry_payload), NULL));
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_append_begin_object(&json_builder));
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_append_property_name(&json_builder, telemetry_name));
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_append_int32(
      &json_builder, current_device_temp));
  RETURN_IF_AZ_RESULT_FAILED(az_json_writer_append_end_object(&json_builder));
  *out_payload = az_json_writer_get_bytes_used_in_destination(&json_builder);

  return AZ_OK;
}

static int send_telemetry_message(void)
{
    int rc;

    if (az_result_failed(
            rc = az_iot_hub_client_telemetry_get_publish_topic(
                &hub_client, NULL, telemetry_topic, sizeof(telemetry_topic), NULL)))
    {
      return rc;
    }

    update_device_temp();

    az_span telemetry_payload_span;
    if (az_result_failed(rc = build_telemetry_message(&telemetry_payload_span)))
    {
      debug_printError("Could not build telemetry payload, az_result %d", rc);
      return rc;
    }

    debug_printInfo("Sending Telemetry Message: temp %d", (int)current_device_temp);
    rc = mqtt_publish_message(telemetry_topic, telemetry_payload_span, 0);

    return rc;
}

// This will get called every 1 second only while we have a valid Cloud connection
void APP_SendToCloud(void)
{
     send_telemetry_message();
}

void APP_application_post_provisioning(void)
{
    App_CloudTaskHandle = SYS_TIME_CallbackRegisterMS(APP_CloudTaskcb, 0, 500,
                                                        SYS_TIME_PERIODIC);
    App_DataTaskHandle = SYS_TIME_CallbackRegisterMS(APP_DataTaskcb, 0, 
                                APP_DATATASK_INTERVAL, SYS_TIME_PERIODIC);
}

// This must exist to keep the linker happy but is never called.
int _gettimeofday( struct timeval *tv, void *tzvp )
{
    struct tm sys_time;
    RTC_RTCCTimeGet(&sys_time);
    time_t currentTime = mktime(&sys_time);
    tv->tv_sec = currentTime;
    tv->tv_usec = 0;
    
    return 0;  // return non-zero for error
} // end _gettimeofday()


/*******************************************************************************
 End of File
 */
