/*
    BitzOS (BOS) V0.1.6 - Copyright (C) 2017-2019 Hexabitz
    All rights reserved

    File Name     : H08R6.c
    Description   : Source code for module P08R6 / H08R6.
                    IR Time-if-Flight (ToF) Sensor (ST VL53L0CX)

    Required MCU resources :

      >> USARTs 1,2,3,4,5,6 for module ports (H08R6).
			>> USARTs 1,2,3,5,6 for module ports (P08R6).
      >> I2C2 for the ToF sensor.
      >> GPIOB 2 for ToF interrupt (INT).
      >> GPIOB 12 for ToF shutdown (XSHUT) in (H08R6) and GPIOB 0 in (P08R6).

*/

/* Includes ------------------------------------------------------------------*/
#include "BOS.h"
#include <stdlib.h>

/* Define UART variables */
UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;
UART_HandleTypeDef huart3;
#ifdef H08R6
	UART_HandleTypeDef huart4;
#endif
UART_HandleTypeDef huart5;
UART_HandleTypeDef huart6;

VL53L0X_Dev_t vl53l0x_HandleDevice;

/* Unit of measurement ranging
 * 0 = mm
 * 1 = cm
 * 2 = inch
 */
uint8_t h08r6UnitMeasurement = UNIT_MEASUREMENT_MM;
EventGroupHandle_t handleNewReadyData = NULL;
uint8_t startMeasurementRanging = STOP_MEASUREMENT_RANGING;

/* Module exported parameters ------------------------------------------------*/
float h08r6_range = 0.0f;
module_param_t modParam[NUM_MODULE_PARAMS] = {{.paramPtr=&h08r6_range, .paramFormat=FMT_FLOAT, .paramName="range"}};


/* Private variables ---------------------------------------------------------*/
int32_t offsetCalibration = 0;
uint8_t h08r6StatesVl53l0x = VL53L0x_STATE_FREE;
float h08r6MinRange = 0.0;
float h08r6MaxRange = 8000.0;
float h08r6BufStreamMem = 0;
TaskHandle_t ToFHandle = NULL;
uint32_t tofPeriod, tofTimeout, t0; 
uint8_t tofPort, tofModule, tofMode, tofState;
float *tofBuffer;
TimerHandle_t xTimerTof = NULL;

/* Private function prototypes -----------------------------------------------*/
static void Vl53l0xInit(void);
static VL53L0X_Error SetMeasurementMode(uint8_t mode, uint32_t period, uint32_t timeout);
static float GetMeasurementResult(void);
static float ConvertCurrentUnit(float distance);
static void SendMeasurementResult(uint8_t request, float distance, uint8_t module, uint8_t port, float *buffer);
static void CheckForEnterKey(void);
void ToFTask(void * argument);
void Stream_ToF(uint32_t period, uint32_t timeout);

/* Create CLI commands --------------------------------------------------------*/
static portBASE_TYPE demoCommand( int8_t *pcWriteBuffer, size_t xWriteBufferLen, const int8_t *pcCommandString );
static portBASE_TYPE Vl53l0xSampleCommand( int8_t *pcWriteBuffer, size_t xWriteBufferLen, const int8_t *pcCommandString );
static portBASE_TYPE Vl53l0xStreamCommand( int8_t *pcWriteBuffer, size_t xWriteBufferLen, const int8_t *pcCommandString );
static portBASE_TYPE Vl53l0xStopCommand( int8_t *pcWriteBuffer, size_t xWriteBufferLen, const int8_t *pcCommandString );
static portBASE_TYPE Vl53l0xUnitsCommand( int8_t *pcWriteBuffer, size_t xWriteBufferLen, const int8_t *pcCommandString );
static portBASE_TYPE Vl53l0xMaxCommand( int8_t *pcWriteBuffer, size_t xWriteBufferLen, const int8_t *pcCommandString );
static portBASE_TYPE rangeModParamCommand( int8_t *pcWriteBuffer, size_t xWriteBufferLen, const int8_t *pcCommandString );

/* CLI command structure : demo */
const CLI_Command_Definition_t demoCommandDefinition =
{
	( const int8_t * ) "demo", /* The command string to type. */
	( const int8_t * ) "demo:\r\n Run a demo program to test module functionality\r\n\r\n",
	demoCommand, /* The function to run. */
	0 /* No parameters are expected. */
};
/*-----------------------------------------------------------*/
/* CLI command structure : sample */
const CLI_Command_Definition_t Vl53l0xSampleCommandDefinition =
{
  ( const int8_t * ) "sample", /* The command string to type. */
  ( const int8_t * ) "sample:\r\nTake one sample measurement\r\n\r\n",
  Vl53l0xSampleCommand, /* The function to run. */
  0 /* No parameters are expected. */
};
/*-----------------------------------------------------------*/
/* CLI command structure : stream */
const CLI_Command_Definition_t Vl53l0xStreamCommandDefinition =
{
  ( const int8_t * ) "stream", /* The command string to type. */
		( const int8_t * ) "stream:\r\nStream measurements to the CLI with this syntax:\n\r\tstream period(in ms) timeout(in ms)\n\r\tstream period timeout -v\t(for verbose output)\
\n\rOr to a specific port in a specific module with this syntax:\r\n\tstream period timeout port(p1..px) module\n\rOr to internal buffer with this syntax:\r\n\tstream period timeout \
buffer.\n\rBuffer here is a literal value and can be accessed in the CLI using module parameter: range\r\n\r\n",
  Vl53l0xStreamCommand, /* The function to run. */
  -1 /* Multiple parameters are expected. */
};
/*-----------------------------------------------------------*/
/* CLI command structure : stop */
const CLI_Command_Definition_t Vl53l0xStopCommandDefinition =
{
  ( const int8_t * ) "stop", /* The command string to type. */
  ( const int8_t * ) "stop:\r\nStop continuous or timed ranging\r\n\r\n",
  Vl53l0xStopCommand, /* The function to run. */
  0 /* No parameters are expected. */
};
/*-----------------------------------------------------------*/
/* CLI command structure : units */
const CLI_Command_Definition_t Vl53l0xUnitsCommandDefinition =
{
  ( const int8_t * ) "units", /* The command string to type. */
  ( const int8_t * ) "units:\r\nSetup the range output unit: mm, cm, inch\r\n\r\n",
  Vl53l0xUnitsCommand, /* The function to run. */
  1 /* one parameter is expected. */
};
/*-----------------------------------------------------------*/
/* CLI command structure : max */
const CLI_Command_Definition_t Vl53l0xMaxCommandDefinition =
{
  ( const int8_t * ) "max", /* The command string to type. */
  ( const int8_t * ) "max:\r\nCalibrate maximum distance\r\n\r\n",
  Vl53l0xMaxCommand, /* The function to run. */
  0 /* one parameter is expected. */
};
/*-----------------------------------------------------------*/
/* CLI command structure : range */
const CLI_Command_Definition_t rangeModParamCommandDefinition =
{
  ( const int8_t * ) "range", /* The command string to type. */
		( const int8_t * ) "range:\r\nDisplay the value of module parameter: range\r\n\r\n",
  rangeModParamCommand, /* The function to run. */
  0 /* one parameter is expected. */
};

/* -----------------------------------------------------------------------
  |                        Private Functions                              |
   -----------------------------------------------------------------------
*/

/* --- H08R6 module initialization.
*/
void Module_Init(void)
{

  /* Array ports */
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  MX_USART3_UART_Init();
#ifdef H08R6
  MX_USART4_UART_Init();
#endif
  MX_USART5_UART_Init();
  MX_USART6_UART_Init();

  /* create a event group for measurement ranging */
  handleNewReadyData = xEventGroupCreate();

  /* I2C initialization */
  MX_I2C_Init();

  /* VL53L0X initialization */
  Vl53l0xInit();
	
	/* Create a ToF task */
	xTaskCreate(ToFTask, (const char *) "ToFTask", (2*configMINIMAL_STACK_SIZE), NULL, osPriorityNormal-osPriorityIdle, &ToFHandle);	

}

/*-----------------------------------------------------------*/

/* --- H08R6 message processing task.
*/
Module_Status Module_MessagingTask(uint16_t code, uint8_t port, uint8_t src, uint8_t dst, uint8_t shift)
{
  Module_Status result = H08R6_OK;
  uint32_t period;
  uint32_t timeout;

  switch (code)
  {
    case CODE_H08R6_GET_INFO:
      break;
    case CODE_H08R6_SAMPLE:
      Sample_ToF();
			SendMeasurementResult(REQ_SAMPLE_ARR, h08r6_range, dst, port, NULL);
      break;
    case CODE_H08R6_STREAM_PORT:
			period = ( (uint32_t) cMessage[port-1][shift] << 24 ) + ( (uint32_t) cMessage[port-1][1+shift] << 16 ) + ( (uint32_t) cMessage[port-1][2+shift] << 8 ) + cMessage[port-1][3+shift];
			timeout = ( (uint32_t) cMessage[port-1][4+shift] << 24 ) + ( (uint32_t) cMessage[port-1][5+shift] << 16 ) + ( (uint32_t) cMessage[port-1][6+shift] << 8 ) + cMessage[port-1][7+shift];
      Stream_ToF_Port(period, timeout, port, dst, false);
      break;
    case CODE_H08R6_STREAM_MEM:
      period = ( (uint32_t) cMessage[port-1][shift] << 24 ) + ( (uint32_t) cMessage[port-1][1+shift] << 16 ) + ( (uint32_t) cMessage[port-1][2+shift] << 8 ) + cMessage[port-1][3+shift];
			timeout = ( (uint32_t) cMessage[port-1][4+shift] << 24 ) + ( (uint32_t) cMessage[port-1][5+shift] << 16 ) + ( (uint32_t) cMessage[port-1][6+shift] << 8 ) + cMessage[port-1][7+shift];
      Stream_ToF_Memory(period, timeout, &h08r6_range);
      break;
    case CODE_H08R6_RESULT_MEASUREMENT:
      break;
    case CODE_H08R6_STOP_RANGING:
      Stop_ToF();
      break;
    case CODE_H08R6_SET_UNIT:
      SetRangeUnit(cMessage[port-1][shift]);
      break;
    case CODE_H08R6_GET_UNIT:
      messageParams[0] = GetRangeUnit();
      SendMessageFromPort(port, myID, dst, CODE_H08R6_RESPOND_GET_UNIT, 1);
      break;
    default:
      result = H08R6_ERR_UnknownMessage;
      break;
  }

  return result;
}

/*-----------------------------------------------------------*/

/* --- Register this module CLI Commands
*/
void RegisterModuleCLICommands(void)
{
  FreeRTOS_CLIRegisterCommand( &demoCommandDefinition);
	FreeRTOS_CLIRegisterCommand( &Vl53l0xSampleCommandDefinition);
  FreeRTOS_CLIRegisterCommand( &Vl53l0xStreamCommandDefinition);
  FreeRTOS_CLIRegisterCommand( &Vl53l0xStopCommandDefinition);
  FreeRTOS_CLIRegisterCommand( &Vl53l0xUnitsCommandDefinition);
  FreeRTOS_CLIRegisterCommand( &Vl53l0xMaxCommandDefinition);
	FreeRTOS_CLIRegisterCommand( &rangeModParamCommandDefinition);
}

/*-----------------------------------------------------------*/

/* --- Get the port for a given UART.
*/
uint8_t GetPort(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART4)
    return P1;
  else if (huart->Instance == USART2)
    return P2;
  else if (huart->Instance == USART6)
    return P3;
  else if (huart->Instance == USART3)
    return P4;
  else if (huart->Instance == USART1)
    return P5;
  else if (huart->Instance == USART5)
    return P6;

  return 0;
}

/*-----------------------------------------------------------*/

/* --- ToF streaming task 
*/
void ToFTask(void * argument)
{
	while(1)
	{
		// Process data when it's ready from the sensor or when the period timer is expired
		if (tofState == REQ_MEASUREMENT_READY || (HAL_GetTick()-t0) >= tofPeriod)
		{
			switch (tofMode)
			{				
				case REQ_STREAM_MEMORY :
					h08r6_range = GetMeasurementResult();
					SendMeasurementResult(REQ_STREAM_MEMORY, h08r6_range, 0, 0, tofBuffer);		
					break;
				
				case REQ_STREAM_PORT_CLI :				
					h08r6_range = GetMeasurementResult();
					SendMeasurementResult(REQ_STREAM_PORT_CLI, h08r6_range, 0, PcPort, NULL);		
					break;
				
				case REQ_STREAM_VERBOSE_PORT_CLI :
					h08r6_range = GetMeasurementResult();
					SendMeasurementResult(REQ_STREAM_VERBOSE_PORT_CLI, h08r6_range, 0, PcPort, NULL);		
					break;
				
				case REQ_STREAM_PORT_ARR :
					h08r6_range = GetMeasurementResult();
					SendMeasurementResult(REQ_STREAM_PORT_ARR, h08r6_range, tofModule, tofPort, NULL);		
					break;
				
				default:
					break;
			}
			
			t0 = HAL_GetTick();			// Reset the timer
		}

		tofState = REQ_IDLE;
		taskYIELD();
	}
}

/*-----------------------------------------------------------*/

static void Vl53l0xInit(void)
{
  VL53L0X_Error status = VL53L0X_ERROR_NONE;
  uint32_t refSpadCount;
  uint8_t isApertureSpads;
  uint8_t VhvSettings;
  uint8_t PhaseCal;

  vl53l0x_HandleDevice.I2cDevAddr = 0x52;
  vl53l0x_HandleDevice.comms_type = 1; /* Using I2C communication */
  vl53l0x_HandleDevice.comms_speed_khz = 100; /* 100kHz for I2C */

  vl53l0x_set_xshut_pin();
  Delay_us(100);

  if (VL53L0X_ERROR_NONE == status)
  {
    status = VL53L0X_DataInit(&vl53l0x_HandleDevice);
  }

  if (status == VL53L0X_ERROR_NONE)
  {
    /* Device Initialization */
    status = VL53L0X_StaticInit(&vl53l0x_HandleDevice);
  }

  if(status == VL53L0X_ERROR_NONE)
  {
    /* Device Initialization */
    status = VL53L0X_PerformRefSpadManagement(&vl53l0x_HandleDevice,
                                              &refSpadCount,
                                              &isApertureSpads);
  }

  if (status == VL53L0X_ERROR_NONE)
  {
    /* Device Initialization */
    status = VL53L0X_PerformRefCalibration(&vl53l0x_HandleDevice,
                                           &VhvSettings,
                                           &PhaseCal);
  }

  // Enable/Disable Sigma and Signal check
  if (status == VL53L0X_ERROR_NONE)
  {
    status = VL53L0X_SetLimitCheckEnable(&vl53l0x_HandleDevice,
                                         VL53L0X_CHECKENABLE_SIGMA_FINAL_RANGE,
                                         1);
  }

  if (status == VL53L0X_ERROR_NONE)
  {
    status = VL53L0X_SetLimitCheckEnable(&vl53l0x_HandleDevice,
                                         VL53L0X_CHECKENABLE_SIGNAL_RATE_FINAL_RANGE,
                                         1);
  }

  if (status == VL53L0X_ERROR_NONE)
  {
    status = VL53L0X_SetLimitCheckValue(&vl53l0x_HandleDevice,
                                        VL53L0X_CHECKENABLE_SIGNAL_RATE_FINAL_RANGE,
                                        (FixPoint1616_t)(0.25*65536));
  }

  if (status == VL53L0X_ERROR_NONE)
  {
    status = VL53L0X_SetLimitCheckValue(&vl53l0x_HandleDevice,
                                        VL53L0X_CHECKENABLE_SIGMA_FINAL_RANGE,
                                        (FixPoint1616_t)(18*65536));
  }

  if (status == VL53L0X_ERROR_NONE)
  {
    /* Timing budget for High accuracy */
    status = VL53L0X_SetMeasurementTimingBudgetMicroSeconds(&vl53l0x_HandleDevice, 200000);
  }

/*   if (status == VL53L0X_ERROR_NONE)
  {
    status = VL53L0X_GetOffsetCalibrationDataMicroMeter(&vl53l0x_HandleDevice, &offsetCalibration);
  }

  if (status == VL53L0X_ERROR_NONE)
  {
    status = VL53L0X_SetOffsetCalibrationDataMicroMeter(&vl53l0x_HandleDevice, offsetCalibration);
  } */

  if(status == VL53L0X_ERROR_NONE)
  {
    /* no need to do this when we use VL53L0X_PerformSingleRangingMeasurement */
    /* Setup in single ranging mode */
    status = VL53L0X_SetDeviceMode(&vl53l0x_HandleDevice, VL53L0X_DEVICEMODE_SINGLE_RANGING);
  }

  if (status == VL53L0X_ERROR_NONE)
  {
    VL53L0X_StartMeasurement(&vl53l0x_HandleDevice);
  }

  /* Setting interrupt on INT pin of VL53L0X */
  if (VL53L0X_ERROR_NONE == status)
  {
    status = VL53L0X_SetGpioConfig(&vl53l0x_HandleDevice,
                                   0,
                                   0,
                                   VL53L0X_GPIOFUNCTIONALITY_NEW_MEASURE_READY,
                                   VL53L0X_INTERRUPTPOLARITY_LOW);
  }

  if (VL53L0X_ERROR_NONE == status)
  {
    status = VL53L0X_SetInterruptThresholds(&vl53l0x_HandleDevice, 0, 60, 200);
  }

  if(VL53L0X_ERROR_NONE == status)
  {
    status = VL53L0X_ClearInterruptMask(&vl53l0x_HandleDevice, 0);
  }
}

/*-----------------------------------------------------------*/

/* --- Send result measurement ranging to other UART port
*/
static void HandleTimeout(TimerHandle_t xTimer)
{
  uint32_t tid = 0;

  /* close DMA stream */
  tid = ( uint32_t ) pvTimerGetTimerID( xTimerTof );
  if (TIMERID_TIMEOUT_MEASUREMENT == tid)
  {
    startMeasurementRanging = STOP_MEASUREMENT_RANGING;
		tofMode = REQ_IDLE;		// Stop the streaming task
  }
}

/*-----------------------------------------------------------*/

/* --- Select run mode for VL53L0CX
*/
static VL53L0X_Error SetMeasurementMode(uint8_t mode, uint32_t period, uint32_t timeout)
{
  VL53L0X_Error status = VL53L0X_ERROR_NONE;

  if (VL53L0x_MODE_SINGLE == mode)
  {
    status = VL53L0X_SetDeviceMode(&vl53l0x_HandleDevice, VL53L0X_DEVICEMODE_SINGLE_RANGING);
  }
  else if (VL53L0x_MODE_CONTINUOUS == mode)
  {
    status = VL53L0X_SetDeviceMode(&vl53l0x_HandleDevice, VL53L0X_DEVICEMODE_CONTINUOUS_RANGING);
  }
  else if (VL53L0x_MODE_CONTINUOUS_TIMED == mode)
  {
    if(VL53L0X_ERROR_NONE == status)
    {
      status = VL53L0X_SetDeviceMode(&vl53l0x_HandleDevice, VL53L0X_DEVICEMODE_CONTINUOUS_TIMED_RANGING);
    }
    if(VL53L0X_ERROR_NONE == status)
    {
      status = VL53L0X_SetInterMeasurementPeriodMilliSeconds(&vl53l0x_HandleDevice, period);
    }
  }
  else
  {
    /* nothing to do here */
  }

  if ((timeout > 0) && (timeout < 0xFFFFFFFF))
  {
    /* start software timer which will create event timeout */
    /* Create a timeout timer */
    xTimerTof = xTimerCreate( "Timeout Measurement", pdMS_TO_TICKS(timeout), pdFALSE, ( void * ) TIMERID_TIMEOUT_MEASUREMENT, HandleTimeout );
    /* Start the timeout timer */
    xTimerStart( xTimerTof, portMAX_DELAY );
  }

  /* start measurement */
  if (status == VL53L0X_ERROR_NONE)
  {
    VL53L0X_StartMeasurement(&vl53l0x_HandleDevice);
  }

  return status;
}

/*-----------------------------------------------------------*/

/* --- Get measurement result
*/
static float GetMeasurementResult(void)
{
  VL53L0X_RangingMeasurementData_t measurementResult;
  VL53L0X_Error status = VL53L0X_ERROR_NONE;

  status = VL53L0X_GetRangingMeasurementData(&vl53l0x_HandleDevice, &measurementResult);
	
	if (VL53L0X_ERROR_NONE == status) {
		status = VL53L0X_ClearInterruptMask(&vl53l0x_HandleDevice, 0);
		return (float)measurementResult.RangeMilliMeter;
	} else {
		return 0;
	}
}

/*-----------------------------------------------------------*/

/* --- Get measurement result and convert from "mm" to other units
 * Input : distance (mm)
*/
static float ConvertCurrentUnit(float distance)
{
  float temp = distance;

  if (UNIT_MEASUREMENT_CM == h08r6UnitMeasurement)
  {
    temp = distance / 10;
  }
  else if (UNIT_MEASUREMENT_INCH == h08r6UnitMeasurement)
  {
    temp = distance / 25.4; /* 1mm = (1/25.4)″ = 0.03937007874″ */
  }
  else
  {
    /* nothing to do here */
  }

  return temp;
}

/*-----------------------------------------------------------*/

/* --- Send measurement results
*/
static void SendMeasurementResult(uint8_t request, float distance, uint8_t module, uint8_t port, float *buffer)
{
  uint16_t numberOfParams;
  int8_t *pcOutputString;
  static const int8_t *pcDistanceMsg = ( int8_t * ) "Distance (%s): %.2f\r\n";
	static const int8_t *pcDistanceVerboseMsg = ( int8_t * ) "%.2f\r\n";
  static const int8_t *pcOutMaxRange = ( int8_t * ) "MAX\r\n";
	static const int8_t *pcOutTimeout = ( int8_t * ) "TIMEOUT\r\n";
  float tempData;
  char *strUnit;

  /* Get CLI output buffer */
  pcOutputString = FreeRTOS_CLIGetOutputBuffer();
  tempData = ConvertCurrentUnit(distance);

	if (request != REQ_SAMPLE_VERBOSE_CLI && request != REQ_STREAM_VERBOSE_PORT_CLI)
	{
		strUnit = malloc(6*sizeof(char));
		memset(strUnit, 0, (6*sizeof(char)));
		if (UNIT_MEASUREMENT_MM == h08r6UnitMeasurement)
		{
			sprintf( ( char * ) strUnit, "mm");
		}
		else if (UNIT_MEASUREMENT_CM == h08r6UnitMeasurement)
		{
			sprintf( ( char * ) strUnit, "cm");
		}
		else if (UNIT_MEASUREMENT_INCH == h08r6UnitMeasurement)
		{
			sprintf( ( char * ) strUnit, "inch");
		}
		else
		{
			/* nothing to do here */
		}
	}

	// If the value is out of range
  if (tempData >= h08r6MaxRange)
  {
    switch(request)
    {
      case REQ_SAMPLE_CLI:
      case REQ_STREAM_PORT_CLI:
        request = REQ_OUT_RANGE_CLI;
        break;
			case REQ_STREAM_MEMORY:
				break;
      case REQ_SAMPLE_ARR:
      case REQ_STREAM_PORT_ARR:
        request = REQ_OUT_RANGE_ARR;
        break;
      default:        
        break;
    }
  }

	// If measurement timeout occured 
	if (tofState == REQ_TIMEOUT)
	{
    switch(request)
    {
      case REQ_SAMPLE_CLI:
      case REQ_STREAM_PORT_CLI:
        request = REQ_TIMEOUT_CLI;
        break;
			case REQ_SAMPLE_VERBOSE_CLI:
			case REQ_STREAM_VERBOSE_PORT_CLI:
				request = REQ_TIMEOUT_VERBOSE_CLI;
				break;
			case REQ_STREAM_MEMORY:
				request = REQ_TIMEOUT_MEMORY;
				break;
      case REQ_SAMPLE_ARR:
      case REQ_STREAM_PORT_ARR:
        request = REQ_TIMEOUT_ARR;
        break;
      default:        
        break;
    }				
	}
	
	// Send the value to appropriate outlet
  switch(request)
  {
    case REQ_SAMPLE_CLI:
    case REQ_STREAM_PORT_CLI:
      sprintf( ( char * ) pcOutputString, ( char * ) pcDistanceMsg, strUnit, tempData);
      writePxMutex(PcPort, (char *)pcOutputString, strlen((char *)pcOutputString), cmd500ms, HAL_MAX_DELAY);
			CheckForEnterKey();
      break;

    case REQ_SAMPLE_VERBOSE_CLI:
    case REQ_STREAM_VERBOSE_PORT_CLI:
      sprintf( ( char * ) pcOutputString, ( char * ) pcDistanceVerboseMsg, tempData);
      writePxMutex(PcPort, (char *)pcOutputString, strlen((char *)pcOutputString), cmd500ms, HAL_MAX_DELAY);
			CheckForEnterKey();
      break;
		
    case REQ_SAMPLE_ARR:
    case REQ_STREAM_PORT_ARR:
      memset(messageParams, 0, sizeof(messageParams));
      numberOfParams = sizeof(float);
      memcpy(messageParams, &tempData, sizeof(float));
      SendMessageFromPort(port, myID, module, CODE_H08R6_RESULT_MEASUREMENT, numberOfParams);
      break;
		
    case REQ_STREAM_MEMORY:
      memset(buffer, 0, sizeof(float));
      memcpy(buffer, &tempData, sizeof(float));
      break;
		
    case REQ_OUT_RANGE_CLI:
      strcpy( ( char * ) pcOutputString, ( char * ) pcOutMaxRange);
      writePxMutex(PcPort, (char *)pcOutputString, strlen((char *)pcOutputString), cmd500ms, HAL_MAX_DELAY);
			CheckForEnterKey();
      break;
		
    case REQ_OUT_RANGE_ARR:
      SendMessageFromPort(port, myID, module, CODE_H08R6_MAX_RANGE, 0);
      break;

    case REQ_TIMEOUT_CLI:
      strcpy( ( char * ) pcOutputString, ( char * ) pcOutTimeout);
      writePxMutex(PcPort, (char *)pcOutputString, strlen((char *)pcOutputString), cmd500ms, HAL_MAX_DELAY);
			CheckForEnterKey();
      break;

    case REQ_TIMEOUT_VERBOSE_CLI:
      sprintf( ( char * ) pcOutputString, ( char * ) pcDistanceVerboseMsg, 0);
      writePxMutex(PcPort, (char *)pcOutputString, strlen((char *)pcOutputString), cmd500ms, HAL_MAX_DELAY);
			CheckForEnterKey();
      break;
		
		case REQ_TIMEOUT_MEMORY:
      memset(buffer, 0, sizeof(float));
      break;

    case REQ_TIMEOUT_ARR:
      SendMessageFromPort(port, myID, module, CODE_H08R6_TIMEOUT, 0);
      break;
		
    default:
      break;
  }
	
	if (request != REQ_SAMPLE_VERBOSE_CLI && request != REQ_STREAM_VERBOSE_PORT_CLI){
		free(strUnit);
	}
}

/*-----------------------------------------------------------*/

/* --- Check for CLI stop key
*/
static void CheckForEnterKey(void)
{	
	// Look for ENTER key to stop the stream
	for (uint8_t chr=0 ; chr<MSG_RX_BUF_SIZE ; chr++)
	{
		if (UARTRxBuf[PcPort-1][chr] == '\r') {
			UARTRxBuf[PcPort-1][chr] = 0;
			startMeasurementRanging = STOP_MEASUREMENT_RANGING;
			tofMode = REQ_IDLE;		// Stop the streaming task
			xTimerStop( xTimerTof, 0 ); // Stop any running timeout timer
			break;
		}
	}
}

/*-----------------------------------------------------------*/

/* -----------------------------------------------------------------------
  |                               APIs                                    |
   -----------------------------------------------------------------------
*/

/* --- Take one measurement sample - polling until timeout (triggers ST API Single Ranging)
*/
float Sample_ToF(void)
{
	tofMode = REQ_SAMPLE;
  SetMeasurementMode(VL53L0x_MODE_SINGLE, 0, 0);
	startMeasurementRanging = START_MEASUREMENT_RANGING;
	
	if (tofState == REQ_TIMEOUT) {
		return 0;
	} else {	
		h08r6_range = GetMeasurementResult();
		tofState = REQ_IDLE;
		return h08r6_range;
	}
}

/*-----------------------------------------------------------*/

/* --- Stream measurements continuously to a port (triggers ST API Continuous Ranging)
*/
void Stream_ToF_Port(uint32_t period, uint32_t timeout, uint8_t port, uint8_t module, bool verbose)
{
	if (!port && !module && verbose)
		tofMode = REQ_STREAM_VERBOSE_PORT_CLI;
	else if (!port && !module)
		tofMode = REQ_STREAM_PORT_CLI;
	else
		tofMode = REQ_STREAM_PORT_ARR;
	
	tofPeriod = period;
	tofTimeout = timeout;
	tofPort = port;
	tofModule = module;
	
  if (0 == period)
  {
    SetMeasurementMode(VL53L0x_MODE_CONTINUOUS, 0, timeout);
  }
  else
  {
    SetMeasurementMode(VL53L0x_MODE_CONTINUOUS_TIMED, period, timeout);
  }

  startMeasurementRanging = START_MEASUREMENT_RANGING;
	t0 = HAL_GetTick();
	h08r6_range = GetMeasurementResult();
}

/*-----------------------------------------------------------*/

/* --- Stream measurements continuously to a memory location (triggers ST API Continuous Ranging)
*/
void Stream_ToF_Memory(uint32_t period, uint32_t timeout, float* buffer)
{
	tofMode = REQ_STREAM_MEMORY;
	tofPeriod = period;
	tofTimeout = timeout;
	tofBuffer = buffer;
	
  if (0 == period)
  {
    SetMeasurementMode(VL53L0x_MODE_CONTINUOUS, 0, timeout);
  }
  else
  {
    SetMeasurementMode(VL53L0x_MODE_CONTINUOUS_TIMED, period, timeout);
  }

  startMeasurementRanging = START_MEASUREMENT_RANGING;
	t0 = HAL_GetTick();
	h08r6_range = GetMeasurementResult();
}

/*-----------------------------------------------------------*/

/* --- Stop ToF measurement
*/
Module_Status Stop_ToF(void)
{
  uint32_t StopCompleted = 0;
  uint32_t loop = 0;
  Module_Status result = H08R6_OK;
  VL53L0X_Error status = VL53L0X_ERROR_NONE;
  VL53L0X_RangingMeasurementData_t measurementResult;

  VL53L0X_StopMeasurement(&vl53l0x_HandleDevice);
  do{
    status = VL53L0X_GetStopCompletedStatus(&vl53l0x_HandleDevice, &StopCompleted);
    if ((0 == StopCompleted) || (VL53L0X_ERROR_NONE != status))
    {
      break;
    }
    loop++;
    VL53L0X_PollingDelay(&vl53l0x_HandleDevice);
  } while (loop < VL53L0X_DEFAULT_MAX_LOOP);

  if (loop >= VL53L0X_DEFAULT_MAX_LOOP)
  {
    result = H08R6_ERR_Timeout;
  }
  else
  {
    if (VL53L0X_ERROR_NONE != status)
    {
      result = H08R6_ERROR;
    }
    else
    {
      startMeasurementRanging = STOP_MEASUREMENT_RANGING;
			tofMode = REQ_IDLE;		// Stop the streaming task
      status = VL53L0X_GetRangingMeasurementData(&vl53l0x_HandleDevice, &measurementResult);

      if(VL53L0X_ERROR_NONE == status)
      {
        status = VL53L0X_ClearInterruptMask(&vl53l0x_HandleDevice, 0);
      }
    }
  }

  return result;
}

/*-----------------------------------------------------------*/

/* --- Set measurement unit
*/
Module_Status SetRangeUnit(uint8_t input)
{
  Module_Status result = H08R6_OK;

  switch(input)
  {
    case UNIT_MEASUREMENT_MM:
    case UNIT_MEASUREMENT_CM:
    case UNIT_MEASUREMENT_INCH:
      h08r6UnitMeasurement = input;
      break;
    default:
      result = H08R6_ERROR;
      break;
  }

  return result;
}

/*-----------------------------------------------------------*/

/* --- Get measurement unit
*/
uint8_t GetRangeUnit(void)
{
  return h08r6UnitMeasurement;
}

/* -----------------------------------------------------------------------
  |                             Commands                                  |
   -----------------------------------------------------------------------
*/

portBASE_TYPE demoCommand( int8_t *pcWriteBuffer, size_t xWriteBufferLen, const int8_t *pcCommandString )
{
	static const int8_t *pcMessage = ( int8_t * ) "Streaming range measurements at 2 Hz for 10 seconds\r\n";
	
	/* Remove compile time warnings about unused parameters, and check the
	write buffer is not NULL.  NOTE - for simplicity, this example assumes the
	write buffer length is adequate, so does not check for buffer overflows. */
	( void ) pcCommandString;
	( void ) xWriteBufferLen;
	configASSERT( pcWriteBuffer );

	/* Respond to the command */
	strcpy(( char * ) pcWriteBuffer, ( char * ) pcMessage);
	writePxMutex(PcPort, (char *)pcWriteBuffer, strlen((char *)pcWriteBuffer), cmd50ms, HAL_MAX_DELAY);
	Stream_ToF_Port(500, 10000, 0, 0, false);
	
	/* Wait till the end of stream */
	while(startMeasurementRanging != STOP_MEASUREMENT_RANGING){	Delay_ms(1); };
	/* clean terminal output */
	memset((char *) pcWriteBuffer, 0, strlen((char *)pcWriteBuffer));
			
	/* There is no more data to return after this single string, so return
	pdFALSE. */
	return pdFALSE;
}

/*-----------------------------------------------------------*/

static portBASE_TYPE Vl53l0xSampleCommand( int8_t *pcWriteBuffer, size_t xWriteBufferLen, const int8_t *pcCommandString )
{
  /* Remove compile time warnings about unused parameters, and check the
  write buffer is not NULL.  NOTE - for simplicity, this example assumes the
  write buffer length is adequate, so does not check for buffer overflows. */
  ( void ) pcCommandString;
  ( void ) xWriteBufferLen;
  configASSERT( pcWriteBuffer );
	
	Sample_ToF();
  SendMeasurementResult(REQ_SAMPLE_CLI, h08r6_range, 0, PcPort, NULL);

  /* clean terminal output */
  memset((char *) pcWriteBuffer, 0, configCOMMAND_INT_MAX_OUTPUT_SIZE);

  /* There is no more data to return after this single string, so return pdFALSE. */
  return pdFALSE;
}

/*-----------------------------------------------------------*/

static portBASE_TYPE Vl53l0xStreamCommand( int8_t *pcWriteBuffer, size_t xWriteBufferLen, const int8_t *pcCommandString )
{
	static const int8_t *pcMessageBuffer = ( int8_t * ) "Streaming measurements to internal buffer. Access in the CLI using module parameter: range\n\r";
	static const int8_t *pcMessageModule = ( int8_t * ) "Streaming measurements to port P%d in module #%d\n\r";
	static const int8_t *pcMessageCLI = ( int8_t * ) "Streaming measurements to the CLI\n\n\r";
	static const int8_t *pcMessageError = ( int8_t * ) "Wrong parameter\r\n";
  int8_t *pcParameterString1; /* period */
  int8_t *pcParameterString2; /* timeout */
  int8_t *pcParameterString3; /* port or buffer */
  int8_t *pcParameterString4; /* module */
  portBASE_TYPE xParameterStringLength1 = 0;
  portBASE_TYPE xParameterStringLength2 = 0;
  portBASE_TYPE xParameterStringLength3 = 0;
  portBASE_TYPE xParameterStringLength4 = 0;
  uint32_t period = 0;
  uint32_t timeout = 0;
  uint8_t port = 0;
  uint8_t module = 0;
  Module_Status result = H08R6_OK;

  /* Remove compile time warnings about unused parameters, and check the
  write buffer is not NULL.  NOTE - for simplicity, this example assumes the
  write buffer length is adequate, so does not check for buffer overflows. */
  ( void ) xWriteBufferLen;
  configASSERT( pcWriteBuffer );

  /* Obtain the 1st parameter string: period */
  pcParameterString1 = ( int8_t * ) FreeRTOS_CLIGetParameter (pcCommandString, 1, &xParameterStringLength1);
  /* Obtain the 2nd parameter string: timeout */
  pcParameterString2 = ( int8_t * ) FreeRTOS_CLIGetParameter (pcCommandString, 2, &xParameterStringLength2);
  /* Obtain the 3rd parameter string: port */
  pcParameterString3 = ( int8_t * ) FreeRTOS_CLIGetParameter (pcCommandString, 3, &xParameterStringLength3);
  /* Obtain the 4th parameter string: module */
  pcParameterString4 = ( int8_t * ) FreeRTOS_CLIGetParameter (pcCommandString, 4, &xParameterStringLength4);

  if (NULL != pcParameterString1)
  {
    period = atoi( (char *)pcParameterString1);
  }
  else
  {
    result = H08R6_ERR_WrongParams;
  }
  if (NULL != pcParameterString2)
  {
    if (!strncmp((const char *)pcParameterString2, "inf", 3))
    {
      timeout = portMAX_DELAY;
    }
    else
    {
      timeout = atoi( (char *)pcParameterString2);
    }
  }
  else
  {
    result = H08R6_ERR_WrongParams;
  }

	/* streaming data to internal buffer (module parameter) */
	if (NULL != pcParameterString3 && !strncmp((const char *)pcParameterString3, "buffer", 6)) 
	{
		strcpy(( char * ) pcWriteBuffer, ( char * ) pcMessageBuffer);
		Stream_ToF_Memory(period, timeout, &h08r6_range);
		// Return right away here as we don't want to block the CLI
		return pdFALSE;
	} 
	/* streaming data to port */
	else if (NULL != pcParameterString3 && NULL != pcParameterString4 && pcParameterString3[0] == 'P') 
	{
		port = ( uint8_t ) atol( ( char * ) pcParameterString3+1 );
		module = atoi( (char *)pcParameterString4);
		sprintf( ( char * ) pcWriteBuffer, ( char * ) pcMessageModule, port, module);
		Stream_ToF_Port(period, timeout, port, module, false);
		// Return right away here as we don't want to block the CLI
		return pdFALSE;
	} 
	/* Stream to the CLI */
	else if (NULL == pcParameterString4) 
	{	
		if (NULL != pcParameterString3 && !strncmp((const char *)pcParameterString3, "-v", 2)) {
			Stream_ToF_Port(period, timeout, 0, 0, true);
		} else {
			strcpy(( char * ) pcWriteBuffer, ( char * ) pcMessageCLI);
			writePxMutex(PcPort, (char *)pcWriteBuffer, strlen((char *)pcWriteBuffer), cmd50ms, HAL_MAX_DELAY);
			Stream_ToF_Port(period, timeout, 0, 0, false);
		}		
		/* Wait till the end of stream */
		while(startMeasurementRanging != STOP_MEASUREMENT_RANGING){	Delay_ms(1); };
		/* clean terminal output */
		memset((char *) pcWriteBuffer, 0, strlen((char *)pcWriteBuffer));
	}		
	else 
	{
		result = H08R6_ERR_WrongParams;
	}	

  if (H08R6_ERR_WrongParams == result)
  {
    strcpy( ( char * ) pcWriteBuffer, ( char * ) pcMessageError);
  }
	
  /* There is no more data to return after this single string, so return pdFALSE. */
  return pdFALSE;
}

/*-----------------------------------------------------------*/

static portBASE_TYPE Vl53l0xStopCommand( int8_t *pcWriteBuffer, size_t xWriteBufferLen, const int8_t *pcCommandString )
{
  Module_Status result = H08R6_OK;
	static const int8_t *pcMessageOK = ( int8_t * ) "Streaming stopped successfuly\r\n";
	static const int8_t *pcMessageError = ( int8_t * ) "Command failed! Please try again or reboot\r\n";
	

  /* Remove compile time warnings about unused parameters, and check the
  write buffer is not NULL.  NOTE - for simplicity, this example assumes the
  write buffer length is adequate, so does not check for buffer overflows. */
  ( void ) pcCommandString;
  ( void ) xWriteBufferLen;
  configASSERT( pcWriteBuffer );

  result = Stop_ToF();

  if (H08R6_OK == result)
  {
    strcpy( ( char * ) pcWriteBuffer, ( char * ) pcMessageOK);
  }
  else
  {
    strcpy( ( char * ) pcWriteBuffer, ( char * ) pcMessageError);
  }

  /* There is no more data to return after this single string, so return pdFALSE. */
  return pdFALSE;
}

/*-----------------------------------------------------------*/

static portBASE_TYPE Vl53l0xUnitsCommand( int8_t *pcWriteBuffer, size_t xWriteBufferLen, const int8_t *pcCommandString )
{
  Module_Status result = H08R6_OK;
  int8_t *pcParameterString1;
  portBASE_TYPE xParameterStringLength1 = 0;
  static const int8_t *pcMessageWrongParam = ( int8_t * ) "Wrong parameter!\r\n";

  /* Remove compile time warnings about unused parameters, and check the
  write buffer is not NULL.  NOTE - for simplicity, this example assumes the
  write buffer length is adequate, so does not check for buffer overflows. */
  ( void ) xWriteBufferLen;
  configASSERT( pcWriteBuffer );

  /* 1st parameter for naming of uart port: P1 to P6 */
  pcParameterString1 = ( int8_t * ) FreeRTOS_CLIGetParameter (pcCommandString, 1, &xParameterStringLength1);
  if (!strncmp((const char *)pcParameterString1, "mm", 2))
  {
    h08r6UnitMeasurement = UNIT_MEASUREMENT_MM;
    strcpy( ( char * ) pcWriteBuffer, ( char * ) "Used measurement unit: mm\r\n" );
  }
  else if (!strncmp((const char *)pcParameterString1, "cm", 2))
  {
    h08r6UnitMeasurement = UNIT_MEASUREMENT_CM;
    strcpy( ( char * ) pcWriteBuffer, ( char * ) "Used measurement unit: cm\r\n" );
  }
  else if (!strncmp((const char *)pcParameterString1, "inch", 4))
  {
    h08r6UnitMeasurement = UNIT_MEASUREMENT_INCH;
    strcpy( ( char * ) pcWriteBuffer, ( char * ) "Used measurement unit: inch\r\n" );
  }
  else
  {
    result = H08R6_ERR_WrongParams;
  }

  /* Respond to the command */
  if (H08R6_ERR_WrongParams == result)
  {
    strcpy( ( char * ) pcWriteBuffer, ( char * ) pcMessageWrongParam );
  }

  /* There is no more data to return after this single string, so return pdFALSE. */
  return pdFALSE;
}

/*-----------------------------------------------------------*/

static portBASE_TYPE Vl53l0xMaxCommand( int8_t *pcWriteBuffer, size_t xWriteBufferLen, const int8_t *pcCommandString )
{
  uint8_t temp = 0;
	static const int8_t *pcStartMsg = ( int8_t * ) "Calibrating maximum distance. Make sure the IR sensor is unblocked (~2m) and press any key when ready\r\n";
  static const int8_t *pcMaxDistanceMsg = ( int8_t * ) "Maximum distance (mm): %.2f\r\n";
	
  /* Remove compile time warnings about unused parameters, and check the
  write buffer is not NULL.  NOTE - for simplicity, this example assumes the
  write buffer length is adequate, so does not check for buffer overflows. */
  ( void ) xWriteBufferLen;
  configASSERT( pcWriteBuffer );

	strcpy( ( char * ) pcWriteBuffer, ( char * ) pcStartMsg);
	writePxMutex(PcPort, (char *)pcWriteBuffer, strlen((char *)pcWriteBuffer), cmd50ms, HAL_MAX_DELAY);
	memset((char *) pcWriteBuffer, 0, strlen((char *)pcWriteBuffer));
	// Wait for user to be ready
	readPxMutex(PcPort, (char *)pcWriteBuffer, sizeof(char), cmd500ms, HAL_MAX_DELAY);
	
	while(temp < 10)
	{
		h08r6_range += Sample_ToF();;
		temp++;
	}
	h08r6MaxRange = h08r6_range / (temp - 1);

	sprintf( ( char * ) pcWriteBuffer, ( char * ) pcMaxDistanceMsg, h08r6MaxRange);

  /* There is no more data to return after this single string, so return pdFALSE. */
  return pdFALSE;
}

/*-----------------------------------------------------------*/

static portBASE_TYPE rangeModParamCommand( int8_t *pcWriteBuffer, size_t xWriteBufferLen, const int8_t *pcCommandString )
{
  static const int8_t *pcDistanceVerboseMsg = ( int8_t * ) "%.2f\r\n";

  /* Remove compile time warnings about unused parameters, and check the
  write buffer is not NULL.  NOTE - for simplicity, this example assumes the
  write buffer length is adequate, so does not check for buffer overflows. */
  ( void ) xWriteBufferLen;
  configASSERT( pcWriteBuffer );

  sprintf( ( char * ) pcWriteBuffer, ( char * ) pcDistanceVerboseMsg, h08r6_range);

  /* There is no more data to return after this single string, so return pdFALSE. */
  return pdFALSE;
}

/*-----------------------------------------------------------*/

/************************ (C) COPYRIGHT HEXABITZ *****END OF FILE****/
