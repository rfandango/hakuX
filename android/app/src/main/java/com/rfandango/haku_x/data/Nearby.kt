package com.rfandango.haku_x.data

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.net.NetworkInfo
import android.net.wifi.p2p.WifiP2pConfig
import android.net.wifi.p2p.WifiP2pInfo
import android.net.wifi.p2p.WifiP2pManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.os.SystemClock
import android.util.Log
import androidx.core.content.ContextCompat
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.nio.ByteBuffer
import java.security.SecureRandom
import java.util.UUID
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/**
 * Nearby system link: the consoles of devices next to each other, whatever their networks, on one, as a
 * real console's system link has them: on the same Wi-Fi they reach each other through it (the core's
 * LAN, native/net/lan.c, broadcasts there), and when some can't (another network, none: a plane, a car)
 * the devices form a Wi-Fi Direct group, whose network the core is told before it exists (Android's are
 * all 192.168.49.0/24, the owner at .1). Nothing is chosen: each device decides from what it hears, and
 * all decide alike.
 *
 * - Each device says, over Bluetooth LE (Wi-Fi Direct's own discovery isn't answered by every group
 *   owner: a Retroid Pocket Flip 2's wasn't): its ID (random, kept), its Wi-Fi address, whether it needs a
 *   group, and the group it's in (a four-letter code: the group is DIRECT-hX-<code>, its passphrase from
 *   the name).
 * - It tries the others' Wi-Fi addresses (a probe on PROBE_PORT): one that answers is on its network.
 *   One that doesn't (another network, a Wi-Fi that keeps its clients apart, no Wi-Fi) needs a group.
 * - If any device needs a group, there is one, owned by the lowest ID heard; everyone joins it (a console
 *   reached over both networks too: lan.c drops the second copies). An owner that hears a lower ID gives
 *   its group up, and its devices join the new one. No one needs one: none.
 * - Groups are on 5 GHz: a phone on 6 GHz Wi-Fi may not scan 2.4 GHz (a Galaxy S26 didn't).
 *
 * NearbyService runs it in the app's process while games run in the core's.
 */
object Nearby {
  /** The devices linked now (on the Wi-Fi or in the group) and nearby (heard). */
  data class Status(val linked: Int, val nearby: Int, val group: String?)

  private const val TAG = "hakuX-nearby"
  private val SERVICE: ParcelUuid = ParcelUuid(UUID.fromString("6e8a1c2e-5b0f-4c5e-9a7d-68616b755801"))
  const val BROADCAST = "192.168.49.255/24"
  const val OWNER = "192.168.49.1"
  private const val PROBE_PORT = 3076
  private const val TICK_MS = 1_500L
  private const val GONE_MS = 8_000L        // a device unheard that long has gone
  private const val UNREACHED_MS = 6_000L   // its Wi-Fi address unanswered that long: not on ours
  private const val JOIN_TIMEOUT_MS = 25_000L

  private val _status = MutableStateFlow(Status(0, 0, null))
  val status: StateFlow<Status> = _status.asStateFlow()

  private class Peer(val id: Int) {
    var seen = 0L
    var firstSeen = 0L
    var address = 0                         // its Wi-Fi IPv4, 0 none
    var needs = false
    var group: String? = null
    var owner = false
    var answered = 0L                       // its last probe answer
    var probed = 0L
  }

  private enum class Role { NONE, HOSTING, JOINING, JOINED }

  private var app: Context? = null
  private var myId = 0
  private var running = false
  private val handler = Handler(Looper.getMainLooper())
  private val peers = HashMap<Int, Peer>()
  private var role = Role.NONE
  private var code: String? = null          // our group's (hosting) or the one joined
  private var groupUp = false               // hosting: the group exists (said only then)
  private var joinedAt = 0L
  private var p2p: WifiP2pManager? = null
  private var channel: WifiP2pManager.Channel? = null
  private var receiver: BroadcastReceiver? = null
  private var advertised: ByteArray? = null
  private var probeSocket: DatagramSocket? = null
  private var decided = ""                  // the last decision logged
  private var apart = false                 // debug: as if on another Wi-Fi than everyone

  /** Wi-Fi Direct with the config builder (Android 10), and Bluetooth LE to find each other. */
  fun supported(context: Context) = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q &&
    context.packageManager.hasSystemFeature(PackageManager.FEATURE_WIFI_DIRECT) &&
    context.packageManager.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE)

  /** What it asks for: nearby devices (Android 13: Wi-Fi; 12: Bluetooth), the precise location before. */
  fun permissions(): Array<String> = buildList {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) add(Manifest.permission.NEARBY_WIFI_DEVICES)
    else add(Manifest.permission.ACCESS_FINE_LOCATION)
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
      add(Manifest.permission.BLUETOOTH_SCAN)
      add(Manifest.permission.BLUETOOTH_ADVERTISE)
    }
  }.toTypedArray()

  fun permitted(context: Context) = permissions().all {
    ContextCompat.checkSelfPermission(context, it) == PackageManager.PERMISSION_GRANTED
  }

  fun bluetoothOn(context: Context) =
    context.getSystemService(BluetoothManager::class.java)?.adapter?.isEnabled == true

  /** Starts (a game is starting): saying itself, listening, and deciding every TICK_MS. */
  fun start(context: Context) {
    if (running) return
    val c = context.applicationContext
    app = c
    myId = AppPrefs(c).nearbyId
    if (!permitted(c) || !bluetoothOn(c) || !initP2p(c)) {
      Log.i(TAG, "not started: permitted ${permitted(c)}, Bluetooth ${bluetoothOn(c)}")
      return
    }
    running = true
    apart = debugApart()
    peers.clear()
    // a group left over (a game before, ended without the library) goes first
    p2p?.let { m -> channel?.let { ch -> m.cancelConnect(ch, null); m.removeGroup(ch, null) } }
    role = Role.NONE
    code = null
    decided = ""
    startProbes()
    startScan()
    Log.i(TAG, "started, ID %08x".format(myId))
    tick()
  }

  /** Stops: the group left (or closed), the saying and listening ended. */
  fun stop() {
    if (!running) return
    running = false
    handler.removeCallbacksAndMessages(null)
    stopScan()
    advertise(null)
    probeSocket?.close()
    probeSocket = null
    leaveGroup()
    peers.clear()
    _status.value = Status(0, 0, null)
    Log.i(TAG, "stopped")
  }

  // ---------------------------------------------------------------- deciding

  private fun tick() {
    if (!running) return
    val now = SystemClock.elapsedRealtime()
    peers.values.removeAll { now - it.seen > GONE_MS }
    val mine = myAddress()
    for (p in peers.values) {
      if (mine != 0 && p.address != 0 && now - p.probed >= 2_000) probe(p, now)
    }
    // a device not answered on our Wi-Fi (after a while to answer) needs a group with us
    val needs = peers.values.any { p ->
      mine == 0 || p.address == 0 || (now - p.firstSeen > UNREACHED_MS && now - p.answered > UNREACHED_MS)
    }
    val wanted = peers.isNotEmpty() && (needs || peers.values.any { it.needs })
    val owner = (peers.keys + myId).minOrNull()!!
    val decision = peers.values.sortedBy { it.id }.joinToString { p ->
      "%08x %s".format(p.id, if (now - p.answered < UNREACHED_MS) "on our Wi-Fi" else if (p.needs) "needs a group" else "not reached")
    } + if (wanted) "; group, owner %08x".format(owner) else "; no group"
    if (decision != decided) {
      decided = decision
      Log.i(TAG, decision)
    }
    when {
      !wanted -> if (role != Role.NONE) {
        Log.i(TAG, "no group needed: leaving ${code ?: ""}")
        leaveGroup()
      }
      owner == myId -> if (role != Role.HOSTING) {
        if (role != Role.NONE) leaveGroup()
        host()
      }
      else -> {
        val theirs = peers[owner]?.takeIf { it.owner }?.group
        val inIt = (role == Role.JOINED || role == Role.JOINING) && code == theirs
        if (theirs != null && !inIt) {
          if (role != Role.NONE) leaveGroup()
          join(theirs)
        } else if (theirs == null && role == Role.HOSTING) {
          // the owner hasn't its group yet: ours goes, so that its devices come to it
          leaveGroup()
        }
      }
    }
    if (role == Role.JOINING && now - joinedAt > JOIN_TIMEOUT_MS) {
      Log.i(TAG, "DIRECT-hX-$code didn't answer: trying again")
      leaveGroup()
    }
    advertise(beacon(mine, needs))
    val linked = peers.values.count { p ->
      now - p.answered < UNREACHED_MS || (code != null && p.group == code && role.let { it == Role.HOSTING || it == Role.JOINED })
    }
    _status.value = Status(linked, peers.size, code.takeIf { role == Role.HOSTING || role == Role.JOINED })
    handler.postDelayed({ tick() }, TICK_MS)
  }

  /** id 4, Wi-Fi address 4, flags 1 (1: needs a group, 2: owns the one named), the group's code 4. */
  private fun beacon(address: Int, needs: Boolean): ByteArray {
    val owning = role == Role.HOSTING && groupUp
    val flags = (if (needs) 1 else 0) or (if (owning) 2 else 0)
    val g = (code.takeIf { owning || role == Role.JOINED } ?: "\u0000\u0000\u0000\u0000")
    return ByteBuffer.allocate(13).putInt(myId).putInt(address).put(flags.toByte())
      .put(g.toByteArray(Charsets.ISO_8859_1)).array()
  }

  private fun heard(data: ByteArray) {
    if (data.size < 13) return
    val b = ByteBuffer.wrap(data)
    val id = b.int
    if (id == myId || id == 0) return
    val now = SystemClock.elapsedRealtime()
    val p = peers.getOrPut(id) { Peer(id).also { it.firstSeen = now; Log.i(TAG, "heard %08x".format(id)) } }
    p.seen = now
    p.address = b.int
    val flags = b.get().toInt()
    p.needs = flags and 1 != 0
    p.owner = flags and 2 != 0
    val g = ByteArray(4).also { b.get(it) }
    p.group = String(g, Charsets.ISO_8859_1).takeIf { it.matches(Regex("[a-z2-9]{4}")) }
  }

  // ---------------------------------------------------------------- Wi-Fi probes

  /** "hX?" + our ID to a device's Wi-Fi address; it answers "hX!" + its ID. */
  private fun startProbes() {
    val s = runCatching { DatagramSocket(null).apply { reuseAddress = true; bind(InetSocketAddress(PROBE_PORT)) } }
      .getOrElse { Log.i(TAG, "probe port: ${it.message}"); return }
    probeSocket = s
    Thread({
      val buf = ByteArray(16)
      while (!s.isClosed) {
        val pkt = DatagramPacket(buf, buf.size)
        runCatching { s.receive(pkt) }.onFailure { return@Thread }
        if (pkt.length < 7 || buf[0] != 'h'.code.toByte() || buf[1] != 'X'.code.toByte()) continue
        val id = ByteBuffer.wrap(buf, 3, 4).int
        if (buf[2] == '?'.code.toByte()) {
          val answer = ByteBuffer.allocate(7).put("hX!".toByteArray()).putInt(myId).array()
          runCatching { s.send(DatagramPacket(answer, answer.size, pkt.socketAddress)) }
        } else if (buf[2] == '!'.code.toByte() && !apart) {
          handler.post { peers[id]?.answered = SystemClock.elapsedRealtime() }
        }
      }
    }, "hakuX nearby probes").apply { isDaemon = true }.start()
  }

  private fun probe(p: Peer, now: Long) {
    p.probed = now
    val s = probeSocket ?: return
    val to = InetAddress.getByAddress(ByteBuffer.allocate(4).putInt(p.address).array())
    val q = ByteBuffer.allocate(7).put("hX?".toByteArray()).putInt(myId).array()
    Thread { runCatching { s.send(DatagramPacket(q, q.size, to, PROBE_PORT)) } }.start()
  }

  private fun myAddress(): Int =
    app?.let { Network.localAddress(it) }?.address?.let { ByteBuffer.wrap(it).int } ?: 0

  // ---------------------------------------------------------------- Bluetooth

  @SuppressLint("MissingPermission")
  private fun startScan() {
    val scanner = app?.getSystemService(BluetoothManager::class.java)?.adapter?.bluetoothLeScanner ?: return
    runCatching {
      scanner.startScan(listOf(ScanFilter.Builder().setServiceData(SERVICE, byteArrayOf()).build()),
        ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_BALANCED).build(), scan)
    }.onFailure { Log.i(TAG, "Bluetooth scan: ${it.message}") }
  }

  @SuppressLint("MissingPermission")
  private fun stopScan() {
    runCatching { app?.getSystemService(BluetoothManager::class.java)?.adapter?.bluetoothLeScanner?.stopScan(scan) }
  }

  private val scan = object : ScanCallback() {
    override fun onScanResult(callbackType: Int, result: ScanResult) {
      result.scanRecord?.getServiceData(SERVICE)?.let { heard(it) }
    }
  }

  /** Says data (null: nothing), restarting the advertising when it changed. */
  @SuppressLint("MissingPermission")
  private fun advertise(data: ByteArray?) {
    if (data?.contentEquals(advertised) == true) return
    val adv = app?.getSystemService(BluetoothManager::class.java)?.adapter?.bluetoothLeAdvertiser ?: return
    if (advertised != null) runCatching { adv.stopAdvertising(advertising) }
    advertised = data
    if (data == null) return
    runCatching {
      adv.startAdvertising(
        AdvertiseSettings.Builder().setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
          .setConnectable(false).build(),
        AdvertiseData.Builder().addServiceData(SERVICE, data).build(), advertising)
    }.onFailure { Log.i(TAG, "Bluetooth advertising: ${it.message}") }
  }

  private val advertising = object : AdvertiseCallback() {
    override fun onStartFailure(errorCode: Int) {
      Log.i(TAG, "Bluetooth advertising failed ($errorCode)")
    }
  }

  // ---------------------------------------------------------------- Wi-Fi Direct

  private fun initP2p(c: Context): Boolean {
    if (channel != null) return true
    val m = c.getSystemService(WifiP2pManager::class.java) ?: return false
    p2p = m
    channel = m.initialize(c, Looper.getMainLooper()) { channel = null }
    receiver = object : BroadcastReceiver() {
      override fun onReceive(ctx: Context, intent: Intent) {
        if (intent.action == WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION) connectionChanged(intent)
      }
    }.also {
      ContextCompat.registerReceiver(c, it, IntentFilter(WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION),
        ContextCompat.RECEIVER_NOT_EXPORTED)
    }
    return true
  }

  @SuppressLint("MissingPermission")
  private fun host() {
    val m = p2p ?: return
    val ch = channel ?: return
    val c = token()
    val name = "DIRECT-hX-$c"
    role = Role.HOSTING
    code = c
    groupUp = false
    m.removeGroup(ch, null)
    handler.postDelayed({
      if (role != Role.HOSTING || code != c) return@postDelayed
      m.createGroup(ch, WifiP2pConfig.Builder().setNetworkName(name).setPassphrase(passphraseOf(name))
        .setGroupOperatingBand(WifiP2pConfig.GROUP_OWNER_BAND_5GHZ).build(),
        object : WifiP2pManager.ActionListener {
          override fun onSuccess() {
            Log.i(TAG, "hosting $name")
            if (code == c) groupUp = true
          }
          override fun onFailure(reason: Int) {
            Log.i(TAG, "hosting $name failed ($reason)")
            if (code == c) { role = Role.NONE; code = null }
          }
        })
    }, 300)
  }

  @SuppressLint("MissingPermission")
  private fun join(c: String) {
    val m = p2p ?: return
    val ch = channel ?: return
    val name = "DIRECT-hX-$c"
    role = Role.JOINING
    code = c
    joinedAt = SystemClock.elapsedRealtime()
    Log.i(TAG, "joining $name")
    m.connect(ch, WifiP2pConfig.Builder().setNetworkName(name).setPassphrase(passphraseOf(name)).build(), null)
  }

  private fun leaveGroup() {
    val m = p2p
    val ch = channel
    if (m != null && ch != null && role != Role.NONE) {
      m.cancelConnect(ch, null)
      m.removeGroup(ch, null)
    }
    role = Role.NONE
    code = null
    groupUp = false
  }

  private fun connectionChanged(intent: Intent) {
    @Suppress("DEPRECATION")
    val net = intent.getParcelableExtra<NetworkInfo>(WifiP2pManager.EXTRA_NETWORK_INFO)
    val info = intent.getParcelableExtra<WifiP2pInfo>(WifiP2pManager.EXTRA_WIFI_P2P_INFO)
    @Suppress("DEPRECATION")
    val connected = net?.isConnected == true && info?.groupFormed == true
    if (role == Role.JOINING && connected && !info!!.isGroupOwner) {
      Log.i(TAG, "joined DIRECT-hX-$code")
      role = Role.JOINED
    } else if (role == Role.JOINED && !connected) {
      Log.i(TAG, "DIRECT-hX-$code ended")
      role = Role.NONE
      code = null
    }
  }

  /** Debug builds: debug.hakux.nearby_apart=1 ignores the Wi-Fi probes' answers, as if every device were on
   *  another network (testing the Wi-Fi Direct groups on one Wi-Fi). */
  @SuppressLint("PrivateApi")
  private fun debugApart(): Boolean = runCatching {
    Class.forName("android.os.SystemProperties").getMethod("get", String::class.java)
      .invoke(null, "debug.hakux.nearby_apart") == "1"
  }.getOrDefault(false)

  // ---------------------------------------------------------------- names

  /** A group's passphrase, from its name: joining needs only the code Bluetooth carries. Not a secret from
   *  other hakuX devices, as nearby system link joins any of them. */
  fun passphraseOf(network: String): String {
    val mac = Mac.getInstance("HmacSHA256")
    mac.init(SecretKeySpec("hakuX nearby v1".toByteArray(), "HmacSHA256"))
    val h = mac.doFinal(network.toByteArray())
    return String(CharArray(16) { CODE_CHARS[(h[it].toInt() and 0xff) % CODE_CHARS.length] })
  }

  private const val CODE_CHARS = "abcdefghjkmnpqrstuvwxyz23456789"
  private val random = SecureRandom()
  private fun token() = String(CharArray(4) { CODE_CHARS[random.nextInt(CODE_CHARS.length)] })
}
