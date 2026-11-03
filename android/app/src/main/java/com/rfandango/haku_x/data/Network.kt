package com.rfandango.haku_x.data

import android.content.Context
import android.net.ConnectivityManager
import android.net.LinkAddress
import android.net.NetworkCapabilities
import android.net.wifi.WifiManager
import java.net.Inet4Address

/**
 * What the core's network (native/net) needs from Android: the local network's broadcast address,
 * for the LAN mode's system link, and the Wi-Fi multicast lock, without which many phones drop the
 * broadcasts other consoles send while the screen is on as much as off.
 */
object Network {
  /**
   * The local network's IPv4 broadcast address and prefix ("a.b.c.d/bits"), or null (no IPv4
   * network): Wi-Fi's or Ethernet's, not a VPN's (the active network while one carries the
   * default route, its address a host of its own).
   */
  fun broadcastAddress(context: Context): String? {
    val cm = context.getSystemService(ConnectivityManager::class.java) ?: return null
    val link = localIpv4(cm) ?: return null
    val ip = link.address.address
    val bits = link.prefixLength.coerceIn(0, 32)
    val host = if (bits == 32) 0 else (-1 ushr bits)
    val addr = ((ip[0].toInt() and 0xFF) shl 24) or ((ip[1].toInt() and 0xFF) shl 16) or
      ((ip[2].toInt() and 0xFF) shl 8) or (ip[3].toInt() and 0xFF)
    val b = addr or host
    return "${(b ushr 24) and 0xFF}.${(b ushr 16) and 0xFF}.${(b ushr 8) and 0xFF}.${b and 0xFF}/$bits"
  }

  /** This device's IPv4 address on its Wi-Fi or Ethernet network (not a VPN's), or null: nearby system link
   *  tells it, so that consoles on one network find they need no Wi-Fi Direct between them. */
  fun localAddress(context: Context): Inet4Address? =
    context.getSystemService(ConnectivityManager::class.java)?.let { localIpv4(it) }?.address as? Inet4Address

  /** The first IPv4 address of a Wi-Fi or Ethernet network that isn't a VPN, the active one first. */
  @Suppress("DEPRECATION") // allNetworks, deprecated for callbacks: one look as the game starts is enough
  private fun localIpv4(cm: ConnectivityManager): LinkAddress? =
    (listOfNotNull(cm.activeNetwork) + cm.allNetworks).asSequence()
      .filter { n ->
        val caps = cm.getNetworkCapabilities(n) ?: return@filter false
        !caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN) &&
          (caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) ||
            caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET))
      }
      .mapNotNull { n -> cm.getLinkProperties(n)?.linkAddresses?.firstOrNull { it.address is Inet4Address } }
      .firstOrNull()

  /** The Wi-Fi multicast lock, held while a game runs on the LAN; null if there's no Wi-Fi. */
  fun multicastLock(context: Context): WifiManager.MulticastLock? =
    context.applicationContext.getSystemService(WifiManager::class.java)
      ?.createMulticastLock("hakuX system link")
      ?.apply { setReferenceCounted(false); acquire() }
}
