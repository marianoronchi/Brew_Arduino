//-----------------------------------------------------------------------------
// Created: 22-4-2013 07:26:24
// Author : Emile
// File   : command_interpreter.c
//-----------------------------------------------------------------------------
// $Log$
// Revision 1.3  2013/06/23 09:08:51  Emile
// - Headers added to files
//
//
//-----------------------------------------------------------------------------
#include <string.h>
#include <ctype.h>
#include <util/atomic.h>
#include "command_interpreter.h"
#include "misc.h"
#include "brew_arduino.h"

extern uint8_t    system_mode;       // from Brew_Arduino.c
extern const char *ebrew_revision;   // ebrew CVS revision number
extern uint8_t    gas_non_mod_llimit; 
extern uint8_t    gas_non_mod_hlimit;
extern uint8_t    gas_mod_pwm_llimit;
extern uint8_t    gas_mod_pwm_hlimit;
extern uint8_t    triac_llimit;
extern uint8_t    triac_hlimit;
extern uint8_t    tmr_on_val;           // ON-timer  for PWM to Time-Division signal
extern uint8_t    tmr_off_val;          // OFF-timer for PWM to Time-Division signal

extern ma         lm35_ma;              // Moving Average filter for LM35 Temperature
extern int8_t     lm35_temp;            // LM35 Temperature in �C
extern uint16_t   lm35_frac;            // LM35 Temperature fraction part in E-2 �C

char    rs232_inbuf[USART_BUFLEN]; // buffer for RS232 commands
uint8_t rs232_ptr = 0;             // index in RS232 buffer

/*-----------------------------------------------------------------------------
  Purpose  : Scan all devices on the I2C bus on all channels of the PCA9544
  Variables: ch: the I2C channel number, 0 is the main channel
  Returns  : -
  ---------------------------------------------------------------------------*/
void i2c_scan(uint8_t ch)
{
	char    s[50]; // needed for printing to serial terminal
	uint8_t x = 0;
	int     i;     // Leave this as an int!
	enum i2c_acks retv;
	
	retv = i2c_select_channel(ch);
	if (ch == PCA9544_NOCH)
	sprintf(s,"I2C[-]: ");
	else sprintf(s,"I2C[%1d]: ",ch-PCA9544_CH0);
	xputs(s);
	for (i = 0x00; i < 0xff; i+=2)
	{
		if (i2c_start(i) == I2C_ACK)
		{
			if ((ch == PCA9544_NOCH) || ((ch != PCA9544_NOCH) && (i != PCA9544)))
			{
				sprintf(s,"0x%0x ",i);
				xputs(s);
				x++;
			}
		} // if
		i2c_stop();
	} // for
	if (!x) xputs("no devices detected");
	xputs("\n");
} // i2c_scan()

/*-----------------------------------------------------------------------------
  Purpose  : Process PWM signal for all system-modes
             MODULATING GAS BURNER:     the HEATER bit energizes the gas-valve 
										and the PWM signal is generated by a timer.
		     NON MODULATING GAS BURNER: ON/OFF signal (bit NON_MOD) controlled
			                            by a RELAY that is used to switch 24 VAC.
			 ELECTRICAL HEATING:        The HEATER Triac is switched with a 5 sec.
			                            period, the PWM signal is converted into
										a time-division signal of 100 * 50 msec.
  Variables: pwm: the PWM signal [0%..100%]
  Returns  : -
  ---------------------------------------------------------------------------*/
void process_pwm_signal(uint8_t pwm)
{
	switch (system_mode)
	{
		case GAS_MODULATING: // Modulating gas-burner
							 if (PORTD & HEATER_LED)
							 {   // HEATER is ON
								 if (pwm < gas_mod_pwm_llimit)
								 {   // set HEATER and HEATER_LED off
									 PORTD &= ~(HEATER | HEATER_LED);
								 } // if
								 // else do nothing (hysteresis)
							 } // if
							 else
							 {	 // HEATER is OFF					 
								 if (pwm > gas_mod_pwm_hlimit)
								 {   // set HEATER and HEATER_LED on
									 PORTD |= (HEATER | HEATER_LED);
								 } // if
								 // else do nothing (hysteresis)
							 } // else
							 pwm_write(pwm); // write PWM value to Timer register
		                     break;
							 
		case GAS_NON_MODULATING: // Non-Modulating gas-burner
							 if (PORTD & HEATER_LED)
							 {   // HEATER is ON
								 if (pwm < gas_non_mod_llimit)
								 {   // set RELAY and HEATER_LED off
									 PORTD &= ~(NON_MOD | HEATER_LED);
								 } // if
								 // else do nothing (hysteresis)
							 } // if
							 else
							 {	 // HEATER is OFF
								 if (pwm > gas_non_mod_hlimit)
								 {   // set RELAY and HEATER_LED on
									 PORTD |= (NON_MOD | HEATER_LED);
								 } // if
								 // else do nothing (hysteresis)
							 } // else
		                     break;

		case ELECTRICAL_HEATING: // Electrical heating
		                     ATOMIC_BLOCK(ATOMIC_FORCEON)
							 {  // set values for pwm_2_time() task
								tmr_on_val  = pwm;
								tmr_off_val = 100 - tmr_on_val;
							 } 	// ATOMIC_BLOCK						 
							 break;

		default: // This should not happen
							 break;
	} // switch
} // process_pwm_signal()

/*-----------------------------------------------------------------------------
  Purpose  : Non-blocking RS232 command-handler via the USB port
  Variables: -
  Returns  : [NO_ERR, ERR_CMD, ERR_NUM, ERR_I2C]
  ---------------------------------------------------------------------------*/
uint8_t rs232_command_handler(void)
{
  char    ch;
  static uint8_t cmd_rcvd = 0;
  
  if (!cmd_rcvd && usart_kbhit())
  { // A new character has been received
    ch = tolower(usart_getc()); // get character as lowercase
	switch (ch)
	{
		case '\r': break;
		case '\n': cmd_rcvd  = 1;
		           rs232_inbuf[rs232_ptr] = '\0';
		           rs232_ptr = 0;
				   break;
		default  : rs232_inbuf[rs232_ptr++] = ch;
				   break;
	} // switch
  } // if
  if (cmd_rcvd)
  {
	  cmd_rcvd = 0;
	  return execute_rs232_command(rs232_inbuf);
  } // if
  else return NO_ERR;
} // rs232_command_handler()

/*-----------------------------------------------------------------------------
  Purpose: interpret commands which are received via the USB serial terminal:
   - A0           : Read Analog value: LM35 temperature sensor
   - A1 / A2      : Read Analog value: VHLT / VMLT Volumes
   - A3 / A4      : Read Temperature sensor LM92: THLT / TMLT temperature

   - L0 / L1      : ALIVE Led ON / OFF

   - N0           : System-Mode: 0=Modulating, 1=Non-Modulating, 2=Electrical
     N1 / N2      : Hysteresis Lower-Limit / Upper-Limit for Non-Modulating gas-valve
     N3 / N4      : Hysteresis Lower-Limit / Upper-Limit for Electrical heating

   - P0 / P1      : set Pump OFF / ON

   - S0           : Ebrew hardware revision number
	 S1			  : List value of parameters that can be set with Nx command
	 S2           : List all connected I2C devices  
	 S3           : List all tasks
	 	
   - W0...W100    : PID-output, needed for:
			        - PWM output for modulating gas-valve (N0=0)
				    - Time-Division ON/OFF signal for non-modulating gas-valve (N0=1)
				    - Time-Division ON/OFF signal for Electrical heating-element (N0=2)
   
  Variables: s: the string that contains the command from RS232 serial port 0
  Returns  : [NO_ERR, ERR_CMD, ERR_NUM, ERR_I2C] or ack. value for command
             cmd ack   cmd ack   cmd ack   cmd ack   cmd      ack
			 A0  33     L0  38    N0  43    P0  47   W0..W100 52
			 A1  34     L1  39    N1  44    P1  48   
			 A2  35     M0  40    N2  45    S0  49   
			 A3  36     M1  41    N3  46    S1  50    
			 A4  37     M2  42
  ---------------------------------------------------------------------------*/
uint8_t execute_rs232_command(char *s)
{
   uint8_t  num = atoi(&s[1]); // convert number in command (until space is found)
   uint8_t  num2;               // 2nd number (some commands only) 
   uint8_t  rval = NO_ERR, frac_16, err;
   int16_t  temp;
   uint16_t tmp2;
   char     s2[40], s3[USART_BUFLEN];
   
   strcpy(s3,s); 
   switch (s[0])
   {
	   case 'a': // Read analog (LM35, VHLT, VMLT) + digital (THLT, TMLT) values
			     rval = 33 + num;
				 switch (num)
				 {
				    case 0: // LM35. Processing is done by lm35_task()
							sprintf(s2,"Lm35=%d.%02d\n",lm35_temp,lm35_frac);
							break;
					case 1: // VHLT
							temp = adc_read(num); // 0=LM35, 1=VHLT, 2=VMLT
							sprintf(s2,"Vhlt=%d\n",temp);
							break;
					case 2: // VMLT
							temp = adc_read(num); // 0=LM35, 1=VHLT, 2=VMLT
							sprintf(s2,"Vmlt=%d\n",temp);
							break;
					case 3: // THLT
							temp = lm92_read(THLT, &frac_16, &err); // 0=THLT, 1=TMLT
							if (err)
							{
								sprintf(s2,"Thlt=0.00\n");
								rval = ERR_I2C;
							} // if
							else sprintf(s2,"Thlt=%d.%04d\n",temp,(uint16_t)frac_16*625);
							break;
					case 4: // TMLT
							temp = lm92_read(TMLT, &frac_16, &err); // 0=THLT, 1=TMLT
							if (err)
							{
								sprintf(s2,"Tmlt=0.00\n");
								rval = ERR_I2C;
							} // if							
							else sprintf(s2,"Tmlt=%d.%04d\n",temp,(uint16_t)frac_16*625);
							break;
					default: rval = ERR_NUM;
					         break;
				 } // switch
				 xputs(s2);
			     break;

	   case 'l': // ALIVE-Led
				 if (num > 1) rval = ERR_NUM;
				 else
			  	 {
					 rval = 38 + num;
					 if (num) PORTD |=  ALIVE_LED;
					 else     PORTD &= ~ALIVE_LED;
					 sprintf(s2,"ok%2d\n",rval);
					 xputs(s2);
				 } // else
				 break;

	   case 'n': // Set parameters / variables to a new value
	             if      (num > 4)                          rval = ERR_NUM;
				 else if ((s[2] != ' ') || (strlen(s) < 4)) rval = ERR_CMD;
				 else
				 {
					rval = 43 + num; 
					num2 = atoi(&s[3]); // convert to number
	                switch (num)
				    {
					   case 0:  // Ebrew System-Mode
					            if (num2 > 2) rval = ERR_NUM;
								else 
								{   // One of [GAS_MODULATING, GAS_NON_MODULATING, ELECTRICAL_HEATING]
									system_mode = num2;
								} // else					            
					   case 1:  // non-modulating gas valve: hysteresis lower-limit
					            gas_non_mod_llimit = num2;
				 		        break;
					   case 2:  // non-modulating gas valve: hysteresis upper-limit
					            gas_non_mod_hlimit = num2;
				 		        break;
					   case 3:  // electrical heating: hysteresis lower-limit
					            gas_mod_pwm_llimit = num2;
				 		        break;
					   case 4:  // electrical heating: hysteresis upper-limit
					            gas_mod_pwm_hlimit = num2;
				 		        break;
					   default: break;
				    } // switch
					sprintf(s2,"ok%2d\n",rval);
					xputs(s2);
				 } // if				 
	             break;

	   case 'p': // Pump
	             rval = 47 + num;
				 if (num > 1) rval = ERR_NUM;
				 else 
				 {
					 if (num == 0) PORTD &= ~(PUMP | PUMP_LED);
					 else          PORTD |=  (PUMP | PUMP_LED);
					 sprintf(s2,"ok%2d\n",rval);
					 xputs(s2);
				 }				 
	             break;

	   case 's': // System commands
	             rval = 49 + num;
				 switch (num)
				 {
					 case 0: // Ebrew revision
							 sprintf(s2,"%s\n",ebrew_revision); 
							 xputs(s2); // print CVS revision number
							 break;
					 case 1: // List parameters
							 sprintf(s2,"%01d,%3d,%3d,%3d,%3d\n",system_mode, 
					                    gas_non_mod_llimit, gas_non_mod_hlimit,
					                    gas_mod_pwm_llimit, gas_mod_pwm_hlimit);
							 xputs(s2); // print parameter values
							 break;
					 case 2: // List all I2C devices
					         i2c_scan(PCA9544_NOCH); // Start with main I2C channel
					         i2c_scan(PCA9544_CH0);  // PCA9544 channel 0
					         i2c_scan(PCA9544_CH1);  // PCA9544 channel 1
					         i2c_scan(PCA9544_CH2);  // PCA9544 channel 2
					         i2c_scan(PCA9544_CH3);  // PCA9544 channel 3
							 break;
					 case 3: // List all tasks
							 list_all_tasks(); 
							 break;				 
					 default: rval = ERR_NUM;
							  break;
				 } // switch

	   case 'w': // PWM signal for Modulating Gas-Burner
	             rval = 52 + num;
				 if (num > 100) 
				      rval = ERR_NUM;
				 else 
				 {
					 process_pwm_signal(num);
					 sprintf(s2,"ok%2d\n",rval);
					 xputs(s2);
				 } // else				 
	             break;

	   default: rval = ERR_CMD;
	            break;
   } // switch
   return rval;	
} // execute_rs232_commands()