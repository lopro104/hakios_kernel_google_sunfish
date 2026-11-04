/*
 * The temperature sensors of the SM7150 (Pixel 4a), for drivers/power/hakios.thermal.c: 25 zones in two TSENS blocks, whose
 * readings are at the same addresses as the reference phone's (this phone's device tree: tsens_tm_physical 0xc263000 and
 * 0xc265000). Name, block, number, the temperature to slow down at, and the one the vendor gives up at, in millidegrees.
 *
 * Where the numbers come from: bsd/dev/sweet_thermal.h of ios-sunfish, where every one of these was read on this phone and the
 * limits were taken from the device tree. No code here: it only reads, like the driver.
 *
 * Status: the table ran on the phone in the old tree. THIS FILE: compiled only, not yet run.
 */
#include <stdint.h>
#include <sys/hakios.thermal_sensors.h>

const struct hakios_thermal_sensor hakios_thermal_sm7150[HAKIOS_THERMAL_SM7150_SENSORS] = {
	{ "aoss-0", 0xc263000, 0, 100000, 110000 },
	{ "cpu-0-0", 0xc263000, 1, 95000, 110000 },
	{ "cpu-0-1", 0xc263000, 2, 95000, 110000 },
	{ "cpu-0-2", 0xc263000, 3, 95000, 110000 },
	{ "cpu-0-3", 0xc263000, 4, 95000, 110000 },
	{ "cpu-0-4", 0xc263000, 5, 95000, 110000 },
	{ "cpu-0-5", 0xc263000, 6, 95000, 110000 },
	{ "cpuss-0", 0xc263000, 7, 95000, 110000 },
	{ "cpuss-1", 0xc263000, 8, 95000, 110000 },
	{ "cpu-1-0", 0xc263000, 9, 95000, 110000 },
	{ "cpu-1-1", 0xc263000, 10, 95000, 110000 },
	{ "cpu-1-2", 0xc263000, 11, 95000, 110000 },
	{ "cpu-1-3", 0xc263000, 12, 95000, 110000 },
	{ "gpuss-0", 0xc263000, 13, 95000, 110000 },
	{ "gpuss-1", 0xc263000, 14, 95000, 110000 },
	{ "aoss-1", 0xc265000, 0, 100000, 110000 },
	{ "cwlan", 0xc265000, 1, 100000, 110000 },
	{ "audio", 0xc265000, 2, 100000, 110000 },
	{ "ddr", 0xc265000, 3, 100000, 110000 },
	{ "q6-hvx", 0xc265000, 4, 100000, 110000 },
	{ "camera", 0xc265000, 5, 100000, 110000 },
	{ "mdm-core", 0xc265000, 6, 100000, 110000 },
	{ "mdm-dsp", 0xc265000, 7, 100000, 110000 },
	{ "npu", 0xc265000, 8, 100000, 110000 },
	{ "video", 0xc265000, 9, 100000, 110000 },
};
