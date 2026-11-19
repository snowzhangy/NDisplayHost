import Foundation

/// Monotonic microsecond clock used for every timestamp on the wire.
///
/// `CLOCK_UPTIME_RAW` is the `mach_absolute_time` time base, which is also CoreMedia's host-time
/// clock (ScreenCaptureKit presentation timestamps, VideoToolbox). Each side uses its own clock;
/// `ClockSync` estimates the offset between them over the control stream (plan §8, one clock domain).
public enum MonoClock {
    @inline(__always) public static func nowMicros() -> UInt64 {
        clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1_000
    }

    @inline(__always) public static func nowNanos() -> UInt64 {
        clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
    }

    /// Sleeps the calling thread until an absolute `nowNanos()` deadline with sub-100 µs precision.
    public static func sleep(untilNanos deadline: UInt64) {
        let now = nowNanos()
        guard deadline > now else { return }
        let delta = deadline - now
        var ts = timespec(tv_sec: Int(delta / 1_000_000_000), tv_nsec: Int(delta % 1_000_000_000))
        nanosleep(&ts, nil)
    }
}
