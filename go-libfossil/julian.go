package libfossil

import "time"

const julianEpoch = 2440587.5

func TimeToJulian(t time.Time) float64 {
	return julianEpoch + float64(t.UTC().UnixMilli())/(86400.0*1000.0)
}

func JulianToTime(j float64) time.Time {
	millis := int64((j - julianEpoch) * 86400.0 * 1000.0)
	return time.UnixMilli(millis).UTC()
}
