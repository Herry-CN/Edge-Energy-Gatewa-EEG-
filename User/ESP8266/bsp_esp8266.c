#include "./ESP8266/bsp_esp8266.h"
#include "./ESP8266/bsp_esp8266_mqtt.h"
#include "./common/common.h"
#include <stdio.h>  
#include <string.h>  
#include <stdbool.h>
#include "./dwt_delay/core_delay.h"
#include "./wdg/bsp_iwdg.h"
#include "./led/bsp_led.h" 

static void                   ESP8266_GPIO_Config                 ( void );
static void                   ESP8266_USART_Config                ( void );
static void                   ESP8266_USART_NVIC_Configuration    ( void );

struct  STRUCT_USARTx_Fram strEsp8266_Fram_Record = { 0 };
struct  STRUCT_USARTx_Fram strUSART_Fram_Record = { 0 };

UART_HandleTypeDef Uart3Handle;

/* Grace period after a reply keyword was matched, so the tail of the frame (and
 * any URC the module appends right behind it) lands before the caller fires the
 * next command. Still far cheaper than waiting out the full timeout. */
#define macESP8266_AT_SETTLE_MS     20

/* Set while ESP8266_Cmd() is waiting for a reply. The USART3 ISR reads it to
 * tell "bytes somebody asked for" from "unsolicited traffic nobody is holding". */
static volatile uint8_t s_at_cmd_in_flight = 0;

volatile uint32_t g_esp8266_rx_drop = 0;

/*
 * Reset the AT response buffer with interrupts masked.
 * FramLength and FramFinishFlag are bitfields sharing one 16-bit word, so a
 * plain assignment from thread mode is a read-modify-write: if the USART3 ISR
 * stores a byte in the middle of it, the write-back silently reverts the
 * length the ISR just advanced.
 */
void ESP8266_ATFrame_Reset ( void )
{
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    /* Bank any downlink frame that already arrived in full before the buffer is
     * thrown away: without this, a command that lands just as thread mode is
     * about to send the next AT command gets its +MQTTSUBRECV header wiped and
     * can never be recognised again. Masked here, and otherwise only called
     * from the USART3 ISR, so the harvest never runs re-entrantly. */
    MQTT_RxQueue_Harvest();

    strEsp8266_Fram_Record .InfBit .FramLength     = 0;
    strEsp8266_Fram_Record .InfBit .FramFinishFlag = 0;
    strEsp8266_Fram_Record .Data_RX_BUF [ 0 ]      = '\0';
    MQTT_RxScan_Reset();
    __set_PRIMASK ( primask );
}

bool ESP8266_AT_CmdInFlight ( void )
{
    return s_at_cmd_in_flight ? true : false;
}

/**
  * @brief  ESP8266��ʼ������
  * @param  ��
  * @retval ��
  */
void ESP8266_Init ( void )
{
	ESP8266_GPIO_Config (); 
	
	ESP8266_USART_Config (); 
	
	/* ESP8266上电默认 RST=高 / CH_PD=高 (CH_ENABLE, not DISABLE)
	 * 这样 diagnostic_mode() 也能直接发 AT 通信（不需要业务层手动使能）。
	 * 之前写成 CH_DISABLE() 会让 ESP8266 掉电，导致诊断模式 8 条 AT 全部 RX=[]
	 * (完全收不到字节)。——bug fix 2026. */
	macESP8266_RST_HIGH_LEVEL();
	macESP8266_CH_ENABLE();
	
}


/**
  * @brief  ��ʼ��ESP8266�õ���GPIO����
  * @param  ��
  * @retval ��
  */
static void ESP8266_GPIO_Config ( void )
{
	/*����һ��GPIO_InitTypeDef���͵Ľṹ��*/
	GPIO_InitTypeDef GPIO_InitStructure;


	/* ���� CH_PD ����*/
	macESP8266_CH_PD_CLK_ENABLE(); 
											   
	GPIO_InitStructure.Pin = macESP8266_CH_PD_PIN;	

	GPIO_InitStructure.Mode = GPIO_MODE_OUTPUT_PP;   
   
	GPIO_InitStructure.Speed = GPIO_SPEED_FREQ_HIGH; 

	HAL_GPIO_Init ( macESP8266_CH_PD_PORT, & GPIO_InitStructure );	 

	
	/* ���� RST ����*/
	macESP8266_RST_CLK_ENABLE(); 
											   
	GPIO_InitStructure.Pin = macESP8266_RST_PIN;	

	HAL_GPIO_Init ( macESP8266_RST_PORT, & GPIO_InitStructure );	 


}


/**
  * @brief  ��ʼ��ESP8266�õ��� USART
  * @param  ��
  * @retval ��
  */
static void ESP8266_USART_Config ( void )
{

	
	/* USART3 mode config */
    Uart3Handle.Instance          = macESP8266_USARTx;
    
    Uart3Handle.Init.BaudRate     = macESP8266_USART_BAUD_RATE;
    Uart3Handle.Init.WordLength   = UART_WORDLENGTH_8B;
    Uart3Handle.Init.StopBits     = UART_STOPBITS_1;
    Uart3Handle.Init.Parity       = UART_PARITY_NONE;
    Uart3Handle.Init.Mode         = UART_MODE_TX_RX;
    Uart3Handle.Init.HwFlowCtl    = UART_HWCONTROL_NONE;

    HAL_UART_Init(&Uart3Handle);
	
	
	/* �ж����� */
	__HAL_UART_ENABLE_IT ( &Uart3Handle, USART_IT_RXNE ); //ʹ�ܴ��ڽ����ж� 
	__HAL_UART_ENABLE_IT ( &Uart3Handle, USART_IT_IDLE ); //ʹ�ܴ������߿����ж� 	

	ESP8266_USART_NVIC_Configuration ();
}


/**
  * @brief  ���� ESP8266 USART �� NVIC �ж�
  * @param  ��
  * @retval ��
  */
static void ESP8266_USART_NVIC_Configuration ( void )
{
	HAL_NVIC_SetPriorityGrouping(macNVIC_PriorityGroup_x);
	/* Configure the NVIC Preemption Priority Bits */  
    HAL_NVIC_SetPriority(macESP8266_USART_IRQ,1,0);
    HAL_NVIC_EnableIRQ(macESP8266_USART_IRQ);
}



/*
 * ��������ESP8266_Rst
 * ����  ������WF-ESP8266ģ��
 * ����  ����
 * ����  : ��
 * ����  ���� ESP8266_AT_Test ����
 */
void ESP8266_Rst ( void )
{
	#if 0
	 ESP8266_Cmd ( "AT+RST", "OK", "ready", 2500 );   	
	
	#else
	 macESP8266_RST_LOW_LEVEL();
	 HAL_Delay ( 500 ); 
	 macESP8266_RST_HIGH_LEVEL();
	#endif

}

bool ESP8266_DHCP_CUR ( )
{
	char cCmd [40];

	sprintf ( cCmd, "AT+CWDHCP=1,1");
	
	return ESP8266_Cmd ( cCmd, "OK", NULL, 500 );
	
}

/*
 * ��������ESP8266_Cmd
 * ����  ����WF-ESP8266ģ�鷢��ATָ��
 * ����  ��cmd�������͵�ָ��
 *         reply1��reply2���ڴ�����Ӧ��ΪNULL��������Ӧ������Ϊ���߼���ϵ
 *         waittime���ȴ���Ӧ��ʱ��
 * ����  : 1��ָ��ͳɹ�
 *         0��ָ���ʧ��
 * ����  �����ⲿ����
 */
/*
 * Poll the RX buffer for either keyword instead of unconditionally burning the
 * whole waittime. The ISR keeps Data_RX_BUF NUL-terminated after every byte, so
 * matching can start the moment new data lands and waittime turns into a
 * ceiling rather than a fixed cost: a normal "OK" used to cost 500..5000 ms,
 * now it costs a few ms.
 * Only FramLength is read here - nothing in this loop writes to the shared
 * bitfield word, so it cannot corrupt what the ISR is doing.
 */
static void ESP8266_AT_Poll ( const char * reply1, const char * reply2, uint32_t waittime,
                              bool * hit1, bool * hit2 )
{
    uint32_t start    = HAL_GetTick();
    uint16_t seen_len = 0xFFFF;      /* force one match attempt on entry */

    *hit1 = false;
    *hit2 = false;

    do
    {
        uint16_t len = strEsp8266_Fram_Record .InfBit .FramLength;

        if ( len != seen_len )
        {
            seen_len = len;

            *hit1 = ( reply1 != 0 ) ? ( strstr ( strEsp8266_Fram_Record .Data_RX_BUF, reply1 ) != NULL ) : false;
            *hit2 = ( reply2 != 0 ) ? ( strstr ( strEsp8266_Fram_Record .Data_RX_BUF, reply2 ) != NULL ) : false;

            if ( *hit1 || *hit2 )
            {
                HAL_Delay ( macESP8266_AT_SETTLE_MS );
                break;
            }
        }

        IWDG_Feed();
    }
    while ( ( HAL_GetTick() - start ) < waittime );
}

/*
 * Wait for a keyword that a command already in progress is going to produce,
 * without sending anything and without clearing what has arrived so far.
 * Needed by the two-step AT+MQTTPUBRAW handshake, where the payload is written
 * between the "> " prompt and the final "+MQTTPUB:OK".
 * reply1 = the keyword we want, reply2 = optional failure keyword that ends the
 * wait early. Returns true only when reply1 was seen.
 */
bool ESP8266_AT_WaitFor ( const char * reply1, const char * reply2, uint32_t waittime )
{
    bool hit1;
    bool hit2;

    s_at_cmd_in_flight = 1;
    ESP8266_AT_Poll ( reply1, reply2, waittime, &hit1, &hit2 );
    s_at_cmd_in_flight = 0;

    return hit1;
}

/*
 * Push raw bytes at USART3 with no formatting and no CR/LF terminator: the
 * AT+MQTTPUBRAW payload is length-delimited, so a single extra byte would be
 * published as part of the message (or shift the whole frame).
 * Writes DR directly for the same reason USART_printf() does - HAL_UART_Transmit
 * takes a lock the USART3 ISR must never wait on.
 */
void ESP8266_SendRaw ( const char * data, uint16_t len )
{
    uint16_t i;

    for ( i = 0; i < len; i++ )
    {
        while ( ( macESP8266_USARTx->SR & USART_SR_TXE ) == 0 )
        {
        }
        macESP8266_USARTx->DR = ( uint8_t ) data [ i ];
    }

    while ( ( macESP8266_USARTx->SR & USART_SR_TC ) == 0 )
    {
    }
}

bool ESP8266_Cmd ( char * cmd, char * reply1, char * reply2, uint32_t waittime )
{    
    bool hit1 = false;
    bool hit2 = false;

    /* Drop whatever the module said before this command, then send. */
    ESP8266_ATFrame_Reset();
    s_at_cmd_in_flight = 1;
    macESP8266_Usart ( "%s\r\n", cmd );
    
    if ( ( reply1 == 0 ) && ( reply2 == 0 ) )                      //����Ҫ��������
    {
        s_at_cmd_in_flight = 0;
        return true;
    }
    
    ESP8266_AT_Poll ( reply1, reply2, waittime, &hit1, &hit2 );
    
    s_at_cmd_in_flight = 0;
    
    macPC_Usart ( "[AT RX] %s", strEsp8266_Fram_Record .Data_RX_BUF );

    /* The reply is deliberately left in the buffer: callers such as the health
       check and MQTT_SUB inspect it for URCs like +MQTTDISCONNECTED right after
       this call returns. The next ESP8266_Cmd() clears it atomically. */                             
    if ( ( reply1 != 0 ) && ( reply2 != 0 ) )
        return ( hit1 || hit2 );
    
    else if ( reply1 != 0 )
        return hit1;
    
    else
        return hit2;
	
}


/*
 * ��������ESP8266_AT_Test
 * ����  ����WF-ESP8266ģ�����AT��������
 * ����  ����
 * ����  : ��
 * ����  �����ⲿ����
 */
//void ESP8266_AT_Test ( void )
//{
//	macESP8266_RST_HIGH_LEVEL();
//	
//	HAL_Delay ( 1000 ); 
//	
//	while ( ! ESP8266_Cmd ( "AT", "OK", NULL, 500 ) ) ESP8266_Rst ();  	

//}
bool ESP8266_AT_Test ( void )
{
	char count=0;
	
	macESP8266_RST_HIGH_LEVEL();	
    printf("\r\nAT����.....\r\n");
	HAL_Delay ( 2000 );
	while ( count < 10 )
	{
        printf("\r\nAT���Դ��� %d......\r\n", count);
		if( ESP8266_Cmd ( "AT", "OK", NULL, 500 ) )
        {
            printf("\r\nAT���������ɹ� %d......\r\n", count);
            return 1;
        }
		ESP8266_Rst();
		++ count;
	}
  return 0;
}


/*
 * ��������ESP8266_Net_Mode_Choose
 * ����  ��ѡ��WF-ESP8266ģ��Ĺ���ģʽ
 * ����  ��enumMode������ģʽ
 * ����  : 1��ѡ��ɹ�
 *         0��ѡ��ʧ��
 * ����  �����ⲿ����
 */
bool ESP8266_Net_Mode_Choose ( ENUM_Net_ModeTypeDef enumMode )
{
	switch ( enumMode )
	{
		case STA:
			return ESP8266_Cmd ( "AT+CWMODE=1", "OK", "no change", 2500 ); 
		
	  case AP:
		  return ESP8266_Cmd ( "AT+CWMODE=2", "OK", "no change", 2500 ); 
		
		case STA_AP:
		  return ESP8266_Cmd ( "AT+CWMODE=3", "OK", "no change", 2500 ); 
		
	  default:
		  return false;
  }
	
}


/*
 * ��������ESP8266_JoinAP
 * ����  ��WF-ESP8266ģ�������ⲿWiFi
 * ����  ��pSSID��WiFi�����ַ���
 *       ��pPassWord��WiFi�����ַ���
 * ����  : 1�����ӳɹ�
 *         0������ʧ��
 * ����  �����ⲿ����
 */
bool ESP8266_JoinAP ( char * pSSID, char * pPassWord )
{
	char cCmd [120];

	sprintf ( cCmd, "AT+CWJAP=\"%s\",\"%s\"", pSSID, pPassWord );
	
	return ESP8266_Cmd ( cCmd, "OK", NULL, 5000 );
	
}


/*
 * ��������ESP8266_BuildAP
 * ����  ��WF-ESP8266ģ�鴴��WiFi�ȵ�
 * ����  ��pSSID��WiFi�����ַ���
 *       ��pPassWord��WiFi�����ַ���
 *       ��enunPsdMode��WiFi���ܷ�ʽ�����ַ���
 * ����  : 1�������ɹ�
 *         0������ʧ��
 * ����  �����ⲿ����
 */
bool ESP8266_BuildAP ( char * pSSID, char * pPassWord, ENUM_AP_PsdMode_TypeDef enunPsdMode )
{
	char cCmd [120];

	sprintf ( cCmd, "AT+CWSAP=\"%s\",\"%s\",1,%d", pSSID, pPassWord, enunPsdMode );
	
	return ESP8266_Cmd ( cCmd, "OK", 0, 1000 );
	
}


/*
 * ��������ESP8266_Enable_MultipleId
 * ����  ��WF-ESP8266ģ������������
 * ����  ��enumEnUnvarnishTx�������Ƿ������
 * ����  : 1�����óɹ�
 *         0������ʧ��
 * ����  �����ⲿ����
 */
bool ESP8266_Enable_MultipleId ( FunctionalState enumEnUnvarnishTx )
{
	char cStr [20];
	
	sprintf ( cStr, "AT+CIPMUX=%d", ( enumEnUnvarnishTx ? 1 : 0 ) );
	
	return ESP8266_Cmd ( cStr, "OK", 0, 500 );
	
}


/*
 * ��������ESP8266_Link_Server
 * ����  ��WF-ESP8266ģ�������ⲿ������
 * ����  ��enumE������Э��
 *       ��ip��������IP�ַ���
 *       ��ComNum���������˿��ַ���
 *       ��id��ģ�����ӷ�������ID
 * ����  : 1�����ӳɹ�
 *         0������ʧ��
 * ����  �����ⲿ����
 */
bool ESP8266_Link_Server ( ENUM_NetPro_TypeDef enumE, char * ip, char * ComNum, ENUM_ID_NO_TypeDef id)
{
	char cStr [100] = { 0 }, cCmd [120];

  switch (  enumE )
  {
		case enumTCP:
		  sprintf ( cStr, "\"%s\",\"%s\",%s", "TCP", ip, ComNum );
		  break;
		
		case enumUDP:
		  sprintf ( cStr, "\"%s\",\"%s\",%s", "UDP", ip, ComNum );
		  break;
		
		default:
			break;
  }

  if ( id < 5 )
    sprintf ( cCmd, "AT+CIPSTART=%d,%s", id, cStr);

  else
	  sprintf ( cCmd, "AT+CIPSTART=%s", cStr );

	return ESP8266_Cmd ( cCmd, "OK", "ALREAY CONNECT", 4000 );
	
}


/*
 * ��������ESP8266_StartOrShutServer
 * ����  ��WF-ESP8266ģ�鿪����رշ�����ģʽ
 * ����  ��enumMode������/�ر�
 *       ��pPortNum���������˿ں��ַ���
 *       ��pTimeOver����������ʱʱ���ַ�������λ����
 * ����  : 1�������ɹ�
 *         0������ʧ��
 * ����  �����ⲿ����
 */
bool ESP8266_StartOrShutServer ( FunctionalState enumMode, char * pPortNum, char * pTimeOver )
{
	char cCmd1 [120], cCmd2 [120];

	if ( enumMode )
	{
		sprintf ( cCmd1, "AT+CIPSERVER=%d,%s", 1, pPortNum );
		
		sprintf ( cCmd2, "AT+CIPSTO=%s", pTimeOver );

		return ( ESP8266_Cmd ( cCmd1, "OK", 0, 500 ) &&
						 ESP8266_Cmd ( cCmd2, "OK", 0, 500 ) );
	}
	
	else
	{
		sprintf ( cCmd1, "AT+CIPSERVER=%d,%s", 0, pPortNum );

		return ESP8266_Cmd ( cCmd1, "OK", 0, 500 );
	}
	
}


/*
 * ��������ESP8266_Get_LinkStatus
 * ����  ����ȡ WF-ESP8266 ������״̬�����ʺϵ��˿�ʱʹ��
 * ����  ����
 * ����  : 2�����ip
 *         3����������
 *         3��ʧȥ����
 *         0����ȡ״̬ʧ��
 * ����  �����ⲿ����
 */
uint8_t ESP8266_Get_LinkStatus ( void )
{
	if ( ESP8266_Cmd ( "AT+CIPSTATUS", "OK", 0, 500 ) )
	{
		if ( strstr ( strEsp8266_Fram_Record .Data_RX_BUF, "STATUS:2\r\n" ) )
			return 2;
		
		else if ( strstr ( strEsp8266_Fram_Record .Data_RX_BUF, "STATUS:3\r\n" ) )
			return 3;
		
		else if ( strstr ( strEsp8266_Fram_Record .Data_RX_BUF, "STATUS:4\r\n" ) )
			return 4;		

	}
	
	return 0;
	
}


/*
 * ��������ESP8266_Get_IdLinkStatus
 * ����  ����ȡ WF-ESP8266 �Ķ˿ڣ�Id������״̬�����ʺ϶�˿�ʱʹ��
 * ����  ����
 * ����  : �˿ڣ�Id��������״̬����5λΪ��Чλ���ֱ��ӦId5~0��ĳλ����1����Id���������ӣ�������0����Idδ��������
 * ����  �����ⲿ����
 */
uint8_t ESP8266_Get_IdLinkStatus ( void )
{
	uint8_t ucIdLinkStatus = 0x00;
	
	
	if ( ESP8266_Cmd ( "AT+CIPSTATUS", "OK", 0, 500 ) )
	{
		if ( strstr ( strEsp8266_Fram_Record .Data_RX_BUF, "+CIPSTATUS:0," ) )
			ucIdLinkStatus |= 0x01;
		else 
			ucIdLinkStatus &= ~ 0x01;
		
		if ( strstr ( strEsp8266_Fram_Record .Data_RX_BUF, "+CIPSTATUS:1," ) )
			ucIdLinkStatus |= 0x02;
		else 
			ucIdLinkStatus &= ~ 0x02;
		
		if ( strstr ( strEsp8266_Fram_Record .Data_RX_BUF, "+CIPSTATUS:2," ) )
			ucIdLinkStatus |= 0x04;
		else 
			ucIdLinkStatus &= ~ 0x04;
		
		if ( strstr ( strEsp8266_Fram_Record .Data_RX_BUF, "+CIPSTATUS:3," ) )
			ucIdLinkStatus |= 0x08;
		else 
			ucIdLinkStatus &= ~ 0x08;
		
		if ( strstr ( strEsp8266_Fram_Record .Data_RX_BUF, "+CIPSTATUS:4," ) )
			ucIdLinkStatus |= 0x10;
		else 
			ucIdLinkStatus &= ~ 0x10;	

	}
	
	return ucIdLinkStatus;
	
}


/*
 * ��������ESP8266_Inquire_ApIp
 * ����  ����ȡ F-ESP8266 �� AP IP
 * ����  ��pApIp����� AP IP ��������׵�ַ
 *         ucArrayLength����� AP IP ������ĳ���
 * ����  : 0����ȡʧ��
 *         1����ȡ�ɹ�
 * ����  �����ⲿ����
 */
uint8_t ESP8266_Inquire_ApIp ( char * pApIp, uint8_t ucArrayLength )
{
	char uc;
	
	char * pCh;
	
	
  ESP8266_Cmd ( "AT+CIFSR", "OK", 0, 500 );
	
	pCh = strstr ( strEsp8266_Fram_Record .Data_RX_BUF, "APIP,\"" );
	
	if ( pCh )
		pCh += 6;
	
	else
		return 0;
	
	for ( uc = 0; uc < ucArrayLength; uc ++ )
	{
		pApIp [ uc ] = * ( pCh + uc);
		
		if ( pApIp [ uc ] == '\"' )
		{
			pApIp [ uc ] = '\0';
			break;
		}
		
	}
	
	return 1;
	
}


/*
 * ��������ESP8266_UnvarnishSend
 * ����  ������WF-ESP8266ģ�����͸������
 * ����  ����
 * ����  : 1�����óɹ�
 *         0������ʧ��
 * ����  �����ⲿ����
 */
bool ESP8266_UnvarnishSend ( void )
{
	if ( ! ESP8266_Cmd ( "AT+CIPMODE=1", "OK", 0, 500 ) )
		return false;
	
	return 
	  ESP8266_Cmd ( "AT+CIPSEND", "OK", ">", 500 );
	
}


/*
 * ��������ESP8266_ExitUnvarnishSend
 * ����  ������WF-ESP8266ģ���˳�͸��ģʽ
 * ����  ����
 * ����  : ��
 * ����  �����ⲿ����
 */
void ESP8266_ExitUnvarnishSend ( void )
{
	HAL_Delay ( 1000 );
	
	macESP8266_Usart ( "+++" );
	
	HAL_Delay ( 500 ); 
	
}


/*
 * ��������ESP8266_SendString
 * ����  ��WF-ESP8266ģ�鷢���ַ���
 * ����  ��enumEnUnvarnishTx�������Ƿ���ʹ����͸��ģʽ
 *       ��pStr��Ҫ���͵��ַ���
 *       ��ulStrLength��Ҫ���͵��ַ������ֽ���
 *       ��ucId���ĸ�ID���͵��ַ���
 * ����  : 1�����ͳɹ�
 *         0������ʧ��
 * ����  �����ⲿ����
 */
bool ESP8266_SendString ( FunctionalState enumEnUnvarnishTx, char * pStr, uint32_t ulStrLength, ENUM_ID_NO_TypeDef ucId )
{
    char cStr [20];
    bool bRet = false;
        
    if ( enumEnUnvarnishTx )
    {
        macESP8266_Usart ( "%s", pStr );
        bRet = true;
    }
    
    else
    {
        if ( ucId < 5 )
            sprintf ( cStr, "AT+CIPSEND=%d,%d", ucId, ulStrLength + 2 );
        else
            sprintf ( cStr, "AT+CIPSEND=%d", ulStrLength + 2 );
        
        ESP8266_Cmd ( cStr, "> ", 0, 100 );
        bRet = ESP8266_Cmd ( pStr, "SEND OK", 0, 500 );
    }
    
    return bRet;
}


/*
 * ��������ESP8266_ReceiveString
 * ����  ��WF-ESP8266ģ������ַ���
 * ����  ��enumEnUnvarnishTx�������Ƿ���ʹ����͸��ģʽ
 * ����  : ���յ����ַ����׵�ַ
 * ����  �����ⲿ����
 */
char * ESP8266_ReceiveString ( FunctionalState enumEnUnvarnishTx )
{
	char * pRecStr = 0;
	
	
	strEsp8266_Fram_Record .InfBit .FramLength = 0;
	strEsp8266_Fram_Record .InfBit .FramFinishFlag = 0;
	
	while ( ! strEsp8266_Fram_Record .InfBit .FramFinishFlag );
	strEsp8266_Fram_Record .Data_RX_BUF [ strEsp8266_Fram_Record .InfBit .FramLength ] = '\0';
	
	if ( enumEnUnvarnishTx )
		pRecStr = strEsp8266_Fram_Record .Data_RX_BUF;
	
	else 
	{
		if ( strstr ( strEsp8266_Fram_Record .Data_RX_BUF, "+IPD" ) )
			pRecStr = strEsp8266_Fram_Record .Data_RX_BUF;

	}

	return pRecStr;
	
}
