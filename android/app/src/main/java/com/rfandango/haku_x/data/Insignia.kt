package com.rfandango.haku_x.data

import android.content.Context
import android.net.Uri
import android.util.Log
import com.rfandango.haku_x.BuildConfig
import com.rfandango.haku_x.data.hdd.XboxHdd
import java.io.File
import java.io.IOException
import java.io.RandomAccessFile
import java.net.HttpURLConnection
import java.net.URL

/**
 * Insignia, the Xbox Live revival (insignia.live): what its setup wizard (ui/insignia) gets and checks.
 * Insignia's Setup Assistant registers the console; its dashboard 5960 (the user's files, Dashboard)
 * makes or recovers the account, which goes in the hard disk's config area.
 */
object Insignia {
  private const val TAG = "hakuX-insignia"

  /** The Setup Assistant's latest release, the ISO made for emulators (insignia-live on GitHub). */
  const val SETUP_URL =
    "https://github.com/insignia-live/setup-assistant-release/releases/latest/download/Insignia-Setup.iso"
  const val SITE = "https://insignia.live"
  const val LOGIN = "https://insignia.live/login"

  /** The Setup Assistant, downloaded. */
  fun setupFile(context: Context) = File(context.filesDir, "insignia/Insignia-Setup.iso")

  /** The Setup Assistant to boot: the downloaded one, else the ISO the user chose; null: neither. */
  fun setupUri(context: Context): Uri? {
    val f = setupFile(context)
    if (f.isFile) return Uri.fromFile(f)
    return AppPrefs(context).insigniaSetupUri.takeIf { it.isNotEmpty() }?.let(Uri::parse)
  }

  /** The release page, for a download in the browser when the app's fails. */
  const val SETUP_PAGE = "https://github.com/insignia-live/setup-assistant-release/releases/latest"

  /**
   * One request of the download: the redirects GitHub answers with (its file host's signed URL)
   * followed here, a new connection each, closed after its response; each hop logged, with its time.
   */
  private fun open(url: String): HttpURLConnection {
    var at = url
    repeat(6) { hop ->
      val t0 = System.nanoTime()
      val c = (URL(at).openConnection() as HttpURLConnection).apply {
        connectTimeout = 15_000
        readTimeout = 30_000
        instanceFollowRedirects = false
        setRequestProperty("User-Agent", "hakuX/${BuildConfig.VERSION_NAME} (Android)")
        setRequestProperty("Accept-Encoding", "identity")
        setRequestProperty("Connection", "close")
      }
      val code = c.responseCode
      Log.i(TAG, "setup download hop $hop: ${URL(at).host} answered $code in ${(System.nanoTime() - t0) / 1_000_000} ms")
      val next = c.getHeaderField("Location")
      if (code !in 300..399 || next == null) return c
      c.disconnect()
      at = URL(URL(at), next).toString()
    }
    throw IOException("too many redirects")
  }

  /** The Setup Assistant from its release into setupFile; progress: bytes done of total (0: not known). */
  fun download(context: Context, progress: (Long, Long) -> Unit) {
    val dest = setupFile(context)
    val part = File(dest.parentFile, dest.name + ".part")
    dest.parentFile?.mkdirs()
    val c = try {
      open(SETUP_URL)
    } catch (e: IOException) {
      Log.w(TAG, "setup download: ${e.javaClass.simpleName}: ${e.message}")
      throw e
    }
    try {
      if (c.responseCode != HttpURLConnection.HTTP_OK) throw IOException("GitHub answered ${c.responseCode}")
      val total = c.contentLengthLong.coerceAtLeast(0)
      var done = 0L
      c.inputStream.use { input ->
        part.outputStream().use { out ->
          val buf = ByteArray(1 shl 16)
          while (true) {
            val n = input.read(buf)
            if (n < 0) break
            out.write(buf, 0, n)
            done += n
            progress(done, total)
          }
        }
      }
    } finally {
      c.disconnect()
    }
    if (!isXboxDisc(part)) {
      part.delete()
      throw IOException("the download isn't an Xbox disc image")
    }
    if (!part.renameTo(dest)) throw IOException("can't keep the download")
  }

  /** An XISO: the XDVDFS volume descriptor at 64 KiB. */
  private fun isXboxDisc(f: File): Boolean = runCatching {
    RandomAccessFile(f, "r").use { r ->
      val b = ByteArray(20)
      r.seek(0x10000)
      r.readFully(b)
      String(b, Charsets.US_ASCII) == "MICROSOFT*XBOX*MEDIA"
    }
  }.getOrDefault(false)

  /**
   * The Insignia accounts on the console: their gamertags, from the config area the native core keeps
   * (raw-partition0.bin): one account a sector from 8, its gamertag at +0x18 and its domain at +0x30
   * ("insignia.live").
   */
  fun accounts(hddRoot: File): List<String> {
    val f = XboxHdd.rawHead(hddRoot, 0)
    if (!f.isFile) return emptyList()
    val d = runCatching { f.readBytes() }.getOrNull() ?: return emptyList()
    val out = mutableListOf<String>()
    for (s in 8 until minOf(d.size / 512, 32)) {
      val name = cString(d, s * 512 + 0x18, 16)
      val domain = cString(d, s * 512 + 0x30, 20)
      if (domain.equals("insignia.live", ignoreCase = true) && name.isNotEmpty() && name.all { it in ' '..'~' }) {
        out += name
      }
    }
    return out
  }

  private fun cString(d: ByteArray, off: Int, max: Int): String {
    var n = 0
    while (n < max && off + n < d.size && d[off + n] != 0.toByte()) n++
    return String(d, off, n, Charsets.US_ASCII)
  }
}
