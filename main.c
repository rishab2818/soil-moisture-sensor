/*	@title Capacitive Soil Moisture Sensor
**
**	@author Robert Quattlebaum <darco@deepdarc.com>
**
**	See http://www.deepdarc.com/soil-moisture-sensor/ for more information.
**
**	@legal
**	Copyright (c) 2011 Robert S. Quattlebaum. All Rights Reserved.
**
**	This package is free software; you can redistribute it and/or
**	modify it under the terms of the GNU General Public License
**	version 2 as published by the Free Software Foundation.
**
**	This package is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
**	General Public License for more details.
**	@endlegal
*/

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <avr/io.h>
#include <avr/eeprom.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <avr/cpufunc.h>
#include <avr/sleep.h>

#include <util/delay.h>
#include <util/crc16.h>

// ----------------------------------------------------------------------------
#pragma mark Build Settings

// TEMPORARY DEVELOPMENT OVERRIDE OF PHYSICAL BUS PROTOCOL.
// THIS MUST BE REMOVED BEFORE THE PROJECT IS OFFICIALLY RELEASED.
#define COMM_PHY_PROTO			COMM_PHY_1WIRE

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION		(0)
#endif

#ifndef CALIBRATED_BITS
#define CALIBRATED_BITS			(10)
#endif

#ifndef COMM_SDA
#define COMM_SDA				(0)		//!< PB0, MOSI
#endif

#ifndef COMM_SCK
#define COMM_SCK				(2)		//!< PB2
#endif

#define COMM_PHY_FxB			(1)		//!< Fox-Bus™
#define COMM_PHY_1WIRE			(2)		//!< 1-Wire® Compatible (DO NOT USE)
#define COMM_PHY_2WIRE			(3)		//!< 2-Wire (SCK/SDA)

#ifndef COMM_PHY_PROTO
#define COMM_PHY_PROTO			COMM_PHY_FxB	//!< Default is Fox-Bus™.
#endif

#ifndef MOIST_DRIVE_PIN
#define MOIST_DRIVE_PIN				(4)		//!< PB4
#endif

#ifndef MOIST_COLLECTOR_PIN
#define MOIST_COLLECTOR_PIN         (3)		//!< PB3
#endif

#ifndef MOIST_FULLY_DRIVE_PULSES
#define MOIST_FULLY_DRIVE_PULSES    (1)
#endif

#ifndef SUPPORT_DEVICE_NAMING
#define SUPPORT_DEVICE_NAMING		(0)		//!< Not yet implemented.
#endif

#ifndef TEMP_VOLT_COMP_NUMERATOR
#define TEMP_VOLT_COMP_NUMERATOR	((uint32_t)7250*16)
#endif

#ifndef TEMP_VOLT_COMP_OFFSET
#define TEMP_VOLT_COMP_OFFSET		(337)
#endif

#ifndef DEVICE_IS_SPACE_CONSTRAINED
#define DEVICE_IS_SPACE_CONSTRAINED (FLASHEND <= 0x3FF)
#endif

#ifndef SUPPORT_CONVERT_INDICATOR
#define SUPPORT_CONVERT_INDICATOR	(1)
#endif

#ifndef USE_WATCHDOG
#define USE_WATCHDOG				!DEVICE_IS_SPACE_CONSTRAINED
#endif

#ifndef SUPPORT_VOLT_READING
#define SUPPORT_VOLT_READING		!DEVICE_IS_SPACE_CONSTRAINED
#endif

#ifndef SUPPORT_TEMP_READING
#define SUPPORT_TEMP_READING		!DEVICE_IS_SPACE_CONSTRAINED
#endif

#ifndef EMULATE_DS18B20
#define EMULATE_DS18B20				!DEVICE_IS_SPACE_CONSTRAINED
#endif

#ifndef DO_MEDIAN_FILTERING
#define DO_MEDIAN_FILTERING			!DEVICE_IS_SPACE_CONSTRAINED
#endif

#ifndef DO_CALIBRATION
#define DO_CALIBRATION				!DEVICE_IS_SPACE_CONSTRAINED
#endif

// ----------------------------------------------------------------------------
#pragma mark -
#pragma mark Helper Macros

#define MOIST_MAX_VALUE \
        (const uint16_t)((1l << \
                (sizeof(uint16_t) * 8l)) - 1)

#if !defined(TIMSK0) && defined(TIMSK)
#define TIMSK0 TIMSK
#endif

#define sbi(x, y)		x |= (uint8_t)(1 << y)		//!< Set bit
#define cbi(x, y)		x &= (uint8_t) ~(1 << y)	//!< Clear bit

#define ATTR_NO_INIT   __attribute__ ((section(".noinit")))

#ifndef WDTO_MAX
#if defined(__AVR_ATtiny13__) || defined (__AVR_ATtiny13A__)
#define WDTO_MAX WDTO_2S
#else
#define WDTO_MAX WDTO_8S
#endif
#endif

#define TEMP_RESOLUTION_MASK                (0x7)
#define OVERSAMPLE_COUNT_EXPONENT_MASK      (0xF)

#define CFG_FLAG_ALARM						(1<<7)
#define CFG_FLAG_ERROR						(1<<6)

typedef uint8_t bool;
#define true (bool)(1)
#define false (bool)(0)

// ----------------------------------------------------------------------------
#pragma mark -
#pragma mark Misc.

#if COMM_PHY_PROTO == COMM_PHY_1WIRE
#define OWSLAVE_T_X					(30)	//!< General 1-Wire® delay period
#endif

#define COMM_FxB_READ_THRESHOLD		(10)

//!	Device Type Codes
enum {
	COMM_TYPE_CUSTOMFLAG = 0x80,
	COMM_TYPE_MOIST      = 0x20|COMM_TYPE_CUSTOMFLAG
};

//!	ROM Commands
enum {
	COMM_ROMCMD_READ=0x33,           // 00110011b
	COMM_ROMCMD_MATCH=0x55,          // 01010101b
	COMM_ROMCMD_SKIP=0xCC,           // 11001100b
	COMM_ROMCMD_SEARCH=0xF0,         // 11110000b
	COMM_ROMCMD_ALARM_SEARCH=0xEC,   // 11101100b
};

//!	Function Commands
enum {
	COMM_FUNCCMD_RD_MEM=0xAA,
	COMM_FUNCCMD_WR_MEM=0x55,
	COMM_FUNCCMD_CONVERT=0x3C,
	COMM_FUNCCMD_COMMIT_MEM=0x48,
	COMM_FUNCCMD_RECALL_MEM=0xB8,
	COMM_FUNCCMD_CONVERT_T=0x44,
	COMM_FUNCCMD_RD_SCRATCH=0xBE,

#if SUPPORT_DEVICE_NAMING
	COMM_FUNCCMD_RD_NAME=0xF1,
	COMM_FUNCCMD_WR_NAME=0xFE,
#endif
};

typedef union {
	struct {
		uint8_t type;
		uint8_t serial[6];
		uint8_t crc;
	} s;
	uint8_t d[8];
} comm_addr_t;

// ----------------------------------------------------------------------------
#pragma mark -
#pragma mark Memory Page Layout

// Page 1 - Values
struct {
	uint16_t	moisture;
	uint16_t	raw;
	int16_t		temp;
	uint16_t	voltage;
} value ATTR_NO_INIT;

// Page 2 - configuration
struct cfg_t {
	uint8_t	alarm_low;
	uint8_t	alarm_high;

	uint8_t	flags;
	uint8_t	reserved[4];

	uint8_t firmware_version;
} cfg ATTR_NO_INIT;

// Page 3 - Calibration
struct calib_t {
	uint8_t range;
	uint8_t offset;
	uint8_t flags;
	int8_t	temp_offset;

	uint8_t reserved[4];

} calib ATTR_NO_INIT;

#if SUPPORT_CONVERT_INDICATOR
register uint8_t was_interrupted __asm__("r3");
#endif

bool convert_error_occured ATTR_NO_INIT;

// ----------------------------------------------------------------------------
#pragma mark -
#pragma mark EEPROM Layout

comm_addr_t comm_addr EEMEM = {
	.s.type		= COMM_TYPE_MOIST,
	.s.serial	= { 0x00, 0x00, 0x00, 0x00, 0x00, 0xff },
	.s.crc		= 0x4D
};

struct cfg_t cfg_eeprom EEMEM = {
	.alarm_low		= 0x00,
	.alarm_high		= 0xFF,
	.flags			= 0x00 | (4&TEMP_RESOLUTION_MASK),
};

struct calib_t calib_eeprom EEMEM = {
	.range			= 0x69,
	.offset			= 0x11,
	.flags			= 0x06,
	.temp_offset	= 0x00,
};

#if SUPPORT_DEVICE_NAMING
char device_name[16] EEMEM = "";
#endif

// ----------------------------------------------------------------------------
#pragma mark -
#pragma mark Misc. Helper Functions

#if DO_MEDIAN_FILTERING
static uint16_t
median_uint16(
	uint16_t a, uint16_t b, uint16_t c
) {
	if(a < c) {
		if(b < a)
			return a;
		else if(c < b)
			return c;
	} else {
		if(a < b)
			return a;
		else if(b < c)
			return c;
	}
	return b;
}
#endif

// ----------------------------------------------------------------------------
#pragma mark -
#pragma mark Other

#if SUPPORT_CONVERT_INDICATOR
static void
comm_begin_busy() {
	sbi(TIMSK0, OCIE0A);
}

static void
comm_end_busy() {
	cbi(DDRB, COMM_SDA);
	cbi(TIMSK0, OCIE0A);
}
#else
#define comm_begin_busy()    do {} while(0)
#define comm_end_busy()  do {} while(0)
#endif

static uint8_t
get_alarm_condition() {
	return (cfg.flags&(CFG_FLAG_ALARM|CFG_FLAG_ERROR))!=0;
}

// This is the general capacitance-reading function.
static uint16_t
moist_calc() {
	uint16_t v;

#if SUPPORT_CONVERT_INDICATOR
again:
#endif

	// Make sure pins are configured.
	cbi(PORTB, MOIST_DRIVE_PIN);
	cbi(PORTB, MOIST_COLLECTOR_PIN);
	sbi(DDRB, MOIST_DRIVE_PIN);
	sbi(DDRB, MOIST_COLLECTOR_PIN);

	// Wait long enough for the sensing capacitor to fully flush.
	_delay_ms(2);

#if SUPPORT_CONVERT_INDICATOR
	was_interrupted = 0;
#else
	cli();
#endif

	cbi(DDRB, MOIST_DRIVE_PIN);
	cbi(DDRB, MOIST_COLLECTOR_PIN);

	for(v = 0;
	        (v != MOIST_MAX_VALUE) &&
	    bit_is_clear(PINB, MOIST_COLLECTOR_PIN);
	    v++
	) {
		// Change the pin state to HIGH.
		sbi(PORTB, MOIST_DRIVE_PIN);

		// Change the output direction to output.
		sbi(DDRB, MOIST_DRIVE_PIN);

		// Change the output direction back to input (Hi-Z).
		cbi(DDRB, MOIST_DRIVE_PIN);

		// Change the pin state back to LOW.
		// If we don't do this, then the built-in pull-up will
		// still be enabled and throw off our readings.
		cbi(PORTB, MOIST_DRIVE_PIN);

#if SUPPORT_CONVERT_INDICATOR
		if(was_interrupted)
			goto again;

		_NOP();
#else
		_delay_us(1);
#endif
	}

	// Turn interrupts back on.
#if !SUPPORT_CONVERT_INDICATOR
	sei();
#endif

	// Pull both lines low to avoid floating inputs
	sbi(DDRB, MOIST_DRIVE_PIN);
	sbi(DDRB, MOIST_COLLECTOR_PIN);

	return v;
}

#if SUPPORT_VOLT_READING
static void
convert_volt() {
#if defined(__AVR_ATtiny13__) || defined (__AVR_ATtiny13A__)
	// Vref=Vcc, Input=PORTB2
	ADMUX = _BV(MUX0);
	cbi(DDRB, 2);
	sbi(PORTB, 2);
#else
	// Vref=Vcc, Input=Vbg
	ADMUX = _BV(MUX3) | _BV(MUX2);
#endif

	// Throw away the first reading.
	sbi(ADCSRA, ADSC);
	_delay_ms(1);
	loop_until_bit_is_clear(ADCSRA, ADSC);

	// Now read the voltage for real.
	sbi(ADCSRA, ADSC);
	loop_until_bit_is_clear(ADCSRA, ADSC);

	value.voltage = ADC;
}
#endif // SUPPORT_VOLT_READING

#if SUPPORT_TEMP_READING
static void
convert_temp() {
	int32_t temp = 0;

	ADMUX = _BV(REFS1) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1) | _BV(MUX0);

	// Throw away the first reading.
	sbi(ADCSRA, ADSC);
	loop_until_bit_is_clear(ADCSRA, ADSC);

	for(uint16_t i = (1 << (4 + (cfg.flags&TEMP_RESOLUTION_MASK))); i; --i) {
		sbi(ADCSRA, ADSC);
		loop_until_bit_is_clear(ADCSRA, ADSC);
		temp += ADC - 270;
	}

	temp >>= (cfg.flags&TEMP_RESOLUTION_MASK);

	temp += calib.temp_offset*2;

#if SUPPORT_VOLT_READING
	// Adjust for changes in voltage.
	temp += TEMP_VOLT_COMP_NUMERATOR/value.voltage - TEMP_VOLT_COMP_OFFSET;
#endif

	value.temp = temp;
}
#endif

static uint16_t
read_moisture() {
	uint16_t ret = 0;

	for(int i = (1 << (calib.flags&OVERSAMPLE_COUNT_EXPONENT_MASK)); i; --i) {
		const uint16_t prev = ret;
		ret += moist_calc();
		if(ret<prev) {
			ret = 0xFFFF;
			goto bail;
		}
	}

bail:

	if(!ret || (ret==0xFFFF))
		convert_error_occured = 1;

	return ret;
}

static void
convert_moisture() {
	uint16_t value_a = 0;

	value_a = read_moisture();

#if DO_MEDIAN_FILTERING
	{
		uint16_t value_b;
		uint16_t value_c;

		_delay_ms(3);

		value_b = read_moisture();

		_delay_ms(3);

		value_c = read_moisture();

		value_a = median_uint16(value_a, value_b, value_c);
	}
#endif

	value.raw = value_a;

#if DO_CALIBRATION
	// Apply calibration
	{
		uint16_t tmp = (calib.offset << (calib.flags&OVERSAMPLE_COUNT_EXPONENT_MASK));

		if(value_a >= tmp)
			value_a -= tmp;
		else
			value_a = 0;

		value_a = ((uint32_t)value_a << CALIBRATED_BITS)
			/ (uint32_t)((uint32_t)calib.range << (calib.flags&OVERSAMPLE_COUNT_EXPONENT_MASK));

		if(value_a > (1<<CALIBRATED_BITS)-1)
			value_a = (1<<CALIBRATED_BITS)-1;

		value_a <<= 16-CALIBRATED_BITS;

		// TODO: Compensate for temperature.
	}
#endif

	value.moisture = value_a;
}

static void
do_convert() {
	comm_begin_busy();
	
	// Clear status flags
	cfg.flags &= ~(CFG_FLAG_ALARM|CFG_FLAG_ERROR);
	cfg.flags |= CFG_FLAG_ERROR;
	convert_error_occured = false;
	
#if !DEVICE_IS_SPACE_CONSTRAINED
	// Set all values to OxFFFF
	uint8_t i = 7;
	do { ((uint8_t*)&value)[i]=0xFF; } while(i--);
#endif

#if SUPPORT_VOLT_READING
	convert_volt();
#endif

#if SUPPORT_TEMP_READING
	convert_temp();
#endif

	convert_moisture();

	{	// Calculate alarm flag.
		uint8_t moist_h = (value.moisture>>8);

		if(	(moist_h > cfg.alarm_high) || (moist_h < cfg.alarm_low) )
			cfg.flags |= CFG_FLAG_ALARM;
	}

	if(!convert_error_occured)
		cfg.flags &= ~CFG_FLAG_ERROR;

	comm_end_busy();
}

static void
do_recall() {
	eeprom_busy_wait();
	eeprom_read_block(
		&cfg,
		&cfg_eeprom,
		sizeof(cfg_eeprom) + sizeof(calib_eeprom)
	);
	cfg.firmware_version = FIRMWARE_VERSION;
}

static void
do_commit() {
#if USE_WATCHDOG
	wdt_reset();
#endif
	eeprom_update_block(
		&cfg,
		&cfg_eeprom,
		sizeof(cfg_eeprom) + sizeof(calib_eeprom)
	);
	eeprom_busy_wait();
}

// ----------------------------------------------------------------------------
#pragma mark -
#pragma mark Bus Communications Functions

static uint8_t
comm_read_bit() {
#if COMM_PHY_PROTO == COMM_PHY_1WIRE
	// Wait for the bus to go idle if it is already low.
	if(bit_is_clear(PINB, COMM_SDA))
		loop_until_bit_is_set(PINB, COMM_SDA);

	// Wait for the slot to open.
	loop_until_bit_is_clear(PINB, COMM_SDA);

	// Wait until we should sample.
	_delay_us(OWSLAVE_T_X);

	// Return the value of the bit.
	return bit_is_set(PINB, COMM_SDA);
#elif COMM_PHY_PROTO == COMM_PHY_FxB
	uint8_t sum=0;
	
	// Wait for the bus to go idle if it is already low.
	if(bit_is_clear(PINB, COMM_SDA))
		loop_until_bit_is_set(PINB, COMM_SDA);

	// Wait for the slot to open.
	loop_until_bit_is_clear(PINB, COMM_SDA);

	// Disable interrupts, so we can make sure we get the timing right.
	cli();

	while(1) {
		if(bit_is_set(PINB, COMM_SDA)) {
			_delay_us(2);
			if(bit_is_set(PINB, COMM_SDA))
				break;
		}
	}

	_delay_us(3);

	for(uint8_t i=0;i<255;i++) {
		if(bit_is_clear(PINB,COMM_SDA)) {
			if(sum++>COMM_FxB_READ_THRESHOLD) {
				// Turn interrupts back on.
				sei();
				return true;
			}
		} else {
			sum = 0;
		}
	}
	
	// Turn interrupts back on.
	sei();
	return 0;
#else
#error COMM_PHY_PROTO not set properly!
#endif
}

static void
comm_write_bit(uint8_t v) {
#if COMM_PHY_PROTO == COMM_PHY_1WIRE
	// Wait for the bus to go idle.
	if(bit_is_clear(PINB, COMM_SDA))
		loop_until_bit_is_set(PINB, COMM_SDA);

	if(v == 0) {
#if SUPPORT_CONVERT_INDICATOR
		comm_begin_busy();
		loop_until_bit_is_clear(PINB, COMM_SDA);
		loop_until_bit_is_set(PINB, COMM_SDA);
		comm_end_busy();
#else
		// Disable interrupts, so we can make sure we get the timing right.
		cli();

		// Wait for the slot to open.
		loop_until_bit_is_clear(PINB, COMM_SDA);

		// Assert our zero bit.
		sbi(DDRB, COMM_SDA);

		// Wait for the master to sample us.
		_delay_us(OWSLAVE_T_X);

		// Return the bus back to idle.
		cbi(DDRB, COMM_SDA);

		// Turn interrupts back on.
		sei();
#endif
	} else {
		// Wait for the slot to open.
		loop_until_bit_is_clear(PINB, COMM_SDA);
	}
#elif COMM_PHY_PROTO == COMM_PHY_FxB
	// Wait for the bus to go idle.
	if(bit_is_clear(PINB, COMM_SDA))
		loop_until_bit_is_set(PINB, COMM_SDA);
	
	if(v == 0) {
		// Disable interrupts, so we can make sure we get the timing right.
		cli();

		// Wait for the slot to open.
		loop_until_bit_is_clear(PINB, COMM_SDA);

		loop_until_bit_is_set(PINB, COMM_SDA);

		_delay_us(5);

		// Assert our zero bit.
		sbi(DDRB, COMM_SDA);

		_delay_us(5);
		
		// Return the bus back to idle.
		cbi(DDRB, COMM_SDA);

		// Turn interrupts back on.
		sei();
	} else {
		loop_until_bit_is_clear(PINB, COMM_SDA);
		loop_until_bit_is_set(PINB, COMM_SDA);
	}
#else
#error COMM_PHY_PROTO not set properly!
#endif
}

static inline void
comm_send_presence() {
#if COMM_PHY_PROTO == COMM_PHY_1WIRE
	// Wait for reset pulse to end.
	while(bit_is_clear(PINB, COMM_SDA)) sleep_cpu();

	// Let the bus idle for 20µSec after the end of the reset pulse.
	_delay_us(20);

	// Send the 80µSec presence pulse.
	sbi(DDRB, COMM_SDA);
	_delay_us(80);
	cbi(DDRB, COMM_SDA);
#elif COMM_PHY_PROTO == COMM_PHY_FxB
	// Wait for reset pulse to end.
	while(bit_is_clear(PINB, COMM_SDA)) sleep_cpu();

	// Send a '0' bit for our presence.
	comm_write_bit(0);
#else
#error COMM_PHY_PROTO not set properly!
#endif
}

static uint8_t
comm_read_byte() {
	// We really DON'T need to initialize this. Honest.
	uint8_t ret;

	for(uint8_t i = 0; i != 8; i++) {
		ret >>= 1;
		if(comm_read_bit())
			sbi(ret, 7);
	}
	return ret;
}

static void
comm_write_byte(uint8_t byte) {
	for(uint8_t i = 0; i != 8; i++) {
		comm_write_bit(byte & 1);
		byte >>= 1;
	}
}

static inline uint16_t
comm_read_word() {
	return comm_read_byte() + (comm_read_byte() << 8);
}

static inline void
comm_write_word(uint16_t x) {
	comm_write_byte(x);
	comm_write_byte(x >> 8);
}

// These next three lines help clean out some,
// but not all, of the C boilerplate cruft. This
// saves a few dozen bytes without affecting behavior.
extern void __do_clear_bss(void) __attribute__ ((naked));
extern void main (void) __attribute__ ((naked)) __attribute__ ((section (".init8")));
void __do_clear_bss() { }

void
main(void) {
	uint8_t cmd;
	uint8_t flags;

	// Stop the timer, if it happens to be running.
	TCCR0B = 0;

	// Only set MOIST_COLLECTOR_PIN and MOIST_DRIVE_PIN to be outputs.
	DDRB = _BV(MOIST_COLLECTOR_PIN) | _BV(MOIST_DRIVE_PIN);

	// All pins other than COMM_SDA, MOIST_COLLECTOR_PIN,
	// and MOIST_DRIVE_PIN are set to HIGH, which turns on
	// the pull-up resistors.
	PORTB = ~(
		_BV(COMM_SDA) | _BV(MOIST_COLLECTOR_PIN) | _BV(MOIST_DRIVE_PIN)
#if COMM_PHY_PROTO == COMM_PHY_2WIRE
		| _BV(COMM_SCK)
#endif
	);

	// Enable overflow interrupt, which is how we detect reset pulses.
	// The interrupt handler for the overflow interrupt isn't actually
	// defined, which means that __bad_interrupt gets called instead.
	// This causes a soft-reset of the device by jumping to address 0x0000.
	TIMSK0 = _BV(TOIE0);

#if SUPPORT_CONVERT_INDICATOR && (COMM_PHY_PROTO==COMM_PHY_1WIRE)
	OCR0A = (uint8_t)((uint32_t)OWSLAVE_T_X * F_CPU / (8l * 1000000l) );
#endif

#if SUPPORT_VOLT_READING || SUPPORT_TEMP_READING
	ADCSRA = _BV(ADEN) | _BV(ADPS2) | _BV(ADPS1) | _BV(ADPS0);
#endif

#if COMM_PHY_PROTO == COMM_PHY_2WIRE
	USICR =
		_BV(USIWM1) | _BV(USIWM0)	// 2-Wire Mode.
		| _BV(USISIE)				// Start-Condition Interrupt Enable.
		| _BV(USICS0) | _BV(USICS1) // External clock, negative edge.
	;
#else
	// Allow the bus pin to generate interrupts.
	sbi(PCMSK, COMM_SDA);

	// Turn on the pin-change interrupt.
	sbi(GIMSK, PCIE);
#endif

#if USE_WATCHDOG
	// Turn on the watchdog with a maximum watchdog timeout period.
	wdt_enable(WDTO_MAX);
#endif

	// Enable interrupts.
	sei();

	// Check to see if this was a hard or soft reset.
	if(MCUSR) {
		// Hard reset. This happens at initial power-up,
		// when a brown-out condition occurs, and when the
		// watchdog timer expires.
		// We don't send a presense-pulse in this case.

		// Reset the MCU status register.
		MCUSR = 0;

#if !USE_WATCHDOG
		// We should always attempt to disable the watchdog if we
		// are not configured to use one. (Advice from the datasheet)
		wdt_disable();
#endif

		// Load our initial settings from EEPROM.
		do_recall();

		goto wait_for_reset;
	}

	// Reset the MCU status register.
	MCUSR = 0;

	comm_send_presence();

#if USE_WATCHDOG
	wdt_reset();
#endif

	// Read ROM command
	cmd = comm_read_byte();
	flags = 0;

	// Interpret what the ROM command means.
	if(cmd == COMM_ROMCMD_MATCH)
		flags = _BV(2);
	else if(cmd == COMM_ROMCMD_READ)
		flags = _BV(0);
	else if((cmd == COMM_ROMCMD_SEARCH)
	    || ((cmd == COMM_ROMCMD_ALARM_SEARCH)
	        && get_alarm_condition()
	    )
	)
		flags = _BV(0) | _BV(1) | _BV(2);
	else if(cmd != COMM_ROMCMD_SKIP)
		goto wait_for_reset;

	// Perfom the ROM command.
	if(flags) {
		for(uint8_t i = 0; i != 8; i++) {
			uint8_t byte = eeprom_read_byte(&comm_addr.d[i]);
			uint8_t j = 8;
			do {
				if(flags & _BV(0))
					comm_write_bit(byte & 1);
				if(flags & _BV(1))
					comm_write_bit((~byte) & 1);
				if(flags & _BV(2))
					if((byte & 1) ^ comm_read_bit())
						goto wait_for_reset;
				byte >>= 1;
			} while(--j);
		}
	}

#if USE_WATCHDOG
	wdt_reset();
#endif

	// Read function command
	cmd = comm_read_byte();

#if SUPPORT_DEVICE_NAMING
	if(cmd == COMM_FUNCCMD_RD_NAME) {
		for(uint8_t i = 0; i != (uint8_t)sizeof(device_name); i++) {
			comm_write_byte(eeprom_read_byte(&device_name[i]));
		}
		comm_write_byte(0x00);
	} else
#endif
	if((cmd == COMM_FUNCCMD_RD_MEM)
	    || (cmd == COMM_FUNCCMD_WR_MEM)
	) {
		// Initialize the CRC by shifting in the command.
		uint16_t crc = _crc16_update(0, cmd);

		// Read in the requested byte address and update the CRC.
		uint8_t i = comm_read_byte();
		crc = _crc16_update(crc, i);
		comm_read_byte();
		crc = _crc16_update(crc, 0);

		while(i < 23) {
			uint8_t byte;

			if(cmd == COMM_FUNCCMD_RD_MEM) {
				// Send the byte to the OW master.
				byte = ((uint8_t*)&value)[i++];
				comm_write_byte(byte);
			} else {
				// receive the byte from the OW master.
				byte = comm_read_byte();
			    ((uint8_t*)&value)[i++] = byte;
			}

			// Update the CRC.
			crc = _crc16_update(crc, byte);

			// Write out the CRC at every 8-byte page boundry.
			if((i & 7) == 0) {
				comm_write_word(crc);
				crc = 0;
			}
		}
	} else if(cmd == COMM_FUNCCMD_CONVERT) {
		comm_read_byte();    // Ignore input select mask
		comm_read_byte();    // Ignore read-out control
		do_convert();
	} else if(cmd == COMM_FUNCCMD_COMMIT_MEM) {
		comm_begin_busy();
		do_commit();
		comm_end_busy();
	} else if(cmd == COMM_FUNCCMD_RECALL_MEM) {
		comm_begin_busy();
		do_recall();
		comm_end_busy();
	} else if(cmd == COMM_FUNCCMD_CONVERT_T) {
		do_convert();
	}
#if EMULATE_DS18B20
	else if(cmd == COMM_FUNCCMD_RD_SCRATCH) {
		uint8_t crc = 0;
		crc = _crc_ibutton_update(crc, ((uint8_t*)&value.temp)[0]);
		comm_write_byte(((uint8_t*)&value.temp)[0]);
		crc = _crc_ibutton_update(crc, ((uint8_t*)&value.temp)[1]);
		comm_write_byte(((uint8_t*)&value.temp)[1]);
		for(uint8_t i = 0; i < 6; i++) {
			crc = _crc_ibutton_update(crc, 0);
			comm_write_byte(0);
		}
		comm_write_byte(crc);
	}
#endif

wait_for_reset:
	for(;;) {
		// Allow the bus pin to generate interrupts.
		sbi(PCMSK, COMM_SDA);

		// Turn on the pin-change interrupt.
		sbi(GIMSK, PCIE);

		// Enable interrupts.
		sei();

#if USE_WATCHDOG
		wdt_reset();
#endif
	}
}

#if (COMM_PHY_PROTO == COMM_PHY_1WIRE) || (COMM_PHY_PROTO == COMM_PHY_FxB)

#if SUPPORT_CONVERT_INDICATOR
ISR(TIM0_COMPA_vect) {
#if COMM_PHY_PROTO == COMM_PHY_1WIRE
	cbi(DDRB, COMM_SDA);
	was_interrupted++;
#elif COMM_PHY_PROTO == COMM_PHY_FxB
	// TODO: Writeme!
#endif
}
#endif

// Pin change interrupt
ISR(PCINT0_vect) {
	TCCR0B = 0; // Stop the timer.

#if SUPPORT_CONVERT_INDICATOR
	was_interrupted++;
#endif

	// Is this a high-to-low transition?
	if(bit_is_clear(PINB, COMM_SDA)) {
#if SUPPORT_CONVERT_INDICATOR
#if COMM_PHY_PROTO == COMM_PHY_1WIRE
		if(bit_is_set(TIMSK0, OCIE0A))
			sbi(DDRB, COMM_SDA);
#elif COMM_PHY_PROTO == COMM_PHY_FxB
	// TODO: Writeme!
#endif
#endif

		TCNT0 = 0; // Reset counter.

		// Start timer with prescaler of 1/8.
		// @9.6MHz: ~.8333µSecconds per tick, 214µSecond reset pulse
		// @8.0MHz: ~1µSecond per tick, 256µSecond reset pulse
		TCCR0B = (1 << 1);
	}
}

#endif

