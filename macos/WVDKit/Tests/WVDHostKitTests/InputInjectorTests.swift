import Testing
@testable import WVDHostKit

@Suite struct InputInjectorTests {
    @Test func scrollDeltaHandlesNonFiniteAndOutOfRangeValues() {
        let cases: [(Float, Int32)] = [
            (.nan, 0), (.signalingNaN, 0), (.infinity, 0), (-.infinity, 0),
            (.greatestFiniteMagnitude, 10_000), (-.greatestFiniteMagnitude, -10_000),
            (Float(Int32.max), 10_000), (Float(Int32.min), -10_000),
            (10_001, 10_000), (-10_001, -10_000), (10_000, 10_000), (-10_000, -10_000),
            (9_999.6, 10_000), (-9_999.6, -10_000),
            (1.4, 1), (-1.4, -1), (1.5, 2), (-1.5, -2),
            (0.49, 0), (-0.49, 0), (0, 0), (-0.0, 0),
        ]
        for (value, expected) in cases {
            #expect(CGEventInjector.wheelDelta(value) == expected, "scroll delta \(value)")
        }
    }
}
