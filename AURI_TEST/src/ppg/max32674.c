/****************************************Copyright (c)************************************************
** File Name:			    max32674.c
** Descriptions:			PPG process source file
** Created By:				xie biao
** Created Date:			2021-05-19
** Modified Date:      		2021-05-19 
** Version:			    	V1.0
******************************************************************************************************/
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <zephyr.h>
#include <soc.h>
#include <device.h>
#include <nrf_socket.h>
#include <drivers/gpio.h>
#include <drivers/i2c.h>
#include <logging/log.h>
#include <nrfx.h>
#include "Max32674.h"
#include "screen.h"
#include "max_sh_interface.h"
#include "max_sh_api.h"
#include "logger.h"

#define PPG_DEBUG

bool ppg_int_event = false;
bool ppg_fw_upgrade_flag = false;
bool ppg_start_flag = false;
bool ppg_test_flag = false;
bool ppg_stop_flag = false;
bool ppg_get_hr_spo2 = false;
bool ppg_redraw_data_flag = false;
u8_t ppg_power_flag = 0;	//0:关闭 1:正在启动 2:启动成功
static u8_t whoamI=0, rst=0;

u8_t g_ppg_trigger = 0;
u16_t g_hr = 0;
u16_t g_spo2 = 0;

static void ppg_auto_stop_timerout(struct k_timer *timer_id);
K_TIMER_DEFINE(ppg_stop_timer, ppg_auto_stop_timerout, NULL);
static void ppg_get_hr_timerout(struct k_timer *timer_id);
K_TIMER_DEFINE(ppg_get_hr_timer, ppg_get_hr_timerout, NULL);

void GetHeartRate(u8_t *HR)
{
	u32_t heart;

	while(1)
	{
		heart = sys_rand32_get();
		if(((heart%200)>=60) && ((heart%200)<=160))
		{
			*HR = (heart%200);
			break;
		}
	}
}

void GetPPGData(u8_t *hr, u8_t *spo2, u8_t *systolic, u8_t *diastolic)
{
	if(hr != NULL)
		*hr = g_hr;
	
	if(spo2 != NULL)
		*spo2 = g_spo2;
	
	if(systolic != NULL)
		*systolic = 120;
	
	if(diastolic != NULL)
		*diastolic = 80;
}

bool PPGIsWorking(void)
{
	if(ppg_power_flag == 0)
		return false;
	else
		return true;
}

void PPGRedrawData(void)
{
	if(screen_id == SCREEN_ID_IDLE)
	{
		scr_msg[screen_id].para |= SCREEN_EVENT_UPDATE_HEALTH;
		scr_msg[screen_id].act = SCREEN_ACTION_UPDATE;
	}
	else if(screen_id == SCREEN_ID_HR)
	{
		scr_msg[screen_id].act = SCREEN_ACTION_UPDATE;
	}
}

void ppg_get_hr_timerout(struct k_timer *timer_id)
{
	ppg_get_hr_spo2 = true;
}

void PPGGetSensorHubData(void)
{
	int ret = 0;
	int num_bytes_to_read = 0;
	u8_t hubStatus = 0;
	u8_t databuf[READ_SAMPLE_COUNT_MAX*(SS_NORMAL_PACKAGE_SIZE+2)] = {0};
	whrm_wspo2_suite_sensorhub_data sensorhub_out = {0};
	accel_data accel = {0};
	max86176_data max86176 = {0};

	ret = sh_get_sensorhub_status(&hubStatus);
#ifdef PPG_DEBUG	
	LOGD("ret:%d, hubStatus:%d", ret, hubStatus);
#endif
	if(hubStatus & SS_MASK_STATUS_FIFO_OUT_OVR)
	{
	#ifdef PPG_DEBUG
		LOGD("SS_MASK_STATUS_FIFO_OUT_OVR");
	#endif
	}

	if((0 == ret) && (hubStatus & SS_MASK_STATUS_DATA_RDY))
	{
		int u32_sampleCnt = 0;

	#ifdef PPG_DEBUG
		LOGD("FIFO ready");
	#endif
		num_bytes_to_read = SS_PACKET_COUNTERSIZE + SSMAX86176_MODE1_DATASIZE + SSACCEL_MODE1_DATASIZE + SSWHRM_WSPO2_SUITE_MODE1_DATASIZE;

		ret = sensorhub_get_output_sample_number(&u32_sampleCnt);
		if(ret == SS_SUCCESS)
		{
		#ifdef PPG_DEBUG
			LOGD("sample count is:%d", u32_sampleCnt);
		#endif
		}
		else
		{
		#ifdef PPG_DEBUG
			LOGD("read sample count fail:%d", ret);
		#endif
		}

		WAIT_MS(5);

		if(u32_sampleCnt > READ_SAMPLE_COUNT_MAX)
			u32_sampleCnt = READ_SAMPLE_COUNT_MAX;
		
		ret = sh_read_fifo_data(u32_sampleCnt, num_bytes_to_read, databuf, sizeof(databuf));
		if(ret == SS_SUCCESS)
		{
			u16_t heart_rate=0,spo2=0;
			u32_t i,j,index = 0;

			for(i=0,j=0;i<u32_sampleCnt;i++)
			{
				index = i * SS_NORMAL_PACKAGE_SIZE + 1;
				
				accel_data_rx(&accel, &databuf[index+SS_PACKET_COUNTERSIZE]);
				max86176_data_rx(&max86176, &databuf[index+SS_PACKET_COUNTERSIZE + SSACCEL_MODE1_DATASIZE]);
				whrm_wspo2_suite_data_rx_mode1(&sensorhub_out, &databuf[index+SS_PACKET_COUNTERSIZE + SSMAX86176_MODE1_DATASIZE + SSACCEL_MODE1_DATASIZE] );
			#ifdef PPG_DEBUG
				LOGD("skin:%d, hr:%d, spo2:%d", sensorhub_out.scd_contact_state, sensorhub_out.hr, sensorhub_out.spo2);
			#endif
				//if(sensorhub_out.scd_contact_state == 3)	//Skin contact state:0: Undetected 1: Off skin 2: On some subject 3: On skin
				{
					heart_rate += sensorhub_out.hr;
					spo2 += sensorhub_out.spo2;
					j++;
				}				
			}

			if(j > 0)
			{
				heart_rate = heart_rate/j;
				spo2 = spo2/j;
			}
			
			g_hr = heart_rate/10 + ((heart_rate%10 > 4) ? 1 : 0);
			g_spo2 = spo2/10 + ((spo2%10 > 4) ? 1 : 0);
		#ifdef PPG_DEBUG
			LOGD("avra hr:%d, spo2:%d", heart_rate, spo2);
			LOGD("g_hr:%d, g_spo2:%d", g_hr, g_spo2);
		#endif	
		}
		else
		{
		#ifdef PPG_DEBUG
			LOGD("read FIFO result fail:%d", ret);
		#endif
		}
	}
	else
	{
	#ifdef PPG_DEBUG
		LOGD("FIFO status is not ready:%d,%d", ret, hubStatus);
	#endif
	}	
}

bool demoSensorhub(void)
{
	int status = -1;
	u8_t hubMode = 0x00;

	SH_rst_to_APP_mode();
	
	status = sh_get_sensorhub_operating_mode(&hubMode);
	if((hubMode != 0x00) && (status != SS_SUCCESS))
	{
	#ifdef PPG_DEBUG
		LOGD("work mode is not app mode:%d", hubMode);
	#endif
		return false;
	}
	else
	{
	#ifdef PPG_DEBUG
		LOGD("work mode is app mode:%d", hubMode);
	#endif
	}

	status = sh_set_data_type(SS_DATATYPE_BOTH, true);
	if(status != SS_SUCCESS)
	{
	#ifdef PPG_DEBUG
		LOGD("sh_set_data_type error:%d", status);
	#endif
		return false;
	}

	status = sh_set_fifo_thresh(1);
	if(status != SS_SUCCESS)
	{
	#ifdef PPG_DEBUG
		LOGD("sh_set_fifo_thresh error:%d", status);
	#endif
		return false;
	}

	status = sh_set_report_period(25);  //1Hz or 25Hz
	if(status != SS_SUCCESS)
	{
	#ifdef PPG_DEBUG
		LOGD("sh_set_fifo_thresh error:%d", status);
	#endif
		return false;
	}

	status = sh_sensor_enable_(SH_SENSORIDX_ACCEL, 1, SH_INPUT_DATA_DIRECT_SENSOR);
	if (status != SS_SUCCESS)
	{
	#ifdef PPG_DEBUG
		LOGD("sh_sensor_enable_ACC eorr:%d", status);
	#endif
		return false;
	}

	status = sh_sensor_enable_(SH_SENSORIDX_MAX86176, 1, SH_INPUT_DATA_DIRECT_SENSOR);
	if(status != SS_SUCCESS)
	{
	#ifdef PPG_DEBUG
		LOGD("sh_sensor_enable_MAX86176 error:%d", status);
	#endif
		status = sh_sensor_disable(SH_SENSORIDX_ACCEL);
		if(status != SS_SUCCESS)
		{
		#ifdef PPG_DEBUG
			LOGD("sh_sensor_disable_ACC error:%d", status);
		#endif
		}
		
		return false;
	}

	//set algorithm operation mode
	sh_set_cfg_wearablesuite_algomode(0x0);

	status = sh_enable_algo_(SS_ALGOIDX_WHRM_WSPO2_SUITE_OS6X , (int) SENSORHUB_MODE_BASIC);
	if(status != SS_SUCCESS)
	{
	#ifdef PPG_DEBUG
		LOGD("sh_sensor_enable_algo error:%d", status);
	#endif
		status = sh_sensor_disable(SH_SENSORIDX_MAX86176);
		if(status != SS_SUCCESS)
		{
		#ifdef PPG_DEBUG
			LOGD("sh_sensor_disble_MAX86176 error:%d", status);
		#endif
		}

		status = sh_sensor_disable(SH_SENSORIDX_ACCEL);
		if(status != SS_SUCCESS)
		{
		#ifdef PPG_DEBUG
			LOGD("sh_sensor_disable_ACC error:%d", status);
		#endif
		}
		
		return false;
	}

	//config a timer to poll FIFO
	//set timer to 5 seconds
	k_timer_start(&ppg_get_hr_timer, K_MSEC(1*1000), K_MSEC(1*1000));

	return true;
}

void APPStartPPG(void)
{
	ppg_start_flag = true;
}

void MenuStartPPG(void)
{
	g_ppg_trigger |= PPG_TRIGGER_BY_MENU;
	ppg_start_flag = true;
}

void MenuStopPPG(void)
{
	ppg_stop_flag = true;
}

void PPGStartCheck(void)
{
	bool ret = false;

#ifdef PPG_DEBUG
	LOGD("ppg_power_flag:%d", ppg_power_flag);
#endif
	if(ppg_power_flag > 0)
		return;

	//Set_PPG_Power_On();
	SH_Power_On();

	ppg_power_flag = 1;
	
	ret = demoSensorhub();
	if(ret)
	{
		if(ppg_power_flag == 0)
		{
		#ifdef PPG_DEBUG
			LOGD("ppg has been stop!");
		#endif
			k_timer_stop(&ppg_get_hr_timer);
			return;
		}
	#ifdef PPG_DEBUG	
		LOGD("ppg start success!");
	#endif
		ppg_power_flag = 2;
		
		if((g_ppg_trigger&PPG_TRIGGER_BY_MENU) == 0)
			k_timer_start(&ppg_stop_timer, K_MSEC(60*1000), NULL);
	}
	else
	{
	#ifdef PPG_DEBUG
		LOGD("ppg start false!");
	#endif
		ppg_power_flag = 0;
	}
}

void PPGStopCheck(void)
{
	int status = -1;
#ifdef PPG_DEBUG
	LOGD("ppg_power_flag:%d", ppg_power_flag);
#endif
	if(ppg_power_flag == 0)
		return;

	k_timer_stop(&ppg_get_hr_timer);
		
	status = sh_sensor_disable(SH_SENSORIDX_MAX86176);
	if(status != SS_SUCCESS)
	{
	#ifdef PPG_DEBUG
		LOGD("sh_sensor_disble_MAX86176 error %d", status);
	#endif
	}

	status = sh_sensor_disable(SH_SENSORIDX_ACCEL);
	if(status != SS_SUCCESS)
	{
	#ifdef PPG_DEBUG
		LOGD("sh_sensor_disable_ACC error %d", status);
	#endif
	}

	status = sh_disable_algo(SS_ALGOIDX_WHRM_WSPO2_SUITE_OS6X);
	if(status != SS_SUCCESS)
	{
	#ifdef PPG_DEBUG
		LOGD("sh_sensor_disble_algo error %d", status);
	#endif
	}

	SH_Power_Off();
	//Set_PPG_Power_Off();

	ppg_power_flag = 0;

	if((g_ppg_trigger&PPG_TRIGGER_BY_APP_ONE_KEY) != 0)
	{
		g_ppg_trigger = g_ppg_trigger&(~PPG_TRIGGER_BY_APP_ONE_KEY);
		MCU_send_app_one_key_measure_data();
	}
	if((g_ppg_trigger&PPG_TRIGGER_BY_APP_HR) != 0)
	{
		g_ppg_trigger = g_ppg_trigger&(~PPG_TRIGGER_BY_APP_HR);
		MCU_send_app_get_hr_data();
	}	
	if((g_ppg_trigger&PPG_TRIGGER_BY_MENU) != 0)
	{
		g_ppg_trigger = g_ppg_trigger&(~PPG_TRIGGER_BY_MENU);
	}

	if((g_ppg_trigger&PPG_TRIGGER_BY_HOURLY) != 0)
	{
		g_ppg_trigger = g_ppg_trigger&(~PPG_TRIGGER_BY_HOURLY);
	}
}

void ppg_auto_stop_timerout(struct k_timer *timer_id)
{
	if((g_ppg_trigger&PPG_TRIGGER_BY_MENU) == 0)
		ppg_stop_flag = true;
}

void PPG_init(void)
{
#ifdef PPG_DEBUG
	LOGD("PPG_init");
#endif

	if(!sh_init_interface())
		return;

#ifdef PPG_DEBUG	
	LOGD("PPG_init done!");
#endif
}

void PPGMsgProcess(void)
{
	if(ppg_int_event)
	{
		ppg_int_event = false;
	}
	if(ppg_fw_upgrade_flag)
	{
		SH_OTA_upgrade_process();
		ppg_fw_upgrade_flag = false;
	}
	if(ppg_start_flag)
	{
	#ifdef PPG_DEBUG
		LOGD("PPG start!");
	#endif
		PPGStartCheck();
		ppg_start_flag = false;
	}
	if(ppg_stop_flag)
	{
	#ifdef PPG_DEBUG
		LOGD("PPG stop!");
	#endif
		PPGStopCheck();
		ppg_stop_flag = false;
	}
	if(ppg_get_hr_spo2)
	{
		PPGGetSensorHubData();
		ppg_get_hr_spo2 = false;
		ppg_redraw_data_flag = true;
	}
	if(ppg_redraw_data_flag)
	{
		PPGRedrawData();
		ppg_redraw_data_flag = false;
	}
}

