package com.cemu

object NativeBridge {
    init { System.loadLibrary("cemu") }
    external fun nativeInit()
    external fun nativeReset()
    external fun nativeOnLocation(
        lat: Double, lon: Double, acc: Double, nowMs: Long,
        carWidth: Double, isCar: Int
    ): Int
    external fun nativeGetArea(isCar: Int): Double
    external fun nativeGetDistance(): Double
    external fun nativeGetCount(): Int
    external fun nativeGetTrackJson(): String
    external fun nativeParseNmea(line: String): DoubleArray?
}
