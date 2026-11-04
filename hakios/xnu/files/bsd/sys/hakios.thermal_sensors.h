/*
 * One temperature sensor, for a table of them outside drivers/power/hakios.thermal.c (a SoC that is not the reference phone's).
 */
#ifndef _SYS_HAKIOS_THERMAL_SENSORS_H_
#define _SYS_HAKIOS_THERMAL_SENSORS_H_

#include <stdint.h>

struct hakios_thermal_sensor {
	const char *    name;
	uint32_t        tm;             /* the TSENS block's readings, by address */
	uint32_t        number;
	int32_t         slow_mc;        /* slow down at, millidegrees */
	int32_t         limit_mc;       /* gives up at */
};

#define HAKIOS_THERMAL_SM7150_SENSORS   25

#endif /* _SYS_HAKIOS_THERMAL_SENSORS_H_ */
