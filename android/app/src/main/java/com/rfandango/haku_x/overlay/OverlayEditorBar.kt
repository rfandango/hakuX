package com.rfandango.haku_x.overlay

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.util.TypedValue
import android.view.Gravity
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import com.rfandango.haku_x.OnScreenController
import com.rfandango.haku_x.R
import kotlin.math.roundToInt

/**
 * The layout editor's bar over the controller: the selected control's size
 * and visibility, the controls' opacity, and reset / cancel / done. It folds
 * away into a small tab at the top edge, so controls can go up there too.
 * Its groups wrap onto more rows where the screen is too narrow for one (a
 * 4:3 screen in landscape).
 * Plain views, so it works over the game (the core's SDL activity) as in the
 * app. Place it at the top centre of the screen.
 */
class OverlayEditorBar(
  context: Context,
  private val controller: OnScreenController,
  private val onFinished: () -> Unit,
) : FrameLayout(context) {
  private val density = resources.displayMetrics.density
  private val bar = Rows(context)
  private val tab = TextView(context)
  private val title = label(bold = true)
  private val smaller = action(R.string.overlay_editor_smaller) { controller.resize(1f / STEP) }
  private val size = label()
  private val larger = action(R.string.overlay_editor_larger) { controller.resize(STEP) }
  private val hideShow = action(R.string.overlay_editor_hide) { controller.toggleSelectedVisible() }
  private val opacity = label()

  init {
    val pad = dp(6)
    bar.setPadding(pad * 2, pad, pad, pad)
    bar.background = GradientDrawable().apply {
      cornerRadius = dp(22).toFloat()
      setColor(Color.argb(225, 24, 24, 24))
      setStroke(dp(1), Color.argb(70, 255, 255, 255))
    }
    bar.elevation = dp(6).toFloat()

    bar.addView(group().apply {
      addView(title, LinearLayout.LayoutParams(dp(104), LinearLayout.LayoutParams.WRAP_CONTENT))
      addView(smaller)
      addView(size, LinearLayout.LayoutParams(dp(76), LinearLayout.LayoutParams.WRAP_CONTENT))
      addView(larger)
      addView(hideShow)
    })
    bar.addView(group().apply {
      addView(action(R.string.overlay_editor_smaller) { controller.changeOpacity(-0.1f) })
      addView(opacity, LinearLayout.LayoutParams(dp(112), LinearLayout.LayoutParams.WRAP_CONTENT))
      addView(action(R.string.overlay_editor_larger) { controller.changeOpacity(0.1f) })
    })
    bar.addView(group().apply {
      addView(action(R.string.overlay_editor_reset) { controller.resetLayout() })
      addView(action(R.string.overlay_editor_cancel) {
        controller.reloadLayout()
        onFinished()
      })
      addView(action(R.string.overlay_editor_done, accent = true) {
        controller.saveLayout()
        onFinished()
      })
    })
    bar.addView(action(R.string.overlay_editor_fold) { setFolded(true) }.apply {
      contentDescription = context.getString(R.string.overlay_editor_fold_desc)
    })
    addView(bar, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.CENTER_HORIZONTAL)
      .apply {
        topMargin = dp(12)
        leftMargin = dp(8)
        rightMargin = dp(8)
      })

    // Folded: a tab hanging from the top edge brings the bar back.
    tab.setText(R.string.overlay_editor_tools)
    tab.setTextColor(Color.WHITE)
    tab.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
    tab.typeface = Typeface.DEFAULT_BOLD
    tab.setPadding(dp(18), dp(4), dp(18), dp(8))
    tab.background = GradientDrawable().apply {
      val r = dp(14).toFloat()
      cornerRadii = floatArrayOf(0f, 0f, 0f, 0f, r, r, r, r)
      setColor(Color.argb(200, 24, 24, 24))
      setStroke(dp(1), Color.argb(70, 255, 255, 255))
    }
    tab.isClickable = true
    tab.setOnClickListener { setFolded(false) }
    tab.visibility = GONE
    addView(tab, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.CENTER_HORIZONTAL))

    controller.onSelectionChanged = { refresh() }
    controller.onLayoutChanged = { refresh() }
    refresh()
  }

  private fun refresh() {
    val sel = controller.selectedControl()
    title.text = sel?.title ?: context.getString(R.string.overlay_editor_pick)
    // Size and opacity: the selected control's, or with none selected all of them.
    val scale = controller.scalePercent()
    size.text = when {
      sel != null -> "${scale ?: 100}%"
      scale != null -> context.getString(R.string.overlay_editor_size_all, scale)
      else -> context.getString(R.string.overlay_editor_size_mixed)
    }
    hideShow.isEnabled = sel != null && sel != ControlId.MENU
    hideShow.alpha = if (hideShow.isEnabled) 1f else 0.35f
    hideShow.setText(if (controller.isSelectedVisible()) R.string.overlay_editor_hide else R.string.overlay_editor_show)
    val percent = controller.opacityPercent()
    opacity.text = when {
      sel != null -> context.getString(R.string.overlay_editor_opacity, percent ?: 0)
      percent != null -> context.getString(R.string.overlay_editor_opacity_all, percent)
      else -> context.getString(R.string.overlay_editor_opacity_mixed)
    }
  }

  private fun label(bold: Boolean = false) = TextView(context).apply {
    setTextColor(Color.WHITE)
    setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
    if (bold) typeface = Typeface.DEFAULT_BOLD
    gravity = Gravity.CENTER
    maxLines = 2
  }

  private fun action(text: Int, accent: Boolean = false, onClick: () -> Unit) = TextView(context).apply {
    setText(text)
    setTextColor(if (accent) ACCENT else Color.WHITE)
    setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
    typeface = Typeface.DEFAULT_BOLD
    gravity = Gravity.CENTER
    minWidth = dp(40)
    setPadding(dp(12), dp(10), dp(12), dp(10))
    val mask = GradientDrawable().apply {
      cornerRadius = dp(16).toFloat()
      setColor(Color.WHITE)
    }
    background = RippleDrawable(ColorStateList.valueOf(Color.argb(70, 255, 255, 255)), null, mask)
    isClickable = true
    isFocusable = true
    setOnClickListener { onClick() }
  }

  /** Folds the bar into the tab at the top edge (or opens it again). */
  fun setFolded(folded: Boolean) {
    bar.visibility = if (folded) GONE else VISIBLE
    tab.visibility = if (folded) VISIBLE else GONE
  }

  private fun group() = LinearLayout(context).apply {
    orientation = LinearLayout.HORIZONTAL
    gravity = Gravity.CENTER_VERTICAL
  }

  /**
   * The bar's groups in rows: as many to a row as its width takes, each row
   * centred, a rule between neighbours on a row.
   */
  private inner class Rows(context: Context) : ViewGroup(context) {
    private val gap = dp(13) // a rule and the room either side of it
    private val rule = Paint().apply { color = Color.argb(60, 255, 255, 255) }
    private var rows = IntArray(0) // each child's
    private var rowWidths = IntArray(0)
    private var rowHeights = IntArray(0)

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
      val padX = paddingLeft + paddingRight
      val bounded = MeasureSpec.getMode(widthSpec) != MeasureSpec.UNSPECIFIED
      val room = maxOf(MeasureSpec.getSize(widthSpec) - padX, 0)
      val unspecified = MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED)
      val spec = if (bounded) MeasureSpec.makeMeasureSpec(room, MeasureSpec.AT_MOST) else unspecified
      val widths = IntArray(childCount) {
        getChildAt(it).measure(spec, unspecified)
        getChildAt(it).measuredWidth
      }
      rows = wrapRows(widths, gap, if (bounded) room else Int.MAX_VALUE)
      val count = (rows.lastOrNull() ?: -1) + 1
      rowWidths = IntArray(count)
      rowHeights = IntArray(count)
      for (i in widths.indices) {
        val r = rows[i]
        rowWidths[r] += if (rowWidths[r] > 0) gap + widths[i] else widths[i]
        rowHeights[r] = maxOf(rowHeights[r], getChildAt(i).measuredHeight)
      }
      setMeasuredDimension(
        resolveSize((rowWidths.maxOrNull() ?: 0) + padX, widthSpec),
        resolveSize(rowHeights.sum() + paddingTop + paddingBottom, heightSpec),
      )
    }

    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
      val inner = width - paddingLeft - paddingRight
      var y = paddingTop
      var x = 0
      for (i in 0 until childCount) {
        val c = getChildAt(i)
        val row = rows[i]
        if (i == 0 || row != rows[i - 1]) {
          if (i > 0) y += rowHeights[row - 1]
          x = paddingLeft + (inner - rowWidths[row]) / 2
        }
        val top = y + (rowHeights[row] - c.measuredHeight) / 2
        c.layout(x, top, x + c.measuredWidth, top + c.measuredHeight)
        x += c.measuredWidth + gap
      }
    }

    override fun dispatchDraw(canvas: Canvas) {
      super.dispatchDraw(canvas)
      for (i in 1 until childCount) {
        if (rows[i] != rows[i - 1]) continue
        val left = getChildAt(i - 1).right + (gap - dp(1)) / 2f
        val mid = (getChildAt(i).top + getChildAt(i).bottom) / 2f
        canvas.drawRect(left, mid - dp(14), left + dp(1), mid + dp(14), rule)
      }
    }
  }

  private fun dp(v: Int) = (v * density).roundToInt()

  private companion object {
    const val STEP = 1.1f
    val ACCENT = 0xFF8BC34A.toInt()
  }
}

/**
 * Each item's row: the items in order, as many to a row as fit [room] with
 * [gap] between neighbours (one wider than [room] alone on its row).
 */
internal fun wrapRows(widths: IntArray, gap: Int, room: Int): IntArray {
  val rows = IntArray(widths.size)
  var row = 0
  var used = 0 // the row's width so far
  widths.forEachIndexed { i, w ->
    if (used > 0 && used + gap + w > room) {
      row++
      used = 0
    }
    used += if (used > 0) gap + w else w
    rows[i] = row
  }
  return rows
}
