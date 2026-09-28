package com.cemu

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothSocket
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.location.*
import android.os.*
import android.speech.tts.TextToSpeech
import android.view.WindowManager
import android.webkit.*
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import java.io.BufferedReader
import java.io.InputStreamReader
import java.util.*

class MainActivity : AppCompatActivity(), LocationListener {

    private lateinit var webView: WebView
    private lateinit var lm: LocationManager
    private var measuring = false
    private var isCar = true
    private var carWidth = 4.0
    private var qualityLimit = 5.0
    private var lastPushTime = 0L
    private var tts: TextToSpeech? = null
    private var rtkSocket: BluetoothSocket? = null
    private var rtkThread: Thread? = null

    inner class JsBridge {
        @JavascriptInterface
        fun startMeasure(isCarInt: Int, width: Double, qLimit: Double) {
            isCar = isCarInt == 1
            carWidth = width
            qualityLimit = qLimit
            runOnUiThread { startLocation() }
        }

        @JavascriptInterface
        fun stopMeasure() = runOnUiThread { stopLocation() }

        @JavascriptInterface
        fun clear() { NativeBridge.nativeReset() }

        @JavascriptInterface
        fun getArea(isCarInt: Int): Double = NativeBridge.nativeGetArea(isCarInt)
        @JavascriptInterface
        fun getDistance(): Double = NativeBridge.nativeGetDistance()
        @JavascriptInterface
        fun getCount(): Int = NativeBridge.nativeGetCount()
        @JavascriptInterface
        fun getTrackJson(): String = NativeBridge.nativeGetTrackJson()

        @JavascriptInterface
        fun speak(text: String) {
            tts?.speak(text, TextToSpeech.QUEUE_FLUSH, null, null)
        }

        @JavascriptInterface
        fun listBtDevices(): String {
            val adapter = BluetoothAdapter.getDefaultAdapter() ?: return "[]"
            if (ActivityCompat.checkSelfPermission(this@MainActivity, Manifest.permission.BLUETOOTH_CONNECT)
                != PackageManager.PERMISSION_GRANTED &&
                Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) return "[]"
            val paired = adapter.bondedDevices ?: return "[]"
            val sb = StringBuilder("[")
            var first = true
            for (d in paired) {
                if (!first) sb.append(",")
                sb.append("{\"name\":\"").append(d.name ?: "未知")
                  .append("\",\"addr\":\"").append(d.address).append("\"}")
                first = false
            }
            sb.append("]")
            return sb.toString()
        }

        @JavascriptInterface
        fun connectRtk(addr: String) {
            runOnUiThread { connectRtkInternal(addr) }
        }

        @JavascriptInterface
        fun disconnectRtk() {
            runOnUiThread { disconnectRtkInternal() }
        }
    }

    @SuppressLint("SetJavaScriptEnabled", "AddJavascriptInterface")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        NativeBridge.nativeInit()

        webView = WebView(this)
        setContentView(webView)
        webView.settings.apply {
            javaScriptEnabled = true
            domStorageEnabled = true
            databaseEnabled = true
            allowFileAccess = true
            cacheMode = WebSettings.LOAD_DEFAULT
        }
        webView.addJavascriptInterface(JsBridge(), "Android")
        webView.webChromeClient = object : WebChromeClient() {
            override fun onGeolocationPermissionsShowPrompt(
                origin: String?, callback: GeolocationPermissions.Callback?
            ) { callback?.invoke(origin, true, false) }
        }
        webView.webViewClient = WebViewClient()
        webView.loadUrl("file:///android_asset/index.html")

        lm = getSystemService(Context.LOCATION_SERVICE) as LocationManager
        tts = TextToSpeech(this) { }
        requestPerms()
    }

    private fun requestPerms() {
        val need = mutableListOf<String>()
        fun add(p: String) {
            if (ContextCompat.checkSelfPermission(this, p) != PackageManager.PERMISSION_GRANTED)
                need.add(p)
        }
        add(Manifest.permission.ACCESS_FINE_LOCATION)
        add(Manifest.permission.ACCESS_COARSE_LOCATION)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            add(Manifest.permission.BLUETOOTH_CONNECT)
            add(Manifest.permission.BLUETOOTH_SCAN)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            add(Manifest.permission.POST_NOTIFICATIONS)
        }
        if (need.isNotEmpty()) ActivityCompat.requestPermissions(this, need.toTypedArray(), 1)
    }

    @SuppressLint("MissingPermission")
    private fun startLocation() {
        if (measuring) return
        NativeBridge.nativeReset()
        measuring = true
        lastPushTime = 0L
        try {
            startService(Intent(this, GnssService::class.java))
        } catch (e: Exception) { }
        try {
            lm.requestLocationUpdates(LocationManager.GPS_PROVIDER, 100L, 0f, this)
            lm.requestLocationUpdates(LocationManager.NETWORK_PROVIDER, 100L, 0f, this)
        } catch (e: Exception) { }
    }

    private fun stopLocation() {
        if (!measuring) return
        measuring = false
        try { lm.removeUpdates(this) } catch (e: Exception) { }
        try { stopService(Intent(this, GnssService::class.java)) } catch (e: Exception) { }
    }

    private fun connectRtkInternal(addr: String) {
        disconnectRtkInternal()
        try {
            val adapter = BluetoothAdapter.getDefaultAdapter() ?: return
            val dev: BluetoothDevice = adapter.getRemoteDevice(addr)
            val uuid = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB")
            val sock = dev.createRfcommSocketToServiceRecord(uuid)
            sock.connect()
            rtkSocket = sock
            rtkThread = Thread {
                try {
                    val reader = BufferedReader(InputStreamReader(sock.inputStream))
                    while (measuring || rtkSocket != null) {
                        val line = reader.readLine() ?: break
                        if (!line.startsWith("$")) continue
                        val arr = NativeBridge.nativeParseNmea(line) ?: continue
                        if (arr.size >= 4 && arr[0] != 0.0) {
                            val now = SystemClock.elapsedRealtime()
                            val ok = NativeBridge.nativeOnLocation(
                                arr[0], arr[1], arr[3], now, carWidth, if (isCar) 1 else 0
                            )
                            if (ok == 1) pushUi(arr[0], arr[1], arr[3])
                        }
                    }
                } catch (e: Exception) { }
            }.also { it.start() }
        } catch (e: Exception) {
            webView.post { webView.evaluateJavascript("window.onRtkError('${e.message}')", null) }
        }
    }

    private fun disconnectRtkInternal() {
        try { rtkSocket?.close() } catch (e: Exception) { }
        rtkSocket = null
        rtkThread?.interrupt()
        rtkThread = null
    }

    override fun onLocationChanged(loc: Location) {
        if (!measuring) return
        if (rtkSocket != null) return
        val acc = if (loc.hasAccuracy()) loc.accuracy.toDouble() else 999.0
        if (acc > qualityLimit) return
        val now = SystemClock.elapsedRealtime()
        val ok = NativeBridge.nativeOnLocation(
            loc.latitude, loc.longitude, acc, now, carWidth, if (isCar) 1 else 0
        )
        if (ok == 1) pushUi(loc.latitude, loc.longitude, acc)
    }

    private fun pushUi(lat: Double, lon: Double, acc: Double) {
        val now = SystemClock.elapsedRealtime()
        if (now - lastPushTime < 200L) return
        lastPushTime = now
        val area = NativeBridge.nativeGetArea(if (isCar) 1 else 0)
        val dist = NativeBridge.nativeGetDistance()
        val cnt = NativeBridge.nativeGetCount()
        val js = "window.onNativeLocation($lat,$lon,$acc,$area,$dist,$cnt)"
        webView.post { webView.evaluateJavascript(js, null) }
    }

    override fun onProviderDisabled(provider: String) {}
    override fun onProviderEnabled(provider: String) {}
    @Deprecated("deprecated")
    override fun onStatusChanged(provider: String?, status: Int, extras: Bundle?) {}

    override fun onDestroy() {
        super.onDestroy()
        disconnectRtkInternal()
        tts?.shutdown()
    }
}
