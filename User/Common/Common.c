#include "./common/common.h"
#include "stm32f1xx.h"
#include <stdarg.h>



static char *                 itoa                                ( int value, char * string, int radix );

/*
 * Blocking single byte write straight to the peripheral registers.
 * HAL_UART_Transmit() must not be used here: it takes the per-handle lock, and
 * a receive interrupt that fires while that lock is held cannot read DR, which
 * livelocks the RX ISR against this transmit. Going at the registers directly
 * keeps the two directions independent.
 * It also makes the USARTx argument meaningful again - the old code ignored it
 * and always sent on USART3.
 */
static void USART_SendByte ( USART_TypeDef * USARTx, uint8_t ch )
{
    uint32_t guard = 0;

    /* Bounded spin: one byte needs ~87 us at 115200 baud, so this only ever
       expires when the peripheral is not clocked. Beats hanging forever. */
    while ( ( ( USARTx->SR & USART_SR_TXE ) == 0 ) && ( guard < 200000u ) )
    {
        guard++;
    }

    USARTx->DR = ( uint16_t ) ( ch & 0x01FF );
}

/*
 * 函数名：USART2_printf
 * 描述  ：格式化输出，类似于C库中的printf，但这里没有用到C库
 * 输入  ：-USARTx 串口通道，这里只用到了串口2，即USART2
 *		     -Data   要发送到串口的内容的指针
 *			   -...    其他参数
 * 输出  ：无
 * 返回  ：无 
 * 调用  ：外部调用
 *         典型应用USART2_printf( USART2, "\r\n this is a demo \r\n" );
 *            		 USART2_printf( USART2, "\r\n %d \r\n", i );
 *            		 USART2_printf( USART2, "\r\n %s \r\n", j );
 */
void USART_printf ( USART_TypeDef * USARTx, char * Data, ... )
{
	const char *s;
	int d;   
	char buf[16];
    uint8_t hc=0x0d;
    uint8_t hh=0x0a;
	
	va_list ap;
	va_start(ap, Data);

	while ( * Data != 0 )     // 判断是否到达字符串结束符
	{				                          
		if ( * Data == 0x5c )  //'\'
		{									  
			switch ( *++Data )
			{
				case 'r':							          //回车符
                    USART_SendByte(USARTx, hc);
                    Data ++;
				break;

				case 'n':							          //换行符
				USART_SendByte(USARTx, hh);	
				Data ++;
				break;

				default:
				Data ++;
				break;
			}			 
		}
		
		else if ( * Data == '%')
		{									  //
			switch ( *++Data )
			{				
				case 's':										  //字符串
				s = va_arg(ap, char *);
				
				for ( ; *s; s++) 
				{
					USART_SendByte(USARTx, (uint8_t)*s);
//					while( __HAL_USART_GET_FLAG(&Uart3Handle, USART_FLAG_TXE) == RESET );
				}
				
				Data++;
				
				break;

				case 'd':			
					//十进制
				d = va_arg(ap, int);
				
				itoa(d, buf, 10);
				
				for (s = buf; *s; s++) 
				{
					USART_SendByte(USARTx, (uint8_t)*s);
//					while( __HAL_USART_GET_FLAG(&Uart3Handle, USART_FLAG_TXE) == RESET );
				}
				
				Data++;
				
				break;
				
				default:
				Data++;
				
				break;
				
			}		 
		}
		
		else USART_SendByte(USARTx, (uint8_t)*Data++);
		
//		while ( __HAL_USART_GET_FLAG(&Uart3Handle, USART_FLAG_TXE ) == RESET );
		
	}
}


/*
 * 函数名：itoa
 * 描述  ：将整形数据转换成字符串
 * 输入  ：-radix =10 表示10进制，其他结果为0
 *         -value 要转换的整形数
 *         -buf 转换后的字符串
 *         -radix = 10
 * 输出  ：无
 * 返回  ：无
 * 调用  ：被USART2_printf()调用
 */
static char * itoa( int value, char *string, int radix )
{
	int     i, d;
	int     flag = 0;
	char    *ptr = string;

	/* This implementation only works for decimal numbers. */
	if (radix != 10)
	{
		*ptr = 0;
		return string;
	}

	if (!value)
	{
		*ptr++ = 0x30;
		*ptr = 0;
		return string;
	}

	/* if this is a negative value insert the minus sign. */
	if (value < 0)
	{
		*ptr++ = '-';

		/* Make the value positive. */
		value *= -1;
		
	}

	for (i = 10000; i > 0; i /= 10)
	{
		d = value / i;

		if (d || flag)
		{
			*ptr++ = (char)(d + 0x30);
			value -= (d * i);
			flag = 1;
		}
	}

	/* Null terminate the string. */
	*ptr = 0;

	return string;

} /* NCL_Itoa */



